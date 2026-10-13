#!/usr/bin/env python3
"""Fill or check the expected outputs in the shared sync-status fixture.

Every case carries a hand-written ``expectState``; the reference
implementation (app/src/sync_status.py) must agree with it before its full
output is written to ``expect``. ``--check`` fails when the committed fixture
differs from what the reference produces.
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

import sync_status  # noqa: E402


def _fill(entry, compute):
    output = compute()
    if output["state"] != entry["expectState"]:
        raise SystemExit("%s: reference says %s, fixture expects %s" % (
            entry.get("id", "step"), output["state"], entry["expectState"]))
    entry["expect"] = output


def build(fixture):
    for case in fixture["cases"]:
        _fill(case, lambda case=case: sync_status.evaluate(case["evidence"]))
    for sequence in fixture["sequences"]:
        for step in sequence["steps"]:
            _fill(step, lambda step=step: sync_status.evaluate(step["evidence"]))
    for case in fixture["mirrorsPayloadCases"]:
        _fill(case, lambda case=case: sync_status.mirrors_payload_status(case["payload"]))
    return fixture


def render(fixture):
    return json.dumps(fixture, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)
    committed = FIXTURE.read_text(encoding="utf-8")
    text = render(build(json.loads(committed)))
    if args.check:
        if text != committed:
            print("fixture.json is out of date; run generate_fixture.py", file=sys.stderr)
            return 1
        return 0
    FIXTURE.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
