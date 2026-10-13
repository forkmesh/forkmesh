#!/usr/bin/env python3
"""Explain a ForkMesh synchronization status from evidence or a mirrors payload.

Implements the forkmesh-sync-status-v1 model (www/docs/sync-status.md) by
reusing the Worker's reference implementation, so the CLI, the API, and the
shared fixture always agree.

  forkmesh_sync_status.py explain evidence.json          plain-language summary
  forkmesh_sync_status.py explain --json evidence.json   machine-readable status
  forkmesh_sync_status.py explain --mirrors payload.json from GET .../mirrors
  forkmesh_sync_status.py fixture                        verify the shared fixture

Exit codes: 0 current (verified or degraded), 3 not current, 1 fixture
mismatch, 2 usage or unreadable input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "conformance" / "sync-status" / "v1" / "fixture.json"
sys.path.insert(0, str(ROOT / "app" / "src"))

import sync_status  # noqa: E402

EXIT_CURRENT = 0
EXIT_MISMATCH = 1
EXIT_USAGE = 2
EXIT_NOT_CURRENT = 3


def _read_json(path):
    text = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
    return json.loads(text)


def _fixture_mismatches():
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    entries = [
        (case["id"], sync_status.evaluate(case["evidence"]), case["expect"])
        for case in fixture["cases"]
    ]
    for sequence in fixture["sequences"]:
        for index, step in enumerate(sequence["steps"]):
            entries.append((
                "%s#%d" % (sequence["id"], index),
                sync_status.evaluate(step["evidence"]),
                step["expect"],
            ))
    for case in fixture["mirrorsPayloadCases"]:
        entries.append((
            case["id"],
            sync_status.mirrors_payload_status(case["payload"]),
            case["expect"],
        ))
    return len(entries), [case_id for case_id, got, want in entries if got != want]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    explain = commands.add_parser("explain", help="explain one object's status")
    explain.add_argument("input", help="evidence JSON file, or - for stdin")
    explain.add_argument("--json", action="store_true", help="print the machine-readable status")
    explain.add_argument("--mirrors", action="store_true",
                         help="input is a GET /api/repo/<owner>/<repo>/mirrors payload")
    commands.add_parser("fixture", help="verify the shared conformance fixture")
    args = parser.parse_args(argv)

    if args.command == "fixture":
        total, mismatches = _fixture_mismatches()
        print(json.dumps({"cases": total, "mismatches": mismatches}, sort_keys=True))
        return EXIT_MISMATCH if mismatches else EXIT_CURRENT

    try:
        data = _read_json(args.input)
    except (OSError, ValueError) as exc:
        print("cannot read %s: %s" % (args.input, exc), file=sys.stderr)
        return EXIT_USAGE
    status = (
        sync_status.mirrors_payload_status(data)
        if args.mirrors
        else sync_status.evaluate(data)
    )
    if args.json:
        print(json.dumps(status, sort_keys=True))
    else:
        print("%s: %s" % (status["label"], status["summary"]))
    return EXIT_CURRENT if status["current"] else EXIT_NOT_CURRENT


if __name__ == "__main__":
    raise SystemExit(main())
