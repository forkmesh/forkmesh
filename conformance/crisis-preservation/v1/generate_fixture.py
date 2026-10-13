#!/usr/bin/env python3
"""Build or check the shared crisis-preservation fixture.

``fixture-input.json`` holds the requests, evidence blocks, and hand-written
expectations. Every plan carries ``expectMode`` and ``expectDecisions``, every
assessment ``expectStatus`` and ``expectStates``, and every drill
``expectResult`` and ``expectFailures``. The reference implementation
(app/src/crisis_preservation.py) must agree with them before its full output is
written to ``expect`` in ``fixture.json``. ``--check`` fails when the committed
fixture differs from what the reference produces.
"""

from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path


SUITE = Path(__file__).resolve().parent
ROOT = SUITE.parents[2]
INPUT = SUITE / "fixture-input.json"
FIXTURE = SUITE / "fixture.json"
sys.path.insert(0, str(ROOT / "app" / "src"))

import crisis_preservation  # noqa: E402


def _fail(label, what, actual, expected):
    raise SystemExit("%s: reference %s %s, fixture expects %s" % (
        label, what, actual, expected))


def _evidence(value, blocks):
    """Resolve ``{"$block": name}`` references to shared evidence blocks."""
    if isinstance(value, dict):
        if set(value) == {"$block"}:
            return copy.deepcopy(blocks[value["$block"]])
        return {key: _evidence(item, blocks) for key, item in value.items()}
    if isinstance(value, list):
        return [_evidence(item, blocks) for item in value]
    return value


def _plan(case):
    output = crisis_preservation.build_plan(case["request"])
    if output["mode"] != case["expectMode"]:
        _fail(case["id"], "mode", output["mode"], case["expectMode"])
    decisions = {step["category"]: step["decision"] for step in output["steps"]}
    if decisions != case["expectDecisions"]:
        _fail(case["id"], "decisions", decisions, case["expectDecisions"])
    case["expect"] = output


def _assessment(case):
    output = crisis_preservation.assess(case["request"], case["evidence"])
    if output["status"] != case["expectStatus"]:
        _fail(case["id"], "status", output["status"], case["expectStatus"])
    states = {item["id"]: item["state"] for item in output["categories"]}
    if states != case["expectStates"]:
        _fail(case["id"], "states", states, case["expectStates"])
    case["expect"] = output


def _drill(entry, label):
    output = crisis_preservation.run_drill(entry["drill"])
    if output["result"] != entry["expectResult"]:
        _fail(label, "result", output["result"], entry["expectResult"])
    if output["failures"] != entry["expectFailures"]:
        _fail(label, "failures", output["failures"], entry["expectFailures"])
    entry["expect"] = output


def build(source):
    blocks = source["evidenceBlocks"]
    fixture = {
        "schema": "forkmesh-crisis-preservation-fixture-v1",
        "model": "conformance/crisis-preservation/v1/model.json",
        "input": "conformance/crisis-preservation/v1/fixture-input.json",
        "plans": copy.deepcopy(source["plans"]),
        "assessments": _evidence(source["assessments"], blocks),
        "drills": _evidence(source["drills"], blocks),
        "sequences": _evidence(source["sequences"], blocks),
    }
    for case in fixture["plans"]:
        _plan(case)
    for case in fixture["assessments"]:
        _assessment(case)
    for case in fixture["drills"]:
        _drill(case, case["id"])
    for sequence in fixture["sequences"]:
        for index, step in enumerate(sequence["steps"]):
            _drill(step, "%s#%d" % (sequence["id"], index))
    return fixture


def render(fixture):
    return json.dumps(fixture, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when fixture.json is not up to date")
    args = parser.parse_args(argv)
    expected = render(build(json.loads(INPUT.read_text(encoding="utf-8"))))
    if args.check:
        current = FIXTURE.read_text(encoding="utf-8") if FIXTURE.exists() else ""
        if current != expected:
            print("fixture.json is out of date; run generate_fixture.py",
                  file=sys.stderr)
            return 1
        return 0
    FIXTURE.write_text(expected, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
