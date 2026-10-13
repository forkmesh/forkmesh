#!/usr/bin/env python3
"""Verify and resolve ForkMesh trust lists offline (issue #514).

``resolve`` reads a bundle of raw signed documents plus local follows and
overrides and prints the resolution defined in
www/docs/design/scoped-trust-lists.md. ``verify`` checks each document on
its own. ``drill`` replays the compromised-list drill from the shared vectors
and fails unless the list flips from ``active`` to ``recovered`` with every
malicious assertion gone.

Bundle format (JSON)::

    {"documents": ["<raw document>", ...],
     "follows": [{"issuer": "...", "name": "...", "roles": null, "communities": null}],
     "overrides": [{"subject": "...", "role": "...", "scope": "...", "stance": "ignore"}]}

Nothing here signs documents or reads private keys.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
VECTORS = ROOT / "conformance" / "trust-lists" / "v1" / "vectors.json"
sys.path.insert(0, str(ROOT / "app" / "src"))

import trust_lists  # noqa: E402


def _load(path: str) -> dict[str, Any]:
    text = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
    bundle = json.loads(text)
    if not isinstance(bundle, dict) or not isinstance(bundle.get("documents"), list):
        raise SystemExit("bundle must be an object with a documents array")
    return bundle


def _now(value: int | None) -> int:
    return value if value is not None else int(time.time() * 1000)


def command_resolve(args: argparse.Namespace) -> int:
    bundle = _load(args.bundle)
    resolution = trust_lists.resolve(
        bundle["documents"],
        now=_now(args.now),
        follows=bundle.get("follows", []),
        overrides=bundle.get("overrides", []),
    )
    json.dump(resolution, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


def command_verify(args: argparse.Namespace) -> int:
    bundle = _load(args.bundle)
    failures = 0
    for raw in bundle["documents"]:
        doc_id = trust_lists.document_id(raw) if isinstance(raw, str) else "-"
        try:
            document = trust_lists.verify_document(raw)
        except trust_lists.TrustListError as error:
            failures += 1
            print(f"{doc_id} rejected {error.code}")
            continue
        print(f"{doc_id} ok {document['schema']}")
    return 1 if failures else 0


def _drill_case(vectors: dict[str, Any], case_id: str) -> dict[str, Any]:
    for case in vectors["cases"]:
        if case["id"] == case_id:
            return case
    raise SystemExit(f"vectors are missing {case_id}")


def command_drill(args: argparse.Namespace) -> int:
    vectors = json.loads(Path(args.vectors).read_text(encoding="utf-8"))
    before_case = _drill_case(vectors, "compromise-drill-before-revocation")
    after_case = _drill_case(vectors, "compromise-drill-revoked-and-recovered")

    def run(case: dict[str, Any]) -> tuple[dict[str, Any], float]:
        data = case["input"]
        started = time.perf_counter()
        result = trust_lists.resolve(
            data["documents"], now=data["now"], follows=data["follows"], overrides=data["overrides"]
        )
        return result, (time.perf_counter() - started) * 1000

    before, _ = run(before_case)
    after, elapsed_ms = run(after_case)
    target = before["lists"][0]["listId"]
    before_row = next(row for row in before["lists"] if row["listId"] == target)
    after_row = next(row for row in after["lists"] if row["listId"] == target)
    malicious = {
        (a["subject"], a["role"], a["scope"], a["effective"]) for a in before["assertions"]
    } - {(a["subject"], a["role"], a["scope"], a["effective"]) for a in after["assertions"]}
    report = {
        "list": target,
        "before": {"state": before_row["state"], "headVersion": before_row["headVersion"]},
        "after": {
            "state": after_row["state"],
            "headVersion": after_row["headVersion"],
            "cutoffVersion": after_row["cutoffVersion"],
            "cutoffReasons": after_row["cutoffReasons"],
            "successor": after_row["successor"],
        },
        "assertionsRemoved": len(malicious),
        "localRecoveryMs": round(elapsed_ms, 3),
    }
    json.dump(report, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    ok = (
        before_row["state"] == "active"
        and after_row["state"] == "recovered"
        and after_row["headVersion"] < before_row["headVersion"]
        and "revoked" in after_row["cutoffReasons"]
        and malicious
        and after == after_case["expect"]
    )
    return 0 if ok else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    resolve = sub.add_parser("resolve", help="print the resolution for a bundle")
    resolve.add_argument("bundle", help="bundle JSON path, or - for stdin")
    resolve.add_argument("--now", type=int, help="clock in milliseconds (default: current time)")
    resolve.set_defaults(func=command_resolve)
    verify = sub.add_parser("verify", help="verify each document in a bundle")
    verify.add_argument("bundle", help="bundle JSON path, or - for stdin")
    verify.set_defaults(func=command_verify)
    drill = sub.add_parser("drill", help="replay the compromised-list recovery drill")
    drill.add_argument("--vectors", default=str(VECTORS))
    drill.set_defaults(func=command_drill)
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
