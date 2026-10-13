#!/usr/bin/env python3
"""Build or check the shared availability-contract fixture.

Each case below builds one evidence bundle (mirrors payloads, restore drills,
incident responses, capacity declarations, and contract terms) and carries a
hand-written ``expectOutcome``, ``expectAdmissionReasons``, ``expectMissCodes``,
and ``expectDisclosures``. The reference implementation
(app/src/availability_contract.py) must agree with them before its full report
is written to ``expect``. ``--check`` fails when the committed fixture differs
from what this script produces.
"""

from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path


SUITE = Path(__file__).resolve().parent
ROOT = SUITE.parents[2]
FIXTURE = SUITE / "fixture.json"
sys.path.insert(0, str(ROOT / "app" / "src"))

import availability_contract  # noqa: E402


T0 = 1800000000000
MINUTE = 60 * 1000
HOUR = 60 * MINUTE
DAY = 24 * HOUR
SLOTS = 6
DIGEST_A = "1" * 64
DIGEST_B = "2" * 64
ACCOUNTS = {
    "alpha": "account-alpha-private",
    "bravo": "account-bravo-private",
    "charlie": "account-charlie-private",
    "delta": "account-delta-private",
}
PRIVATE_STRINGS = list(ACCOUNTS.values()) + [
    "203.0.113.7",
    "git.private.example",
    "/var/backups/forkmesh/private-node.fmbackup",
    "ed25519:private-node-key",
]
REMEDIES = {
    "availabilityTiers": [
        {"belowBps": 9900, "creditBps": 1000},
        {"belowBps": 9500, "creditBps": 2500},
        {"belowBps": 9000, "creditBps": 5000},
    ],
    "recoveryMissCreditBps": 500,
    "responseMissCreditBps": 250,
    "drillMissCreditBps": 250,
    "maxCreditBps": 5000,
}
RELAXED_REMEDIES = dict(REMEDIES, availabilityTiers=[
    {"belowBps": 8000, "creditBps": 1000},
    {"belowBps": 5000, "creditBps": 5000},
])


def rfc3339(ms):
    moment = datetime.fromtimestamp(ms // 1000, tz=timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%S") + ".%03d000000Z" % (ms % 1000)


def row(node, operator, region, at, **overrides):
    base = {
        "node": node,
        "ownerUser": ACCOUNTS[operator],
        "owner": node,
        "source": "remote-clone",
        "status": "online",
        "integrity": "ok",
        "behind": False,
        "cloneAvailable": True,
        "lastSeen": at - MINUTE,
        "branchCount": 4,
        "artifactCount": 2,
        "region": region,
        "endpoint": "https://203.0.113.7:8443",
        "mirrorUrl": "https://git.private.example/acme/archive.git",
        "operations": ["tree", "blob", "git-upload-pack"],
        "abuseBlocked": False,
        "endpointFresh": True,
        "endpointHealthy": True,
        "endpointIntegrity": "ok",
        "checkedAt": at - MINUTE,
        "directVerified": True,
        "directReachable": True,
        "directRefsSha256": DIGEST_A,
        "directCheckedAt": at - HOUR,
    }
    for key, value in overrides.items():
        if value is None:
            base.pop(key, None)
        else:
            base[key] = value
    return base


OFFLINE = {"status": "offline", "endpointFresh": False}
UNPROBED = {"directVerified": None, "directReachable": None,
            "directRefsSha256": None, "directCheckedAt": None}


def drill(node, completed_at, duration=10 * MINUTE):
    return {
        "archive": "/var/backups/forkmesh/private-node.fmbackup",
        "drill": True,
        "node": node,
        "publicKey": "ed25519:private-node-key",
        "version": "0.8.10",
        "backupCreatedAt": rfc3339(completed_at - HOUR),
        "backupAgeSeconds": 3600,
        "files": 18,
        "repositories": 3,
        "refs": 42,
        "completedAt": rfc3339(completed_at),
        "durationMs": duration,
        "recoveryTimeObjectiveMs": 30 * MINUTE,
        "withinObjective": duration <= 30 * MINUTE,
    }


def terms(**overrides):
    base = {
        "copies": 3,
        "operators": 2,
        "regions": 2,
        "uptimeBps": 9900,
        "verificationIntervalMs": HOUR,
        "transports": ["router", "smartHttp"],
        "refCoverage": True,
        "artifactCoverage": True,
        "recoveryTimeObjectiveMs": 2 * HOUR,
        "drillIntervalMs": 92 * DAY,
        "incidentResponseMs": 30 * MINUTE,
        "failureTolerance": 1,
        "maxExcludedBps": 2000,
    }
    base.update(overrides)
    return base


def bundle(case_id, rows_for, *, contract_terms=None, remedies=REMEDIES,
           capacity=(), drills=(), responses=(), exclusions=(), skip=(),
           extra_observations=()):
    observations = [
        {
            "ok": True,
            "owner": "acme",
            "repo": "archive",
            "generatedAt": T0 + slot * HOUR + 30 * MINUTE,
            "mirrors": rows_for(slot, T0 + slot * HOUR + 30 * MINUTE),
        }
        for slot in range(SLOTS) if slot not in skip
    ]
    observations.extend(extra_observations)
    return {
        "schema": availability_contract.BUNDLE_SCHEMA,
        "contract": {
            "id": case_id,
            "repository": "acme/archive",
            "periodStart": T0,
            "periodEnd": T0 + SLOTS * HOUR,
            "feeMicrousd": 250_000_000,
            "terms": contract_terms or terms(),
            "remedies": remedies,
            "exclusions": list(exclusions),
        },
        "capacity": [{"node": node, "class": kind} for node, kind in capacity],
        "observations": observations,
        "drills": list(drills),
        "responses": list(responses),
    }


STANDARD_CAPACITY = (("mirror-a", "community"), ("mirror-b", "contracted"),
                     ("mirror-c", "contracted"))
STANDARD_DRILLS = tuple(drill(node, T0 - DAY) for node in ("mirror-a", "mirror-b", "mirror-c"))


def three_operators(slot, at, **overrides):
    return [
        row("mirror-a", "alpha", "eu-west", at, **overrides),
        row("mirror-b", "bravo", "us-east", at, **overrides),
        row("mirror-c", "charlie", "ap-south", at, **overrides),
    ]


def case_met():
    stale_sample = {
        "ok": True, "owner": "acme", "repo": "archive",
        "generatedAt": T0 - HOUR, "mirrors": [],
    }
    return bundle("met-three-operators", three_operators,
                  capacity=STANDARD_CAPACITY, drills=STANDARD_DRILLS,
                  extra_observations=[stale_sample])


def case_single_operator():
    def rows(slot, at):
        return [
            row("mirror-a", "bravo", "eu-west", at),
            row("mirror-b", "bravo", "us-east", at),
            row("mirror-c", "bravo", "us-east", at),
        ]
    return bundle("single-operator-not-admissible", rows,
                  capacity=STANDARD_CAPACITY, drills=STANDARD_DRILLS)


def case_drills():
    drills = (
        drill("mirror-a", T0 - DAY),
        # An older passing drill cannot hide the newer slow one.
        drill("mirror-b", T0 - 10 * DAY),
        drill("mirror-b", T0 - DAY, duration=3 * HOUR),
        drill("mirror-c", T0 - 100 * DAY),
    )
    return bundle("restore-drills-gate-capacity", three_operators,
                  contract_terms=terms(failureTolerance=0),
                  capacity=STANDARD_CAPACITY, drills=drills)


def case_unverified():
    return bundle("unverified-slots-late-response", three_operators,
                  capacity=STANDARD_CAPACITY, drills=STANDARD_DRILLS,
                  skip=(2, 3, 4),
                  responses=[{"incidentStart": T0 + 2 * HOUR,
                              "acknowledgedAt": T0 + 2 * HOUR + 45 * MINUTE}])


def case_exclusions():
    def rows(slot, at):
        return three_operators(slot, at, **(OFFLINE if slot in (1, 3) else {}))
    exclusions = [
        {"kind": "scheduled-maintenance", "start": T0 + HOUR, "end": T0 + 2 * HOUR,
         "announcedAt": T0 - DAY},
        {"kind": "customer-requested", "start": T0 + 3 * HOUR, "end": T0 + 4 * HOUR,
         "announcedAt": T0 + 3 * HOUR + 10 * MINUTE},
        {"kind": "force-majeure", "start": T0 + 4 * HOUR, "end": T0 + 5 * HOUR,
         "announcedAt": T0 - DAY},
        {"kind": "legal-hold", "start": T0 + 5 * HOUR, "end": T0 + 6 * HOUR,
         "announcedAt": T0 - DAY},
    ]
    return bundle("exclusions-late-unknown-over-budget", rows,
                  contract_terms=terms(uptimeBps=8000), remedies=RELAXED_REMEDIES,
                  capacity=STANDARD_CAPACITY, drills=STANDARD_DRILLS,
                  exclusions=exclusions,
                  responses=[{"incidentStart": T0 + 3 * HOUR,
                              "acknowledgedAt": T0 + 3 * HOUR + 10 * MINUTE}])


def case_disagreement():
    def rows(slot, at):
        return [
            row("mirror-a", "alpha", "eu-west", at),
            row("mirror-b", "bravo", "us-east", at, directRefsSha256=DIGEST_B),
            row("mirror-c", "charlie", "ap-south", at, **UNPROBED),
        ]
    return bundle("state-disagreement-no-false-quorum", rows,
                  contract_terms=terms(incidentResponseMs=8 * HOUR),
                  capacity=STANDARD_CAPACITY, drills=STANDARD_DRILLS)


def case_coverage():
    def rows(slot, at):
        return [
            row("mirror-a", "alpha", "eu-west", at, **UNPROBED),
            row("mirror-b", "bravo", "eu-west", at, branchCount=3, artifactCount=None,
                **UNPROBED),
        ]
    return bundle("coverage-and-transport-gaps", rows,
                  contract_terms=terms(transports=["router", "directHttps"]),
                  capacity=(("mirror-a", "community"), ("mirror-b", "contracted")),
                  drills=STANDARD_DRILLS[:2])


def case_regional_outage():
    def rows(slot, at):
        eu = OFFLINE if slot == 3 else {}
        return [
            row("mirror-a", "alpha", "eu-west", at, **eu),
            row("mirror-b", "bravo", "eu-west", at, **eu),
            row("mirror-c", "charlie", "us-east", at),
            row("mirror-d", "delta", "us-east", at),
        ]
    return bundle("regional-outage-retains-quorum", rows,
                  contract_terms=terms(uptimeBps=8000), remedies=RELAXED_REMEDIES,
                  capacity=STANDARD_CAPACITY + (("mirror-d", "community"),),
                  drills=STANDARD_DRILLS + (drill("mirror-d", T0 - DAY),),
                  responses=[{"incidentStart": T0 + 3 * HOUR,
                              "acknowledgedAt": T0 + 3 * HOUR + 15 * MINUTE}])


def case_operator_failure():
    def rows(slot, at):
        return [
            row("mirror-a", "alpha", "eu-west", at),
            row("mirror-b", "bravo", "us-east", at),
            row("mirror-c", "bravo", "ap-south", at),
        ]
    return bundle("operator-failure-beyond-tolerance", rows,
                  capacity=STANDARD_CAPACITY, drills=STANDARD_DRILLS)


def case_no_evidence():
    return bundle("no-evidence-fails-closed", three_operators,
                  contract_terms=terms(failureTolerance=0),
                  capacity=STANDARD_CAPACITY[1:], skip=range(SLOTS))


CASES = (
    {
        "id": "met-three-operators",
        "note": "Three operators in three regions verified in every slot, with fresh drills; one sample outside the period is ignored.",
        "build": case_met,
        "expectOutcome": "met",
        "expectAdmissionReasons": [],
        "expectMissCodes": [],
        "expectDisclosures": [],
    },
    {
        "id": "single-operator-not-admissible",
        "note": "One operator runs every copy: no slot reaches the quorum, and removing that operator removes everything.",
        "build": case_single_operator,
        "expectOutcome": "missed",
        "expectAdmissionReasons": ["operators_not_demonstrated", "failure_simulation_failed"],
        "expectMissCodes": ["availability_window", "recovery_objective_exceeded", "response_missing"],
        "expectDisclosures": ["community_capacity_relied_on", "contracted_single_operator",
                              "no_failure_headroom", "operator_concentration",
                              "region_concentration"],
    },
    {
        "id": "restore-drills-gate-capacity",
        "note": "Every copy is verified, but one contracted node's latest drill is slow and the other's is stale, so neither counts as capacity.",
        "build": case_drills,
        "expectOutcome": "missed",
        "expectAdmissionReasons": ["copies_not_demonstrated", "operators_not_demonstrated",
                                   "regions_not_demonstrated", "failure_simulation_failed"],
        "expectMissCodes": ["drill_beyond_objective", "drill_stale"],
        "expectDisclosures": ["capacity_not_demonstrated", "community_capacity_relied_on",
                              "no_failure_headroom", "operator_concentration",
                              "region_concentration"],
    },
    {
        "id": "unverified-slots-late-response",
        "note": "Three slots have no sample at all; the incident outlasts the recovery objective and is acknowledged late.",
        "build": case_unverified,
        "expectOutcome": "missed",
        "expectAdmissionReasons": ["copies_not_demonstrated", "operators_not_demonstrated",
                                   "regions_not_demonstrated", "failure_simulation_failed"],
        "expectMissCodes": ["availability_window", "recovery_objective_exceeded", "response_late"],
        "expectDisclosures": ["capacity_not_demonstrated", "community_capacity_relied_on",
                              "no_failure_headroom"],
    },
    {
        "id": "exclusions-late-unknown-over-budget",
        "note": "A pre-announced maintenance slot is excluded; a late announcement, an unknown kind, and an over-budget exclusion are not.",
        "build": case_exclusions,
        "expectOutcome": "met",
        "expectAdmissionReasons": [],
        "expectMissCodes": ["availability_window"],
        "expectDisclosures": [],
    },
    {
        "id": "state-disagreement-no-false-quorum",
        "note": "Two probed copies disagree on the refs digest, so only one operator counts; the incident response is still inside its window.",
        "build": case_disagreement,
        "expectOutcome": "missed",
        "expectAdmissionReasons": ["failure_simulation_failed"],
        "expectMissCodes": ["availability_window", "recovery_objective_exceeded"],
        "expectDisclosures": [],
    },
    {
        "id": "coverage-and-transport-gaps",
        "note": "Two copies in one region, no direct clone probe, a missing branch, and an unknown artifact count.",
        "build": case_coverage,
        "expectOutcome": "missed",
        "expectAdmissionReasons": ["copies_not_demonstrated", "regions_not_demonstrated",
                                   "failure_simulation_failed"],
        "expectMissCodes": ["availability_window", "recovery_objective_exceeded", "response_missing"],
        "expectDisclosures": ["community_capacity_relied_on", "no_failure_headroom",
                              "region_concentration"],
    },
    {
        "id": "regional-outage-retains-quorum",
        "note": "A whole region goes offline for one slot: the operator quorum holds, recovery meets its objective, and every simulated operator or regional failure passes.",
        "build": case_regional_outage,
        "expectOutcome": "met",
        "expectAdmissionReasons": [],
        "expectMissCodes": ["availability_window"],
        "expectDisclosures": [],
    },
    {
        "id": "operator-failure-beyond-tolerance",
        "note": "Every slot meets the contract, but one operator runs both contracted copies, so losing it breaks the quorum.",
        "build": case_operator_failure,
        "expectOutcome": "met",
        "expectAdmissionReasons": ["failure_simulation_failed"],
        "expectMissCodes": [],
        "expectDisclosures": ["community_capacity_relied_on", "contracted_single_operator",
                              "no_failure_headroom", "operator_concentration"],
    },
    {
        "id": "no-evidence-fails-closed",
        "note": "No samples and no drills: every slot is unverified, nothing is admissible, and credits hit the cap.",
        "build": case_no_evidence,
        "expectOutcome": "missed",
        "expectAdmissionReasons": ["no_evidence", "copies_not_demonstrated",
                                   "operators_not_demonstrated", "regions_not_demonstrated",
                                   "failure_simulation_failed"],
        "expectMissCodes": ["availability_window", "drill_missing", "drill_missing",
                            "recovery_objective_exceeded", "response_missing"],
        "expectDisclosures": ["capacity_not_demonstrated", "community_capacity_relied_on",
                              "no_failure_headroom"],
    },
)


def build():
    cases = []
    for spec in CASES:
        evidence = spec["build"]()
        report = availability_contract.evaluate_contract(evidence)
        actual = {
            "expectOutcome": report["outcome"],
            "expectAdmissionReasons": report["admission"]["reasons"],
            "expectMissCodes": sorted(miss["code"] for miss in report["misses"]),
            "expectDisclosures": sorted(item["code"] for item in report["disclosures"]),
        }
        for field, value in actual.items():
            if value != spec[field]:
                raise SystemExit("%s: reference %s %s, fixture expects %s" % (
                    spec["id"], field, value, spec[field]))
        entry = {"id": spec["id"], "note": spec["note"]}
        entry.update({field: spec[field] for field in actual})
        entry["bundle"] = evidence
        entry["expect"] = report
        cases.append(entry)
    return {
        "schema": "forkmesh-availability-contract-fixture-v1",
        "model": "conformance/availability-contract/v1/model.json",
        "privateStrings": PRIVATE_STRINGS,
        "cases": cases,
    }


def render(fixture):
    return json.dumps(fixture, indent=2, ensure_ascii=False) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="fail when fixture.json is not up to date")
    args = parser.parse_args(argv)
    expected = render(build())
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
