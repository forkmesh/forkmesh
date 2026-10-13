#!/usr/bin/env python3
"""Fill or check the expected outputs in the shared availability-proof fixture.

Every case carries a hand-written ``expectProof``, ``expectRisks``, and
``expectClasses``; the reference implementation (app/src/availability_proof.py)
must agree with them before its full output is written to ``expect``.
``--check`` fails when the committed fixture differs from what the reference
produces.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


SUITE = Path(__file__).resolve().parent
ROOT = SUITE.parents[2]
FIXTURE = SUITE / "fixture.json"
sys.path.insert(0, str(ROOT / "app" / "src"))

import availability_proof  # noqa: E402


def _fill(entry):
    output = availability_proof.evaluate_mirrors_payload(entry["payload"])
    label = entry.get("id", "step")
    if output["proof"] != entry["expectProof"]:
        raise SystemExit("%s: reference proof %s, fixture expects %s" % (
            label, output["proof"], entry["expectProof"]))
    risks = sorted(risk["code"] for risk in output["risks"])
    if risks != sorted(entry["expectRisks"]):
        raise SystemExit("%s: reference risks %s, fixture expects %s" % (
            label, risks, sorted(entry["expectRisks"])))
    classes = [copy["class"] for copy in output["copies"]]
    if classes != entry["expectClasses"]:
        raise SystemExit("%s: reference classes %s, fixture expects %s" % (
            label, classes, entry["expectClasses"]))
    entry["expect"] = output


def build(fixture):
    for case in fixture["cases"]:
        _fill(case)
    for sequence in fixture["sequences"]:
        for step in sequence["steps"]:
            _fill(step)
    return fixture


def render(fixture):
    return json.dumps(fixture, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when fixture.json is not up to date")
    args = parser.parse_args(argv)
    current = FIXTURE.read_text(encoding="utf-8")
    expected = render(build(json.loads(current)))
    if args.check:
        if current != expected:
            print("fixture.json is out of date; run generate_fixture.py",
                  file=sys.stderr)
            return 1
        return 0
    FIXTURE.write_text(expected, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
