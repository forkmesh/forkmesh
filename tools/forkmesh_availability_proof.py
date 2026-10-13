#!/usr/bin/env python3
"""Reproduce a repository's availability proof and risk score.

Implements the forkmesh-availability-proof-v1 model
(www/docs/availability-proof.md) by reusing the Worker's reference
implementation, so the CLI, the API, and the shared fixture always agree.

  forkmesh_availability_proof.py explain payload.json          plain-language summary
  forkmesh_availability_proof.py explain --json payload.json   machine-readable report
  forkmesh_availability_proof.py verify payload.json           recompute and compare
                                                               the payload's `availability`
  forkmesh_availability_proof.py fixture                       verify the shared fixture

The input is a GET /api/repo/<owner>/<repo>/mirrors payload, or - for stdin.

Exit codes: 0 quorum proven (or verify matched), 3 below quorum, 1 fixture or
verify mismatch, 2 usage or unreadable input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "conformance" / "availability-proof" / "v1" / "fixture.json"
sys.path.insert(0, str(ROOT / "app" / "src"))

import availability_proof  # noqa: E402

EXIT_PROVEN = 0
EXIT_MISMATCH = 1
EXIT_USAGE = 2
EXIT_AT_RISK = 3


def _read_json(path):
    text = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
    return json.loads(text)


def _fixture_mismatches():
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    entries = [(case["id"], case) for case in fixture["cases"]]
    for sequence in fixture["sequences"]:
        for index, step in enumerate(sequence["steps"]):
            entries.append(("%s#%d" % (sequence["id"], index), step))
    mismatches = [
        case_id for case_id, entry in entries
        if availability_proof.evaluate_mirrors_payload(entry["payload"]) != entry["expect"]
    ]
    return len(entries), mismatches


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    explain = commands.add_parser("explain", help="explain one repository's proof")
    explain.add_argument("input", help="mirrors payload JSON file, or - for stdin")
    explain.add_argument("--json", action="store_true", help="print the machine-readable report")
    verify = commands.add_parser(
        "verify", help="recompute the report and compare it with the payload's availability")
    verify.add_argument("input", help="mirrors payload JSON file, or - for stdin")
    commands.add_parser("fixture", help="verify the shared conformance fixture")
    args = parser.parse_args(argv)

    if args.command == "fixture":
        total, mismatches = _fixture_mismatches()
        print(json.dumps({"cases": total, "mismatches": mismatches}, sort_keys=True))
        return EXIT_MISMATCH if mismatches else EXIT_PROVEN

    try:
        data = _read_json(args.input)
    except (OSError, ValueError) as exc:
        print("cannot read %s: %s" % (args.input, exc), file=sys.stderr)
        return EXIT_USAGE
    report = availability_proof.evaluate_mirrors_payload(data)
    if args.command == "verify":
        published = data.get("availability") if isinstance(data, dict) else None
        if published == report:
            print("reproduced: %s" % report["summary"])
            return EXIT_PROVEN
        print("mismatch: the published availability does not reproduce from the evidence",
              file=sys.stderr)
        return EXIT_MISMATCH
    if args.json:
        print(json.dumps(report, sort_keys=True))
    else:
        print("%s: %s" % (report["label"], report["summary"]))
    return EXIT_PROVEN if report["proof"] == "quorum" else EXIT_AT_RISK


if __name__ == "__main__":
    raise SystemExit(main())
