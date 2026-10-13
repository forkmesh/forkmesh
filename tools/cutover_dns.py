#!/usr/bin/env python3
"""The production cutover's DNS flip (docs/CUTOVER.md, section "DNS")."""

from __future__ import annotations

import argparse
import datetime as dt
import ipaddress
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "deploy" / "core"))

CLOUDFLARE_API = "https://api.cloudflare.com/client/v4"
FLIP = {"app.forkmesh.com": "forkmesh-app",
        "world.forkmesh.com": "forkmesh-world"}
TTL = 60
STATE_NAME = "cutover-dns-state.json"


class DnsError(RuntimeError):
    pass


class GuardError(DnsError):
    pass


def log(message):
    print(message, flush=True)


def guard_host(name):
    clean = str(name or "").strip().lower().rstrip(".")
    if clean not in FLIP:
        raise GuardError("refusing %r: cutover_dns only touches %s"
                         % (name, ", ".join(FLIP)))
    return clean


def guard_ip(ip, allowed):
    address = str(ipaddress.IPv4Address(str(ip)))
    if address not in allowed:
        raise GuardError("refusing A content %s: not a core node address" % address)
    return address


class Cloudflare:
    def __init__(self, token, account, zone, opener=None):
        for label, value in (("CLOUDFLARE_ACCOUNT_ID", account),
                             ("CLOUDFLARE_ZONE_ID", zone)):
            if not value or not all(c in "0123456789abcdef" for c in value):
                raise DnsError("%s must be 32 hex" % label)
        self.token, self.account, self.zone = token, account, zone
        self.opener = opener or urllib.request.urlopen

    @classmethod
    def from_config(cls):
        import next_common as common
        return cls(common.credential("cloudflare.env", "CLOUDFLARE_API_TOKEN"),
                   common.credential("cloudflare.env", "CLOUDFLARE_ACCOUNT_ID"),
                   common.credential("cloudflare.env", "CLOUDFLARE_ZONE_ID"))

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(CLOUDFLARE_API + path, data=data,
                                         method=method, headers={
                                             "Authorization": "Bearer " + self.token,
                                             "Content-Type": "application/json"})
        try:
            with self.opener(request, timeout=30) as response:
                payload = json.loads(response.read() or b"{}")
        except urllib.error.HTTPError as error:
            try:
                codes = [e.get("code") for e in json.loads(error.read()).get("errors") or []]
            except ValueError:
                codes = []
            raise DnsError("Cloudflare %s %s: HTTP %d %s" % (
                method, path.split("?")[0], error.code, codes)) from None
        if not payload and method == "DELETE":
            return {}
        if not payload.get("success", False):
            raise DnsError("Cloudflare %s %s failed: %s" % (
                method, path.split("?")[0],
                [e.get("code") for e in payload.get("errors") or []]))
        return payload.get("result")

    def custom_domains(self, host):
        host = guard_host(host)
        query = urllib.parse.urlencode({"hostname": host})
        return [d for d in self.call("GET", "/accounts/%s/workers/domains?%s" % (
            self.account, query)) or [] if d.get("hostname") == host]

    def detach(self, domain_id, host, service):
        live = [d for d in self.custom_domains(host) if d.get("id") == domain_id]
        if not live or live[0].get("service") != service:
            raise GuardError("custom domain %s of %s is not on %s any more"
                             % (domain_id, host, service))
        return self.call("DELETE", "/accounts/%s/workers/domains/%s" % (
            self.account, domain_id))

    def attach(self, host, service, environment="production"):
        host = guard_host(host)
        if FLIP[host] != service:
            raise GuardError("%s belongs to %s, not %s" % (host, FLIP[host], service))
        return self.call("PUT", "/accounts/%s/workers/domains" % self.account, {
            "hostname": host, "service": service, "environment": environment,
            "zone_id": self.zone})

    def records(self, host, record_type=None):
        host = guard_host(host)
        query = {"name": host}
        if record_type:
            query["type"] = record_type
        return self.call("GET", "/zones/%s/dns_records?%s" % (
            self.zone, urllib.parse.urlencode(query))) or []

    def create_a(self, host, ip, allowed):
        return self.call("POST", "/zones/%s/dns_records" % self.zone, {
            "type": "A", "name": guard_host(host), "content": guard_ip(ip, allowed),
            "ttl": TTL, "proxied": False,
            "comment": "ForkMesh Next core (cutover_dns.py)"})

    def delete_a(self, record_id, allowed):
        live = self.call("GET", "/zones/%s/dns_records/%s" % (self.zone, record_id))
        if live.get("type") != "A":
            raise GuardError("record %s is not an A record" % record_id)
        guard_host(live.get("name"))
        guard_ip(live.get("content"), allowed)
        return self.call("DELETE", "/zones/%s/dns_records/%s" % (self.zone, record_id))

    def dns_settings(self):
        return self.call("GET", "/zones/%s/dns_settings" % self.zone) or {}

    def set_soa_min_ttl(self, seconds):
        soa = dict((self.dns_settings().get("soa") or {}))
        if not soa:
            raise DnsError("zone dns_settings has no soa object")
        soa["min_ttl"] = int(seconds)
        return self.call("PATCH", "/zones/%s/dns_settings" % self.zone, {"soa": soa})


def state_path():
    import next_common as common
    return common.config_dir() / STATE_NAME


def load_state(path=None):
    path = Path(path or state_path())
    if not path.is_file():
        return {}
    return json.loads(path.read_text("utf-8"))


def save_state(state, path=None):
    import next_common as common
    common.save_json(Path(path or state_path()), state)


def core_ips(fleet):
    return [str(ipaddress.IPv4Address(n["public_ipv4"])) for n in fleet["nodes"]]


def plan(cf, fleet, out=log):
    ips = core_ips(fleet)
    report = {}
    for host, service in FLIP.items():
        domains = cf.custom_domains(host)
        records = [r for r in cf.records(host) if r.get("type") in ("A", "AAAA", "CNAME")]
        report[host] = {"domains": domains, "records": records}
        for d in domains:
            out("%s: custom domain on %s (%s)" % (host, d.get("service"), d.get("environment")))
        for r in records:
            out("%s: %s %s proxied=%s ttl=%s%s" % (
                host, r["type"], r["content"], r.get("proxied"), r.get("ttl"),
                " (managed by the Worker)" if (r.get("meta") or {}).get("read_only") else ""))
        a_now = sorted(r["content"] for r in records if r["type"] == "A")
        if a_now == sorted(ips) and not domains:
            out("%s: already flipped to the cores" % host)
        else:
            out("%s: flip -> detach the %s custom domain, then A %s (TTL %d, DNS only)"
                % (host, service, " ".join(ips), TTL))
    soa = (cf.dns_settings().get("soa") or {})
    if soa:
        out("zone SOA min_ttl %s s (negative caching of the flip's sub-second gap)"
            % soa.get("min_ttl"))
    return report


def check_flippable(report, ips):
    for host, service in FLIP.items():
        domains = report[host]["domains"]
        records = report[host]["records"]
        others = [r for r in records if not (r.get("meta") or {}).get("read_only")]
        if others and sorted(r["content"] for r in others if r["type"] == "A") != sorted(ips):
            raise GuardError("%s has DNS records the flip does not expect; look first"
                             % host)
        if len(domains) > 1 or (domains and domains[0].get("service") != service):
            raise GuardError("%s: expected one custom domain on %s" % (host, service))


def flip(cf, fleet, execute=False, state_file=None, now=None):
    ips = core_ips(fleet)
    report = plan(cf, fleet)
    check_flippable(report, ips)
    if not execute:
        log("dry-run: pass --execute")
        return None
    state = load_state(state_file)
    state.setdefault("flipped", {})
    for host, service in FLIP.items():
        domains = report[host]["domains"]
        entry = state["flipped"].setdefault(host, {"records": []})
        existing = {r["content"] for r in cf.records(host, "A")}
        if domains:
            domain = domains[0]
            entry["domain"] = {"id": domain["id"], "service": domain["service"],
                               "environment": domain.get("environment") or "production"}
            save_state(state, state_file)
            cf.detach(domain["id"], host, service)
        for ip in ips:
            if ip in existing:
                continue
            created = cf.create_a(host, ip, ips)
            entry["records"].append(created["id"])
        entry["at"] = (now or dt.datetime.now(dt.timezone.utc)).isoformat()
        save_state(state, state_file)
        log("%s: flipped to %s" % (host, " ".join(ips)))
    return state


def rollback(cf, fleet, execute=False, state_file=None):
    ips = core_ips(fleet)
    state = load_state(state_file)
    flipped = state.get("flipped") or {}
    if not flipped:
        raise DnsError("no flip recorded in %s" % (state_file or state_path()))
    for host in FLIP:
        entry = flipped.get(host)
        if not entry:
            continue
        log("%s: delete A %d record(s), re-attach the custom domain on %s" % (
            host, len(entry.get("records") or []),
            (entry.get("domain") or {}).get("service", FLIP[host])))
        if not execute:
            continue
        for record_id in list(entry.get("records") or []):
            cf.delete_a(record_id, ips)
            entry["records"].remove(record_id)
            save_state(state, state_file)
        domain = entry.get("domain") or {"service": FLIP[host],
                                         "environment": "production"}
        cf.attach(host, domain["service"], domain.get("environment", "production"))
        flipped.pop(host)
        save_state(state, state_file)
        log("%s: back on the %s Worker" % (host, domain["service"]))
    if not execute:
        log("dry-run: pass --execute")


def main(argv=None, cf=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("plan")
    for name in ("flip", "rollback"):
        p = sub.add_parser(name)
        p.add_argument("--execute", action="store_true")
    p = sub.add_parser("soa-min-ttl")
    p.add_argument("seconds", type=int)
    p.add_argument("--execute", action="store_true")
    parser.add_argument("--fleet", default=str(REPO / "deploy" / "fleet.toml"))
    parser.add_argument("--state", default=None)
    args = parser.parse_args(argv)
    try:
        import render
        fleet = render.load_fleet(args.fleet)
        cf = cf or Cloudflare.from_config()
        if args.command == "plan":
            plan(cf, fleet)
        elif args.command == "flip":
            flip(cf, fleet, args.execute, args.state)
        elif args.command == "rollback":
            rollback(cf, fleet, args.execute, args.state)
        elif args.command == "soa-min-ttl":
            if not 30 <= args.seconds <= 86400:
                raise GuardError("SOA min_ttl must be 30..86400")
            current = (cf.dns_settings().get("soa") or {}).get("min_ttl")
            log("zone SOA min_ttl %s -> %d" % (current, args.seconds))
            if args.execute:
                cf.set_soa_min_ttl(args.seconds)
            else:
                log("dry-run: pass --execute")
        return 0
    except DnsError as error:
        print("cutover_dns: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
