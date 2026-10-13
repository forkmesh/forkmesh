#!/usr/bin/env python3
"""Plan, assess, and drill a preservation-first crisis migration.

Implements the forkmesh-crisis-preservation v1 model
(www/docs/crisis-preservation.md) by reusing the Worker's reference
implementation, so the CLI, the API, and the shared fixture always agree.

  forkmesh_crisis_preservation.py plan urgent_takedown --minutes 60 --access owner
  forkmesh_crisis_preservation.py plan lost_account --private --local-clone --json
  forkmesh_crisis_preservation.py assess request.json --report report.json \\
      --availability mirrors.json [--identity identity.json] [--json]
  forkmesh_crisis_preservation.py drill drill.json [--json]
  forkmesh_crisis_preservation.py fixture

`--availability` accepts either a mirrors payload (its `availability` field is
used) or a bare availability proof. `--report` accepts the report export
(its latest `report`) or a bare report.

Exit codes: 0 plan built, everything preserved, or drill passed; 3 at risk,
partly preserved, or drill failed; 1 fixture mismatch; 2 usage or unreadable
input.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "conformance" / "crisis-preservation" / "v1" / "fixture.json"
sys.path.insert(0, str(ROOT / "app" / "src"))

import crisis_preservation  # noqa: E402

EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_USAGE = 2
EXIT_AT_RISK = 3


def _read_json(path):
    text = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
    return json.loads(text)


def _unwrap_report(value):
    if isinstance(value, dict) and isinstance(value.get("report"), dict):
        return value["report"]
    return value


def _unwrap_availability(value):
    if isinstance(value, dict) and isinstance(value.get("availability"), dict):
        return value["availability"]
    return value


def fixture_mismatches():
    fixture = json.loads(FIXTURE.read_text(encoding="utf-8"))
    entries = []
    for case in fixture["plans"]:
        entries.append((case["id"], crisis_preservation.build_plan(case["request"]),
                        case["expect"]))
    for case in fixture["assessments"]:
        entries.append((case["id"], crisis_preservation.assess(
            case["request"], case["evidence"]), case["expect"]))
    for case in fixture["drills"]:
        entries.append((case["id"], crisis_preservation.run_drill(case["drill"]),
                        case["expect"]))
    for sequence in fixture["sequences"]:
        for index, step in enumerate(sequence["steps"]):
            entries.append(("%s#%d" % (sequence["id"], index),
                            crisis_preservation.run_drill(step["drill"]),
                            step["expect"]))
    mismatches = [case_id for case_id, actual, expect in entries if actual != expect]
    return len(entries), mismatches


def _print_plan(plan):
    print("%s (%s mode)" % (plan["title"], plan["mode"]))
    for line in plan["summary"]:
        print("  " + line)
    for step in plan["steps"]:
        when = ("at minute %d" % step["startsAtMinute"]
                if step["startsAtMinute"] is not None else step["reason"])
        print("  %d. [%s] %s, ~%d min, %s" % (
            step["order"], step["decision"], step["title"],
            step["estimateMinutes"], when))
    for tradeoff in plan["tradeoffs"]:
        print("  tradeoff: " + tradeoff["consequence"])


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    plan = commands.add_parser("plan", help="build a preservation plan")
    plan.add_argument("scenario", choices=crisis_preservation.SCENARIO_IDS)
    plan.add_argument("--minutes", type=int)
    plan.add_argument("--access", choices=crisis_preservation.ACCESS_LEVELS)
    plan.add_argument("--size-mb", type=int)
    plan.add_argument("--issues", type=int)
    plan.add_argument("--releases", type=int)
    plan.add_argument("--assets-mb", type=int)
    plan.add_argument("--private", action="store_true")
    plan.add_argument("--local-clone", action="store_true")
    plan.add_argument("--json", action="store_true")

    assess = commands.add_parser("assess", help="judge what was preserved")
    assess.add_argument("request", help="plan request JSON file, or - for stdin")
    assess.add_argument("--report", help="migration report or report export JSON")
    assess.add_argument("--availability", help="mirrors payload or availability proof JSON")
    assess.add_argument("--identity", help="identity verification JSON")
    assess.add_argument("--json", action="store_true")

    drill = commands.add_parser("drill", help="score a scenario drill record")
    drill.add_argument("input", help="drill JSON file, or - for stdin")
    drill.add_argument("--json", action="store_true")

    commands.add_parser("fixture", help="verify the shared conformance fixture")
    args = parser.parse_args(argv)

    if args.command == "fixture":
        total, mismatches = fixture_mismatches()
        print(json.dumps({"cases": total, "mismatches": mismatches}, sort_keys=True))
        return EXIT_MISMATCH if mismatches else EXIT_OK

    try:
        if args.command == "plan":
            repository = {
                "private": args.private,
                "localClone": args.local_clone,
            }
            for field, value in (("sizeMb", args.size_mb), ("issues", args.issues),
                                 ("releases", args.releases),
                                 ("assetsMb", args.assets_mb)):
                if value is not None:
                    repository[field] = value
            result = crisis_preservation.build_plan({
                "scenario": args.scenario, "minutes": args.minutes,
                "access": args.access, "repository": repository,
            })
            if args.json:
                print(json.dumps(result, sort_keys=True))
            else:
                _print_plan(result)
            return EXIT_OK
        if args.command == "assess":
            evidence = {}
            if args.report:
                evidence["migrationReport"] = _unwrap_report(_read_json(args.report))
            if args.availability:
                evidence["availability"] = _unwrap_availability(
                    _read_json(args.availability))
            if args.identity:
                evidence["identity"] = _read_json(args.identity)
            result = crisis_preservation.assess(_read_json(args.request), evidence)
            if args.json:
                print(json.dumps(result, sort_keys=True))
            else:
                print("%s: %s" % (result["status"], " ".join(result["summary"])))
            return EXIT_OK if result["status"] == "preserved" else EXIT_AT_RISK
        result = crisis_preservation.run_drill(_read_json(args.input))
    except crisis_preservation.PlanRequestError as exc:
        print("%s: %s" % (exc.code, exc.detail), file=sys.stderr)
        return EXIT_USAGE
    except (OSError, ValueError) as exc:
        print("cannot read input: %s" % exc, file=sys.stderr)
        return EXIT_USAGE
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print("%s: %s drill, %s" % (
            result["result"], result["scenario"],
            ", ".join(result["failures"]) or "verified mirror and report within budget"))
    return EXIT_OK if result["result"] == "passed" else EXIT_AT_RISK


if __name__ == "__main__":
    raise SystemExit(main())
