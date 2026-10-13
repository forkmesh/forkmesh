#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import ipaddress
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

NAME_RE = re.compile(r"^a([1-9])$")
KEY_RE = re.compile(r"^[A-Za-z0-9+/]{42}[AEIMQUYcgkosw048]=$")
REPLICA_NET_RE = re.compile(r"^10\.77\.0\.2[1-9]/32$")
STATES = ("joining", "serving", "draining", "removing")
QUERY = ("SELECT name, wg_ip, wg_pubkey, public_ipv4, state, created_by "
         "FROM autoscale_nodes WHERE pool='app'")
INTERVAL = 15


def curl_credentials(path):
    text = Path(path).read_text("utf-8")
    match = re.search(r'user\s*=\s*"([^":]+):([^"]+)"', text)
    if not match:
        raise RuntimeError("no user in %s" % path)
    return match.group(1), match.group(2)


def fetch_rows(url, credentials, opener=urllib.request.urlopen):
    request = urllib.request.Request(
        url.rstrip("/") + "/db/query?associative&level=weak&q=" +
        urllib.parse.quote(QUERY))
    token = base64.b64encode(("%s:%s" % credentials).encode()).decode()
    request.add_header("Authorization", "Basic " + token)
    with opener(request, timeout=10) as response:
        body = json.loads(response.read(1 << 20))
    result = (body.get("results") or [{}])[0]
    if result.get("error"):
        raise RuntimeError("rqlite: %s" % result["error"][:200])
    return result.get("rows") or []


def wanted(rows, core_keys=()):
    out = {}
    seen_ips = set()
    for row in rows or []:
        name = str(row.get("name") or "")
        match = NAME_RE.match(name)
        if not match or row.get("created_by") != "autoscaler":
            continue
        if row.get("state") not in STATES:
            continue
        wg_ip = str(row.get("wg_ip") or "")
        if wg_ip != "10.77.0.%d" % (20 + int(match.group(1))):
            continue
        key = str(row.get("wg_pubkey") or "")
        if not KEY_RE.match(key) or key in core_keys or key in out:
            continue
        try:
            public = ipaddress.IPv4Address(str(row.get("public_ipv4") or ""))
        except ValueError:
            continue
        if not public.is_global or wg_ip in seen_ips:
            continue
        seen_ips.add(wg_ip)
        out[key] = {"name": name, "wg_ip": wg_ip, "endpoint": str(public)}
    return out


def parse_dump(text):
    peers = {}
    for line in (text or "").splitlines()[1:]:
        fields = line.split("\t")
        if len(fields) >= 4:
            peers[fields[0]] = fields[3]
    return peers


def plan(current, desired, port):
    commands = []
    managed = {k: v for k, v in current.items() if REPLICA_NET_RE.match(v or "")}
    core_keys = {k for k, v in current.items() if not REPLICA_NET_RE.match(v or "")}
    for key in sorted(managed):
        if key not in desired:
            commands.append(["wg", "set", "wg0", "peer", key, "remove"])
    for key, item in sorted(desired.items()):
        if key in core_keys:
            continue
        if managed.get(key) == item["wg_ip"] + "/32" and key in current:
            continue
        commands.append(["wg", "set", "wg0", "peer", key, "allowed-ips",
                         item["wg_ip"] + "/32", "endpoint",
                         "%s:%d" % (item["endpoint"], port),
                         "persistent-keepalive", "25"])
    return commands


def nft_script(ips):
    lines = ["flush set inet filter autoscaled"]
    if ips:
        lines.append("add element inet filter autoscaled { %s }" % ", ".join(sorted(ips)))
    return "\n".join(lines) + "\n"


def run(command, stdin=None, check=True):
    return subprocess.run(command, input=stdin, capture_output=True, text=True,
                          check=check)


def round_once(config, runner=run, fetch=fetch_rows):
    current = parse_dump(runner(["wg", "show", "wg0", "dump"]).stdout)
    core_keys = {k for k, v in current.items() if not REPLICA_NET_RE.match(v or "")}
    rows = fetch(config["rqlite_url"], curl_credentials(config["curlrc"]))
    desired = wanted(rows, core_keys)
    for command in plan(current, desired, int(config.get("wg_port", 51820))):
        runner(command)
    ips = sorted(item["wg_ip"] for item in desired.values())
    runner(["nft", "-f", "-"], stdin=nft_script(ips))
    out = Path(config["replicas_file"])
    out.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
    temporary = out.with_name(out.name + ".tmp")
    temporary.write_text(json.dumps({"ips": ips, "at": int(time.time())}) + "\n")
    os.chmod(temporary, 0o644)
    os.replace(temporary, out)
    return {"peers": [item["name"] for item in desired.values()], "ips": ips}


def main(argv=None):
    parser = argparse.ArgumentParser(prog="fmnext-fleet-peers")
    parser.add_argument("--config", default="/etc/forkmesh-next/fleet-peers.json")
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args(argv)
    config = json.loads(Path(args.config).read_text("utf-8"))
    last = None
    while True:
        try:
            report = round_once(config)
            if report != last:
                print("fleet-peers: replicas %s" % (",".join(report["peers"]) or "none"),
                      flush=True)
                last = report
        except Exception as error:
            print("fleet-peers: round failed: %s" % str(error)[:200], flush=True)
        if args.once:
            return 0
        time.sleep(INTERVAL)


if __name__ == "__main__":
    sys.exit(main())
