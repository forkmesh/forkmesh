#!/usr/bin/env python3
"""Discovery and analytics: find public repositories and summarize the catalog.

Contract resources: GET /api/version (skew check), GET /api/repositories.
Filtering is client-side, as the contract requires.

    python3 discovery_catalog.py --base-url https://app.forkmesh.com --query mesh
"""

import argparse
from collections import Counter
import json
import sys

import _client

fm = _client.load()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default=fm.DEFAULT_BASE_URLS["hosted"])
    parser.add_argument("--query", default="", help="substring of owner/name/description")
    parser.add_argument("--live-only", action="store_true")
    args = parser.parse_args(argv)

    client = fm.Client(args.base_url)
    version = client.get_json("/api/version")
    skew = fm.skew_verdict(fm.load_contract(), version.get("version"))
    if skew["verdict"] == "unsupported":
        print("service %s is older than %s" % (skew["serviceVersion"], skew["minimum"]),
              file=sys.stderr)
        return fm.EXIT_SKEW

    catalog = client.get_json("/api/repositories")
    query = args.query.lower()
    matches = []
    visibility = Counter()
    live = 0
    for record in catalog.get("repositories") or []:
        owner, name = str(record.get("owner") or ""), str(record.get("name") or "")
        if not owner or not name:
            continue
        visibility[str(record.get("visibility") or "unknown")] += 1
        online = bool(record.get("cloneOnline") or record.get("liveHost"))
        live += online
        haystack = " ".join((owner, name, str(record.get("description") or ""))).lower()
        if query and query not in haystack:
            continue
        if args.live_only and not online:
            continue
        matches.append({
            "repository": "%s/%s" % (owner, name),
            "description": str(record.get("description") or ""),
            "cloneUrl": str(record.get("cloneUrl") or ""),
            "online": online,
        })
    fm.emit({
        "schema": fm.CLI_SCHEMA,
        "command": "example.discovery",
        "serviceVersion": skew["serviceVersion"],
        "repositories": sum(visibility.values()),
        "online": live,
        "byVisibility": dict(sorted(visibility.items())),
        "matches": matches,
    })
    return fm.EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
