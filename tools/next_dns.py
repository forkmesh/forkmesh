#!/usr/bin/env python3
"""Cloudflare DNS for ForkMesh (app/world production names and next.forkmesh.com).

    next_dns.py status
    next_dns.py ensure        [--execute]
    next_dns.py health-once   [--execute]
    next_dns.py health-loop   [--execute]
    next_dns.py drain nN      [--execute]
    next_dns.py undrain nN    [--execute]

Dry-run is the default: without --execute changes are printed only.
docs/OPERATIONS.md, section "DNS health failover".
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import json
import stat
import sys
import time
import urllib.parse
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "app" / "host"))
import next_common as common  # noqa: E402
from forkmesh_host import dns_rules  # noqa: E402
from next_common import GuardError, ToolError  # noqa: E402

CLOUDFLARE_API = "https://api.cloudflare.com/client/v4"
TOKEN_FILE = "cloudflare-dns.env"
ZONE_APEX = dns_rules.ZONE_APEX
PRODUCTION_NAMES = frozenset({"app.forkmesh.com", "world.forkmesh.com"})
RECORD_TYPES = ("A", "AAAA")
TTL = dns_rules.TTL
FAILS_TO_REMOVE = dns_rules.FAILS_TO_REMOVE
HEALTHY_TO_READD = dns_rules.HEALTHY_TO_READD
MIN_CHANGE_INTERVAL = dns_rules.MIN_CHANGE_INTERVAL
PROBE_INTERVAL = dns_rules.PROBE_INTERVAL
COMMENT = "ForkMesh Next (tools/next_dns.py)"


def guard_name(name, record_type="A") -> str:
    if record_type not in RECORD_TYPES:
        raise GuardError("refusing DNS record type %r for %r: only %s"
                         % (record_type, name, ", ".join(RECORD_TYPES)))
    value = str(name or "").strip().lower()
    if value.endswith("."):
        value = value[:-1]
    if value in PRODUCTION_NAMES:
        return value
    try:
        return dns_rules.check_name(name)
    except dns_rules.DnsGuardError as error:
        raise GuardError("%s (allowed: %s, and names under %s)" % (
            error, ", ".join(sorted(PRODUCTION_NAMES)), ZONE_APEX))


def guard_record(record):
    record = record or {}
    return guard_name(record.get("name"), record.get("type"))


def guard_content(record_type, value, allowed):
    try:
        address = dns_rules.check_address(record_type, value)
    except dns_rules.DnsGuardError as error:
        raise GuardError(str(error))
    if allowed is not None and address not in allowed:
        raise GuardError("refusing %s content %s: not a core node address"
                         % (record_type, address))
    return address


def token_path() -> Path:
    return common.config_dir() / TOKEN_FILE


def check_token_file(path):
    try:
        mode = Path(path).stat().st_mode
    except FileNotFoundError:
        raise ToolError("missing %s (the DNS token file; docs/OPERATIONS.md)"
                        % path) from None
    if not stat.S_ISREG(mode):
        raise GuardError("%s is not a regular file" % path)
    if mode & 0o077:
        raise GuardError("%s is readable by group/other; chmod 0600 it" % path)


class Cloudflare:
    def __init__(self, http, zone_id, allowed=None):
        if not zone_id or not all(c in "0123456789abcdef" for c in zone_id):
            raise ToolError("CLOUDFLARE_ZONE_ID must be the 32-hex zone id")
        self.http = http
        self.zone = zone_id
        self.allowed = allowed

    @classmethod
    def from_config(cls, opener=None, allowed=None):
        check_token_file(token_path())
        token = common.credential(TOKEN_FILE, "CLOUDFLARE_API_TOKEN")
        zone = common.credential(TOKEN_FILE, "CLOUDFLARE_ZONE_ID")
        return cls(common.JsonHttp(CLOUDFLARE_API, token, opener), zone,
                   allowed)

    def records(self, name, record_type="A"):
        name = guard_name(name, record_type)
        query = urllib.parse.urlencode({"type": record_type, "name": name,
                                        "per_page": 100})
        result = self.http.request(
            "GET", "/zones/%s/dns_records?%s" % (self.zone, query))
        return [r for r in result.get("result") or []
                if str(r.get("name", "")).lower() == name
                and r.get("type", record_type) == record_type]

    def record(self, record_id):
        return self.http.request("GET", "/zones/%s/dns_records/%s" % (
            self.zone, record_id))["result"]

    def body(self, name, content, record_type):
        return {"type": record_type, "name": guard_name(name, record_type),
                "content": guard_content(record_type, content, self.allowed),
                "ttl": TTL, "proxied": False}

    def create(self, name, content, record_type="A"):
        body = dict(self.body(name, content, record_type), comment=COMMENT)
        return self.http.request("POST", "/zones/%s/dns_records" % self.zone,
                                 body)

    def update(self, record_id, name, content, record_type="A"):
        body = self.body(name, content, record_type)
        live = self.record(record_id)
        if guard_record(live) != body["name"] or live.get("type") != record_type:
            raise GuardError("refusing update of record %s: it is %s %s" % (
                record_id, live.get("type"), live.get("name")))
        return self.http.request("PUT", "/zones/%s/dns_records/%s" % (
            self.zone, record_id), body)

    def delete(self, record_id):
        guard_record(self.record(record_id))
        return self.http.request("DELETE", "/zones/%s/dns_records/%s" % (
            self.zone, record_id))


_REPLICA_CACHE = {"nodes": [], "draining": set(), "at": 0.0}
REPLICA_REFRESH_SECONDS = 60


def cluster_replicas(fetch=None, cache=_REPLICA_CACHE, now=None):
    import ipaddress
    import re
    now = time.time() if now is None else now
    if fetch is None and now - cache.get("at", 0.0) < REPLICA_REFRESH_SECONDS:
        return cache["nodes"], cache["draining"]
    cache["at"] = now
    try:
        if fetch is None:
            import next_autoscale
            fetch = next_autoscale.fetch_status
        document = fetch()
    except Exception as error:
        print("next_dns: autoscale status unavailable (%s); keeping %d replica(s)"
              % (str(error)[:120], len(cache["nodes"])), file=sys.stderr)
        return cache["nodes"], cache["draining"]
    nodes, draining = [], set()
    for item in ((document.get("pools") or {}).get("app") or {}).get("nodes") or []:
        name = str(item.get("name") or "")
        if not re.fullmatch(r"a[1-9]", name) or item.get("state") not in (
                "serving", "draining"):
            continue
        try:
            ip = str(ipaddress.IPv4Address(str(item.get("ip") or "")))
        except ValueError:
            continue
        nodes.append({"name": name, "public_ipv4": ip, "role": "app",
                      "wg_ip": "10.77.0.%d" % (20 + int(name[1:]))})
        if item["state"] == "draining":
            draining.add(name)
    cache["nodes"], cache["draining"] = nodes, draining
    return nodes, draining


def dns_nodes(fleet):
    return list(fleet.get("nodes") or []) + list(fleet.get("app_nodes") or [])


def core_addresses(fleet):
    out = set()
    for node in dns_nodes(fleet):
        out.add(node["public_ipv4"])
        if node.get("public_ipv6"):
            out.add(node["public_ipv6"])
    return out


def served_names(fleet):
    import render
    names = []
    for name in render.served_hosts(fleet) + [fleet["app_host"],
                                              fleet["world_host"]]:
        name = guard_name(name)
        if name not in names:
            names.append(name)
    return names


shared_names = served_names


def probe_host(fleet):
    import render
    return render.served_app_host(fleet)


def node_names(fleet):
    return {guard_name("%s.%s" % (n["name"], fleet["domain"])): n
            for n in fleet.get("nodes") or []}


def addresses(nodes, record_type):
    key = "public_ipv4" if record_type == "A" else "public_ipv6"
    return [n[key] for n in nodes if n.get(key)]


def sync_name(cf, runner, name, wanted_ips, record_type="A"):
    name = guard_name(name, record_type)
    try:
        plan = dns_rules.plan_set(name, cf.records(name, record_type),
                                  wanted_ips, record_type=record_type,
                                  name_check=lambda n: guard_name(n, record_type))
    except dns_rules.DnsGuardError as error:
        raise GuardError(str(error))
    changes = []
    for action, ip, record in plan:
        changes.append((action, name, ip))
        if action == "create":
            runner.step("create %s %s -> %s (TTL %d, DNS only)" % (
                record_type, name, ip, TTL),
                lambda ip=ip: cf.create(name, ip, record_type))
        elif action == "update":
            runner.step("update %s %s -> %s to TTL %d, DNS only" % (
                record_type, name, ip, TTL),
                lambda record=record, ip=ip: cf.update(record["id"], name, ip,
                                                       record_type))
        else:
            runner.step("delete %s %s -> %s" % (record_type, name, ip),
                        lambda record=record: cf.delete(record["id"]))
    return changes


def sync_members(fleet, cf, runner, members):
    changes = []
    for record_type in RECORD_TYPES:
        wanted = addresses(members, record_type)
        if not wanted:
            continue
        for name in served_names(fleet):
            changes += sync_name(cf, runner, name, wanted, record_type)
    return changes


def ensure(fleet, cf, runner, healthy_ips=None):
    nodes = list(fleet.get("nodes") or [])
    if healthy_ips is None:
        with locked(state_path()):
            known = load_state(fleet, state_path())
        held = sorted(n["name"] for n in nodes
                      if (known.nodes.get(n["name"]) or {}).get("held"))
        if held:
            runner.say("held out of DNS (not added): %s" % ", ".join(held))
        nodes = [n for n in nodes if n["name"] not in held]
        nodes += [n for n in fleet.get("app_nodes") or []
                  if (known.nodes.get(n["name"]) or {}).get("in_dns")]
    if healthy_ips is not None:
        nodes = [n for n in nodes if n["public_ipv4"] in set(healthy_ips)]
    changes = sync_members(fleet, cf, runner, nodes)
    for name, node in node_names(fleet).items():
        for record_type in RECORD_TYPES:
            wanted = addresses([node], record_type)
            if wanted:
                changes += sync_name(cf, runner, name, wanted, record_type)
    return changes


def ensure_node_name(fleet, cf, runner, name):
    changes = []
    for host, node in node_names(fleet).items():
        if node["name"] != name:
            continue
        for record_type in RECORD_TYPES:
            wanted = addresses([node], record_type)
            if wanted:
                changes += sync_name(cf, runner, host, wanted, record_type)
    return changes


def hold_joining(fleet, runner, name, path=None):
    path = path or state_path()
    node = next((n for n in fleet.get("nodes") or [] if n["name"] == name), None)
    ip = node["public_ipv4"] if node else ""
    with locked(path):
        data = common.load_json(path, {}) or {}
        nodes = data.setdefault("nodes", {})
        entry = nodes.get(name)
        if entry and entry.get("in_dns"):
            return entry
        wanted = dict(entry or {}, ip=ip or (entry or {}).get("ip", ""),
                      in_dns=False, held=True, joining=True,
                      fails=int((entry or {}).get("fails") or 0),
                      healthy_since=(entry or {}).get("healthy_since"))
        if entry == wanted:
            return entry
        if not runner.execute:
            runner.say("[dry-run] %s would be held out of DNS while it joins" % name)
            return wanted
        nodes[name] = wanted
        common.save_json(path, data)
        runner.say("%s held out of DNS in %s while it joins" % (name, path))
        return wanted


def node_entry(name, path=None):
    data = common.load_json(path or state_path(), {}) or {}
    return dict((data.get("nodes") or {}).get(name) or {})


probe = dns_rules.probe


class HealthState:
    def __init__(self, data=None):
        data = data or {}
        self.nodes = data.get("nodes") or {}
        last = data.get("last_change")
        self.last_change = None if last is None else float(last)

    def to_json(self):
        return {"nodes": self.nodes, "last_change": self.last_change}

    def node(self, name, ip, in_dns=True):
        entry = self.nodes.setdefault(name, {"ip": ip, "in_dns": in_dns,
                                             "fails": 0, "healthy_since": None})
        entry["ip"] = ip
        return entry

    def observe(self, name, ip, ok, now):
        entry = self.node(name, ip)
        if ok:
            entry["fails"] = 0
            if entry.get("healthy_since") is None:
                entry["healthy_since"] = now
        else:
            entry["fails"] = int(entry.get("fails") or 0) + 1
            entry["healthy_since"] = None

    def decide(self, now):
        if self.last_change is not None and now - self.last_change < MIN_CHANGE_INTERVAL:
            return None
        members = [n for n, e in self.nodes.items() if e.get("in_dns")]
        for name in sorted(self.nodes):
            entry = self.nodes[name]
            if entry.get("in_dns") and (entry.get("fails", 0) >= FAILS_TO_REMOVE
                                        or entry.get("held")):
                if len(members) <= 1:
                    continue
                return ("remove", name)
        for name in sorted(self.nodes):
            entry = self.nodes[name]
            since = entry.get("healthy_since")
            if not entry.get("in_dns") and not entry.get("held") \
                    and since is not None and now - since >= HEALTHY_TO_READD:
                return ("add", name)
        return None

    def apply(self, decision, now):
        action, name = decision
        self.nodes[name]["in_dns"] = action == "add"
        self.last_change = now

    def member_names(self):
        return sorted(n for n, e in self.nodes.items() if e.get("in_dns"))

    def members(self):
        return sorted(e["ip"] for e in self.nodes.values() if e.get("in_dns"))


def state_path():
    return common.config_dir() / "dns-health.json"


@contextlib.contextmanager
def locked(path):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path.with_name(path.name + ".lock"), "a") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def load_state(fleet, path):
    state = HealthState(common.load_json(path, {}))
    known = {n["name"] for n in dns_nodes(fleet)}
    state.vanished = []
    for name in list(state.nodes):
        if name not in known and not state.nodes[name].get("joining"):
            if state.nodes[name].get("in_dns"):
                state.vanished.append(name)
            del state.nodes[name]
    joining = any(item.get("in_dns") for item in state.nodes.values())
    for node in fleet.get("nodes") or []:
        if joining and node["name"] not in state.nodes:
            state.nodes[node["name"]] = {"ip": node["public_ipv4"], "in_dns": False,
                                         "held": True, "fails": 0,
                                         "healthy_since": None, "joining": True}
        state.node(node["name"], node["public_ipv4"])
    for node in fleet.get("app_nodes") or []:
        state.node(node["name"], node["public_ipv4"], in_dns=False)
    return state


def member_nodes(fleet, names):
    return [n for n in dns_nodes(fleet) if n["name"] in set(names)]


def change(fleet, cf, runner, state, decision, now):
    action, name = decision
    trial = HealthState(json.loads(json.dumps(state.to_json())))
    trial.apply(decision, now)
    runner.say("health: %s %s (set %s -> %s)" % (
        action, name, ",".join(state.member_names()),
        ",".join(trial.member_names())))
    if not runner.execute:
        runner.say("[dry-run] DNS not changed; state not advanced")
        return False
    sync_members(fleet, cf, runner, member_nodes(fleet, trial.member_names()))
    state.apply(decision, now)
    return True


def health_round(fleet, cf, runner, prober=probe, now=None, path=None):
    now = time.time() if now is None else now
    path = path or state_path()
    host = probe_host(fleet)
    with locked(path):
        state = load_state(fleet, path)
        if getattr(state, "vanished", None) and runner.execute:
            runner.say("health: %s left the fleet while in DNS; resyncing"
                       % ",".join(state.vanished))
            sync_members(fleet, cf, runner, member_nodes(fleet, state.member_names()))
        for name in fleet.get("draining_app_nodes") or ():
            entry = state.nodes.get(name)
            if entry is None:
                continue
            entry["held"] = True
            if entry.get("in_dns") and len(state.member_names()) > 1:
                change(fleet, cf, runner, state, ("remove", name), now)
        probed = []
        for node in dns_nodes(fleet):
            ok = prober(node["public_ipv4"], host)
            extra = ""
            if node.get("public_ipv6"):
                extra = " v6 %s" % ("ok" if prober(node["public_ipv6"], host)
                                    else "FAIL")
            probed.append((node, ok, extra))
        offline = len(probed) > 1 and not any(ok for _node, ok, _extra in probed)
        for node, ok, extra in probed:
            if not offline:
                state.observe(node["name"], node["public_ipv4"], ok, now)
            entry = state.node(node["name"], node["public_ipv4"])
            runner.say("%s %s %s (fails %d)%s%s" % (
                node["name"], node["public_ipv4"], "ok" if ok else "FAIL",
                entry["fails"], extra, " held" if entry.get("held") else ""))
        if offline:
            runner.say("health: no core answered; this box looks offline, so no "
                       "node is counted as failed and DNS is left alone")
            common.save_json(path, state.to_json())
            return None
        decision = state.decide(now)
        if decision:
            change(fleet, cf, runner, state, decision, now)
        common.save_json(path, state.to_json())
    return decision


UNDRAIN_PROBES = 3
UNDRAIN_PROBE_GAP = 2.0


def healthy_now(fleet, name, prober=probe, sleep=time.sleep):
    node = next((n for n in dns_nodes(fleet) if n["name"] == name), None)
    if node is None:
        return False
    host = probe_host(fleet)
    for attempt in range(UNDRAIN_PROBES):
        if attempt:
            sleep(UNDRAIN_PROBE_GAP)
        if not prober(node["public_ipv4"], host):
            return False
        if node.get("public_ipv6") and not prober(node["public_ipv6"], host):
            return False
    return True


def hold(fleet, cf, runner, name, held, now=None, path=None, prober=probe,
         sleep=time.sleep):
    now = time.time() if now is None else now
    path = path or state_path()
    known = {n["name"] for n in dns_nodes(fleet)}
    if name not in known:
        raise GuardError("no core or app node named %r" % name)
    with locked(path):
        state = load_state(fleet, path)
        entry = state.nodes[name]
        decision = None
        if held and entry.get("in_dns"):
            if len(state.member_names()) <= 1:
                raise GuardError("refusing to drain %s: it is the last node "
                                 "in DNS" % name)
            decision = ("remove", name)
        if decision:
            if not change(fleet, cf, runner, state, decision, now):
                return None
        elif not runner.execute:
            runner.say("[dry-run] %s would be %s" % (
                name, "held out of DNS" if held else "released"))
            return None
        entry["held"] = bool(held)
        if not held:
            entry.pop("joining", None)
        if not held and not entry.get("in_dns") and runner.execute \
                and healthy_now(fleet, name, prober, sleep):
            previous = state.last_change
            decision = ("add", name)
            if change(fleet, cf, runner, state, decision, now):
                state.last_change = previous
                entry["healthy_since"] = now
                runner.say("%s passed %d health probes and is back in DNS" % (
                    name, UNDRAIN_PROBES))
            else:
                decision = None
        elif not held:
            runner.say("%s released: the health loop adds it back after %d s "
                       "healthy, at most one change per %d s" % (
                           name, HEALTHY_TO_READD, MIN_CHANGE_INTERVAL))
        common.save_json(path, state.to_json())
        return decision


def status(fleet, cf, out=None):
    out = out or sys.stdout
    for name in served_names(fleet) + sorted(node_names(fleet)):
        for record_type in RECORD_TYPES:
            ips = sorted(r.get("content") for r in cf.records(name, record_type))
            print("%-28s %-4s %s" % (name, record_type, ", ".join(ips) or "(none)"),
                  file=out)


def main(argv=None, cf=None, prober=probe, sleep=time.sleep) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--fleet")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("status")
    for name in ("ensure", "health-once", "health-loop"):
        p = sub.add_parser(name)
        common.add_execute_flags(p)
    for name in ("drain", "undrain"):
        p = sub.add_parser(name)
        p.add_argument("node")
        common.add_execute_flags(p)
    args = parser.parse_args(argv)
    try:
        runner = common.runner_from_args(args)
        fleet = common.load_fleet(args.fleet)
        cf = cf or Cloudflare.from_config(allowed=core_addresses(fleet))
        if args.command == "status":
            status(fleet, cf)
        elif args.command == "ensure":
            changes = ensure(fleet, cf, runner)
            runner.say("%d change(s)%s" % (len(changes), "" if runner.execute
                                             else " planned (dry run)"))
        elif args.command == "health-once":
            health_round(fleet, cf, runner, prober)
        elif args.command in ("drain", "undrain"):
            hold(fleet, cf, runner, args.node, args.command == "drain",
                 prober=prober, sleep=sleep)
        else:
            while True:
                try:
                    fleet = common.load_fleet(args.fleet)
                    replicas, draining = cluster_replicas()
                    fleet["app_nodes"] = [n for n in replicas if n["name"] not in {
                        m["name"] for m in fleet.get("app_nodes") or []}] + list(
                        fleet.get("app_nodes") or [])
                    fleet["draining_app_nodes"] = sorted(draining)
                    cf.allowed = core_addresses(fleet)
                    health_round(fleet, cf, runner, prober)
                except ToolError as error:
                    if isinstance(error, GuardError):
                        raise
                    print("next_dns: round failed: %s" % error, file=sys.stderr)
                sleep(PROBE_INTERVAL)
    except (ToolError, OSError, KeyError) as error:
        print("next_dns: %s" % error, file=sys.stderr)
        return 2 if isinstance(error, GuardError) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
