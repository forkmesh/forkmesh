#!/usr/bin/env python3
"""Deployment: block a rollout step until a self-hosted node is ready.

Contract resource: GET /readyz on the mirror-node loopback control API
(503 while not ready). Exits 0 once the node reports ready at the expected
version, 1 on timeout, and 3 when the node is older than the contract minimum.

    python3 deployment_gate.py --base-url http://127.0.0.1:8791 --expect-version 0.8.8
"""

import argparse
import sys
import time

import _client

fm = _client.load()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default=fm.DEFAULT_BASE_URLS["self-hosted"])
    parser.add_argument("--expect-version", default="")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--interval", type=float, default=fm.MIN_POLL_SECONDS["self-hosted"])
    args = parser.parse_args(argv)

    contract = fm.load_contract()
    client = fm.Client(args.base_url, policy=fm.RetryPolicy(max_attempts=2))
    deadline = time.monotonic() + args.timeout
    last = {}
    while True:
        try:
            result = client.request("GET", "/readyz")
            last = result.json if isinstance(result.json, dict) else {}
        except fm.TransportError as exc:
            last = {"error": str(exc)}
        version = last.get("version")
        skew = fm.skew_verdict(contract, version)
        if skew["verdict"] == "unsupported":
            print("node %s is older than %s" % (version, skew["minimum"]), file=sys.stderr)
            return fm.EXIT_SKEW
        ready = last.get("ready") is True and (
            not args.expect_version or version == args.expect_version)
        if ready or time.monotonic() >= deadline:
            fm.emit({"schema": fm.CLI_SCHEMA, "command": "example.deployment",
                     "ready": ready, "node": last.get("node"), "version": version,
                     "expectedVersion": args.expect_version or None})
            return fm.EXIT_OK if ready else fm.EXIT_FAILED
        time.sleep(min(args.interval, max(0.0, deadline - time.monotonic())))


if __name__ == "__main__":
    raise SystemExit(main())
