#!/usr/bin/env python3
"""Verify and fold a canonical ForkMesh collaboration NDJSON event log."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app" / "src"))

from collaboration import CollaborationError, SignedEventLog, canonical_json  # noqa: E402


def _read(path: str) -> bytes:
    return sys.stdin.buffer.read() if path == "-" else Path(path).read_bytes()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "policy_path",
        nargs="?",
        metavar="POLICY",
        help="repository authority policy JSON (or pass --policy)",
    )
    parser.add_argument("log", metavar="LOG", help="canonical NDJSON event log, or - for stdin")
    parser.add_argument("--policy", help="repository authority policy JSON")
    parser.add_argument(
        "--require-complete",
        action="store_true",
        help="exit 3 when a signed event is pending or any input is rejected",
    )
    parser.add_argument(
        "--state-only",
        action="store_true",
        help="emit only materialized state rather than audit and telemetry",
    )
    return parser


def main() -> int:
    parser = _parser()
    args = parser.parse_args()
    if (args.policy is None) == (args.policy_path is None):
        parser.error("give the policy once: POLICY LOG, or --policy POLICY LOG")
    try:
        policy_path = args.policy or args.policy_path
        policy = json.loads(Path(policy_path).read_text(encoding="utf-8"))
        log = SignedEventLog.from_ndjson(_read(args.log))
        result = log.fold(policy)
    except (OSError, json.JSONDecodeError, CollaborationError) as exc:
        print(f"collaboration verification failed: {exc}", file=sys.stderr)
        return 2

    output = result.state if args.state_only else {
        "state": result.state,
        "stateDigest": result.state_digest,
        "audit": result.audit,
        "telemetry": result.telemetry,
    }
    # Canonical output can itself be hashed and compared by independent nodes.
    sys.stdout.buffer.write(canonical_json(output) + b"\n")
    if args.require_complete and (result.audit["rejected"] or result.audit["pending"]):
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
