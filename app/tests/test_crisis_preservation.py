"""Preservation-first crisis onboarding: reference model, fixture, and clients.

Pins the reference implementation to its model document and shared fixture,
and checks the CLI, the Worker route, the real migration-report and
availability-proof producers, and the user documentation against the same
vocabulary.
"""

import asyncio
import json
import subprocess
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import pytest


ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
SUITE = ROOT / "conformance" / "crisis-preservation" / "v1"
DOC = ROOT / "www" / "docs" / "crisis-preservation.md"
CLI = ROOT / "tools" / "forkmesh_crisis_preservation.py"
sys.path.insert(0, str(ROOT / "app" / "src"))
if str(TESTS) not in sys.path:
    sys.path.insert(0, str(TESTS))

import crisis_preservation as cp  # noqa: E402
import repository_migration  # noqa: E402


MODEL = json.loads((SUITE / "model.json").read_text(encoding="utf-8"))
FIXTURE = json.loads((SUITE / "fixture.json").read_text(encoding="utf-8"))

PLANS = [(case["id"], case) for case in FIXTURE["plans"]]
ASSESSMENTS = [(case["id"], case) for case in FIXTURE["assessments"]]
DRILLS = [(case["id"], case) for case in FIXTURE["drills"]] + [
    ("%s#%d" % (sequence["id"], index), step)
    for sequence in FIXTURE["sequences"]
    for index, step in enumerate(sequence["steps"])
]


def test_model_document_matches_the_reference_vocabulary():
    assert MODEL["outputSchemas"] == {
        "plan": cp.PLAN_SCHEMA, "assessment": cp.ASSESSMENT_SCHEMA,
        "drill": cp.DRILL_SCHEMA, "catalog": cp.CATALOG_SCHEMA,
    }
    assert MODEL["evidenceSchemas"] == {
        "migrationReport": cp.MIGRATION_REPORT_SCHEMA,
        "availability": cp.AVAILABILITY_SCHEMA,
    }
    policy = MODEL["policy"]
    assert policy["urgentMinutes"] == cp.URGENT_MINUTES
    assert policy["maxMinutes"] == cp.MAX_MINUTES
    assert policy["transferMbPerMinute"] == cp.TRANSFER_MB_PER_MINUTE
    assert policy["providerRecordsPerPage"] == cp.PROVIDER_RECORDS_PER_PAGE
    assert policy["pagesPerMinute"] == cp.PAGES_PER_MINUTE
    assert policy["issueEarlyPageFactor"] == cp.ISSUE_EARLY_PAGE_FACTOR
    assert policy["issueLatePageFactor"] == cp.ISSUE_LATE_PAGE_FACTOR
    assert policy["mirrorReceiptMinutes"] == cp.MIRROR_RECEIPT_MINUTES
    assert policy["assumedRepository"] == cp.ASSUMED_REPOSITORY
    assert tuple(policy["importOrder"]) == cp.IMPORT_ORDER
    assert tuple(MODEL["accessLevels"]) == cp.ACCESS_LEVELS
    assert tuple(MODEL["decisions"]) == cp.DECISIONS
    assert MODEL["reasons"] == list(cp.REASONS)
    assert [(c["id"], c["required"]) for c in MODEL["categories"]] == [
        (c["id"], c["required"]) for c in cp.CATEGORIES]
    assert [
        (s["id"], s["urgent"], s["defaultMinutes"], s["defaultAccess"],
         s["maxAccess"], s["drillBudgetMinutes"], tuple(s["priority"]))
        for s in MODEL["scenarios"]
    ] == [
        (s["id"], s["urgent"], s["defaultMinutes"], s["defaultAccess"],
         s["maxAccess"], s["drillBudgetMinutes"], s["priority"])
        for s in cp.SCENARIOS
    ]
    assert tuple(MODEL["coverageStates"]) == cp.COVERAGE_STATES
    assert tuple(MODEL["riskTopics"]) == cp.TOPIC_IDS
    assert [(r["id"], r["requiresPayment"], r["available"]) for r in MODEL["routes"]] == [
        (r["id"], r["requiresPayment"], r["available"]) for r in cp.ROUTES]
    assert tuple(MODEL["drillFailures"]) == cp.DRILL_FAILURES
    for implementation in MODEL["implementations"]:
        assert (ROOT / implementation["path"]).is_file(), implementation
        assert (ROOT / implementation["test"]).is_file(), implementation


def test_import_order_follows_the_importer_phases():
    def last_phase(category_id):
        category = next(c for c in cp.CATEGORIES if c["id"] == category_id)
        return max(repository_migration.PHASES.index(kind)
                   for kind in category["migrationTypes"])

    assert list(cp.IMPORT_ORDER) == sorted(cp.IMPORT_ORDER, key=last_phase)
    covered = {kind for c in cp.CATEGORIES for kind in c["migrationTypes"]}
    assert set(repository_migration.OBJECT_TYPES) <= covered
    for scenario in cp.SCENARIOS:
        priority = scenario["priority"]
        assert sorted(priority) == sorted(cp.CATEGORY_IDS), scenario["id"]
        assert priority[0] == "refs", scenario["id"]
        positions = [priority.index(c) for c in cp.IMPORT_ORDER]
        assert positions == sorted(positions), scenario["id"]
        assert sorted(scenario["topics"]) == sorted(cp.TOPIC_IDS), scenario["id"]


def test_committed_fixture_matches_reference_generation():
    completed = subprocess.run(
        [sys.executable, str(SUITE / "generate_fixture.py"), "--check"],
        capture_output=True, text=True, check=False, cwd=ROOT, timeout=60,
    )
    assert completed.returncode == 0, completed.stderr


@pytest.mark.parametrize("case_id,case", PLANS, ids=[p[0] for p in PLANS])
def test_reference_reproduces_every_fixture_plan(case_id, case):
    plan = cp.build_plan(case["request"])
    assert plan == case["expect"], case_id
    assert plan["mode"] == case["expectMode"]
    assert {s["category"]: s["decision"] for s in plan["steps"]} == case["expectDecisions"]


@pytest.mark.parametrize("case_id,case", ASSESSMENTS, ids=[a[0] for a in ASSESSMENTS])
def test_reference_reproduces_every_fixture_assessment(case_id, case):
    report = cp.assess(case["request"], case["evidence"])
    assert report == case["expect"], case_id
    assert report["status"] == case["expectStatus"]
    assert {c["id"]: c["state"] for c in report["categories"]} == case["expectStates"]


@pytest.mark.parametrize("case_id,case", DRILLS, ids=[d[0] for d in DRILLS])
def test_reference_reproduces_every_fixture_drill(case_id, case):
    result = cp.run_drill(case["drill"])
    assert result == case["expect"], case_id
    assert result["result"] == case["expectResult"]
    assert result["failures"] == case["expectFailures"]


def test_fixture_covers_every_scenario_decision_reason_state_and_failure():
    plans = [case["expect"] for _, case in PLANS]
    assert {p["scenario"] for p in plans} == set(cp.SCENARIO_IDS)
    assert {p["mode"] for p in plans} == {"urgent", "standard"}
    assert {s["decision"] for p in plans for s in p["steps"]} == set(cp.DECISIONS)
    assert {t["reason"] for p in plans for t in p["tradeoffs"]} == set(cp.REASONS)
    assert any(p["notes"] for p in plans)
    assert any(s.get("viaLocalClone") for p in plans for s in p["steps"])
    states = {c["state"] for _, case in ASSESSMENTS for c in case["expect"]["categories"]}
    assert states == set(cp.COVERAGE_STATES)
    assert {case["expect"]["status"] for _, case in ASSESSMENTS} == {
        "preserved", "partial", "at_risk"}
    failures = {code for _, case in DRILLS for code in case["expect"]["failures"]}
    assert failures == set(cp.DRILL_FAILURES)


def test_every_scenario_has_a_drill_with_a_verified_mirror_and_report_within_budget():
    passing = {}
    for _, case in DRILLS:
        result = case["expect"]
        if result["result"] != "passed":
            continue
        assert result["withinBudget"] is True
        assert result["elapsedMs"] <= result["budgetMinutes"] * 60 * 1000
        assessment = cp.assess(case["drill"]["request"], case["drill"]["evidence"])
        assert assessment["verifiedUsableMirror"] is True
        assert assessment["migrationReport"]["present"] is True
        assert assessment["migrationReport"]["reconciled"] is True
        passing.setdefault(result["scenario"], []).append(result)
    assert set(passing) == set(cp.SCENARIO_IDS)
    budgets = {s["id"]: s["drillBudgetMinutes"] for s in cp.SCENARIOS}
    assert budgets["urgent_takedown"] < budgets["censorship"] <= budgets["planned_shutdown"]


def test_every_failure_sequence_ends_recovered():
    for sequence in FIXTURE["sequences"]:
        results = [step["expect"]["result"] for step in sequence["steps"]]
        assert "failed" in results[:-1], sequence["id"]
        assert results[-1] == "passed", sequence["id"]
    ids = {sequence["id"] for sequence in FIXTURE["sequences"]}
    assert {"urgent-takedown-retry-recovers", "provider-eol-mirror-outage-recovers",
            "import-rollback-recovers"} <= ids


def _requests():
    sizes = ({}, {"sizeMb": 4000, "issues": 50000, "releases": 900, "assetsMb": 90000})
    for scenario in cp.SCENARIO_IDS:
        for minutes in (1, 5, 20, 60, 240, 1440, 43200):
            for access in cp.ACCESS_LEVELS:
                for private in (False, True):
                    for local_clone in (False, True):
                        for repository in sizes:
                            yield {
                                "scenario": scenario, "minutes": minutes,
                                "access": access,
                                "repository": dict(repository, private=private,
                                                   localClone=local_clone),
                            }


def test_skipped_or_unreachable_data_is_never_marked_preserved():
    for request in _requests():
        assessment = cp.assess(request, {})
        for category in assessment["categories"]:
            assert category["state"] in ("missing", "skipped", "unavailable"), (
                request, category)
        assert assessment["status"] == "at_risk"
        assert assessment["verifiedUsableMirror"] is False
    for _, case in ASSESSMENTS:
        for category in case["expect"]["categories"]:
            if category["state"] in ("preserved", "empty", "partial", "failed"):
                assert category["evidence"], (case["id"], category["id"])
            if category["decision"] == "deferred" and not category["evidence"]:
                assert category["state"] == "skipped", (case["id"], category["id"])
            if category["decision"] == "unavailable" and not category["evidence"]:
                assert category["state"] == "unavailable", (case["id"], category["id"])


def test_plans_make_every_tradeoff_explicit_and_fit_their_window():
    for request in _requests():
        plan = cp.build_plan(request)
        included = [s for s in plan["steps"] if s["decision"] == "included"]
        assert plan["plannedMinutes"] == sum(s["estimateMinutes"] for s in included)
        start = 0
        for step in included:
            assert step["startsAtMinute"] == start
            assert step["action"] and step["action"]["label"]
            start += step["estimateMinutes"]
        over = [s for s in included if s["reason"] == "over_budget"]
        if not over:
            assert plan["plannedMinutes"] <= plan["minutesAvailable"], request
        else:
            assert [s["category"] for s in over] == ["refs"]
        explained = {t["category"] for t in plan["tradeoffs"]}
        for step in plan["steps"]:
            if step["decision"] != "included" or step["reason"]:
                assert step["category"] in explained, (request, step)
                assert step["reason"] in cp.REASONS
        for tradeoff in plan["tradeoffs"]:
            assert tradeoff["consequence"] and tradeoff["followUp"]
            assert tradeoff["href"] == cp.DOCS_URL + "#" + tradeoff["reason"]
        if plan["mode"] == "urgent" and plan["tradeoffs"]:
            assert plan["acknowledgementRequired"] is True
        if not plan["tradeoffs"]:
            assert plan["acknowledgementRequired"] is False
        not_planned = [t for t in plan["tradeoffs"] if t["decision"] != "included"]
        if not_planned:
            assert "None of it will be reported as preserved." in plan["summary"][1]
        # The import can never finish a later part after skipping an earlier one.
        decisions = {s["category"]: s for s in plan["steps"]}
        seen_blocked = False
        for category in cp.IMPORT_ORDER:
            step = decisions[category]
            if step.get("viaLocalClone"):
                continue
            if seen_blocked:
                assert step["decision"] != "included", request
            if step["decision"] != "included":
                seen_blocked = True
        if request["access"] == "none" and not request["repository"]["localClone"]:
            assert decisions["refs"]["decision"] == "unavailable"
            assert decisions["trusted_mirrors"]["reason"] == "needs_refs"


def test_recovery_routes_avoid_lock_in():
    plan = cp.build_plan({"scenario": "urgent_takedown"})
    routes = plan["routes"]
    assert [r["id"] for r in routes] == list(cp.ROUTE_IDS)
    assert any(r["available"] and not r["requiresPayment"] for r in routes)
    for route in routes:
        assert route["exit"] == cp.EXIT_PATH
        assert route["href"].startswith("https://forkmesh.com/docs/")
        if route["requiresPayment"]:
            # Paid recovery must never be offered as available until it is sold.
            assert route["available"] is False
            assert "not for sale" in route["detail"]
    business = (ROOT / "www" / "docs" / "business-model.md").read_text(encoding="utf-8")
    assert "Research preview; not for sale" in business
    assert "recovery" in business
    security = (ROOT / "SECURITY.md").read_text(encoding="utf-8")
    contacts = next(t for t in cp.RISK_TOPICS if t["id"] == "emergency_contacts")
    assert "security@forkmesh.com" in contacts["detail"]
    assert "security@forkmesh.com" in security


def test_invalid_requests_fail_with_closed_codes():
    cases = (
        ({}, "unknown_scenario"),
        ({"scenario": "meteor"}, "unknown_scenario"),
        ({"scenario": "censorship", "minutes": 0}, "invalid_minutes"),
        ({"scenario": "censorship", "minutes": "soon"}, "invalid_minutes"),
        ({"scenario": "censorship", "minutes": cp.MAX_MINUTES + 1}, "invalid_minutes"),
        ({"scenario": "censorship", "access": "root"}, "unknown_access"),
        ({"scenario": "censorship", "sizeMb": -1}, "invalid_sizeMb"),
        ({"scenario": "censorship", "repository": {"issues": True}}, "invalid_issues"),
    )
    for request, code in cases:
        with pytest.raises(cp.PlanRequestError) as raised:
            cp.build_plan(request)
        assert raised.value.code == code, request
    # Query-string shaped values are accepted.
    plan = cp.build_plan({"scenario": "lost_account", "minutes": "90",
                          "private": "1", "localClone": "true", "sizeMb": "10"})
    assert plan["minutesAvailable"] == 90
    assert plan["repository"]["private"] is True
    assert plan["repository"]["localClone"] is True


def test_malformed_evidence_fails_closed():
    for evidence in (None, [], "x", {"migrationReport": [], "availability": 3},
                     {"migrationReport": {"schema": cp.MIGRATION_REPORT_SCHEMA,
                                          "outcome": "verified",
                                          "reconciliation": {"rows": {"issues": "x"}}}},
                     {"availability": {"schema": cp.AVAILABILITY_SCHEMA, "proof": "quorum",
                                       "counts": "x", "coverage": None}},
                     {"identity": {"recoveryKitVerified": "yes"}}):
        assessment = cp.assess({"scenario": "censorship"}, evidence)
        assert assessment["status"] in ("at_risk", "partial")
        assert assessment["verifiedUsableMirror"] is False
        assert all(c["state"] not in ("preserved", "empty")
                   for c in assessment["categories"] if c["id"] != "trusted_mirrors")
    result = cp.run_drill({"request": {"scenario": "censorship"}, "startedAt": True})
    assert result["result"] == "failed" and "invalid_timing" in result["failures"]


def test_real_migration_report_and_availability_proof_pass_a_drill():
    from test_repository_migration import Harness

    harness = Harness("github")
    started = harness.start()
    job_id = started["data"]["migration"]["id"]
    final = harness.finish(job_id)[-1]
    assert final["data"]["migration"]["status"] == "completed", final
    response = harness.call(
        "GET", "/api/repository-imports/migrations/%s/report" % job_id)
    assert response["status"] == 200, response
    report = response["data"]["report"]

    availability_fixture = json.loads(
        (ROOT / "conformance" / "availability-proof" / "v1" / "fixture.json")
        .read_text(encoding="utf-8"))
    availability = next(
        case for case in availability_fixture["cases"]
        if case["id"] == "quorum-two-operators")["expect"]

    drill = {
        "id": "real-producers", "request": {"scenario": "urgent_takedown"},
        "startedAt": 1800000000000, "verifiedAt": 1800000000000 + 25 * 60 * 1000,
        "evidence": {"migrationReport": report, "availability": availability},
    }
    result = cp.run_drill(drill)
    assert result["result"] == "passed", result
    assessment = cp.assess(drill["request"], drill["evidence"])
    states = {c["id"]: c["state"] for c in assessment["categories"]}
    assert states["refs"] == "preserved"
    assert states["trusted_mirrors"] == "preserved"
    assert states["issues"] == "preserved"
    # The fixture's release asset bytes are external in the report, but the
    # verified mirrors carry every artifact, so they are preserved by the mirror.
    assert states["artifacts"] == "preserved"
    assert states["identity_proofs"] == "missing"
    assert assessment["migrationReport"]["jobId"] == job_id


def _cli(*args, stdin=None):
    return subprocess.run(
        [sys.executable, str(CLI), *args], input=stdin, capture_output=True,
        text=True, check=False, cwd=ROOT, timeout=60,
    )


def test_cli_plans_assesses_drills_and_verifies_the_fixture(tmp_path):
    completed = _cli("fixture")
    assert completed.returncode == 0, completed.stderr
    summary = json.loads(completed.stdout)
    assert summary["mismatches"] == []
    assert summary["cases"] == len(PLANS) + len(ASSESSMENTS) + len(DRILLS)

    case = next(c for _, c in PLANS if c["id"] == "urgent-takedown-tight-window")
    completed = _cli("plan", "urgent_takedown", "--minutes", "20", "--size-mb", "300",
                     "--issues", "4000", "--releases", "20", "--assets-mb", "200", "--json")
    assert completed.returncode == 0, completed.stderr
    assert json.loads(completed.stdout) == case["expect"]
    completed = _cli("plan", "urgent_takedown", "--minutes", "20", "--size-mb", "300")
    assert completed.returncode == 0 and "tradeoff:" in completed.stdout

    assessment = next(c for _, c in ASSESSMENTS if c["id"] == "tampered-report-is-not-trusted")
    request = tmp_path / "request.json"
    report = tmp_path / "report.json"
    mirrors = tmp_path / "mirrors.json"
    request.write_text(json.dumps(assessment["request"]))
    # The CLI unwraps a report export and a mirrors payload.
    report.write_text(json.dumps({"report": assessment["evidence"]["migrationReport"]}))
    mirrors.write_text(json.dumps({"availability": assessment["evidence"]["availability"]}))
    completed = _cli("assess", str(request), "--report", str(report),
                     "--availability", str(mirrors), "--json")
    assert completed.returncode == 3, completed.stderr
    assert json.loads(completed.stdout) == assessment["expect"]

    passing = next(c for _, c in DRILLS if c["id"] == "censorship-drill-passes")
    completed = _cli("drill", "-", stdin=json.dumps(passing["drill"]))
    assert completed.returncode == 0 and completed.stdout.startswith("passed")
    failing = next(c for _, c in DRILLS if c["id"] == "drill-tampered-report")
    completed = _cli("drill", "--json", "-", stdin=json.dumps(failing["drill"]))
    assert completed.returncode == 3
    assert json.loads(completed.stdout) == failing["expect"]
    assert _cli("drill", "-", stdin="not json").returncode == 2
    assert _cli("assess", "-", stdin=json.dumps({"scenario": "meteor"})).returncode == 2


def _handler():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    start = entry.index("async def crisis_preservation_plan_handler(")
    end = entry.index("\n\n\n", start)
    responses = []

    def json_response(data, status=200, cache_seconds=None, **_):
        responses.append((status, data, cache_seconds))
        return responses[-1]

    namespace = {
        "crisis_preservation": cp, "json_response": json_response,
        "parse_qs": parse_qs, "urlparse": urlparse,
        "method_name": lambda request: request.method,
    }
    exec(entry[start:end], namespace)  # noqa: S102 - the real handler source
    return namespace["crisis_preservation_plan_handler"]


class _Request:
    def __init__(self, url, method="GET"):
        self.url = url
        self.method = method


def test_worker_route_serves_catalog_plans_and_closed_errors():
    handler = _handler()
    base = "https://app.forkmesh.com/api/preservation/crisis-plan"

    status, data, cache = asyncio.run(handler(None, _Request(base)))
    assert status == 200 and cache == 300
    assert data == cp.catalog()

    status, data, cache = asyncio.run(handler(None, _Request(
        base + "?scenario=urgent_takedown&minutes=20&sizeMb=300&issues=4000"
        "&releases=20&assetsMb=200")))
    case = next(c for _, c in PLANS if c["id"] == "urgent-takedown-tight-window")
    assert status == 200 and cache == 300 and data == case["expect"]

    status, data, _ = asyncio.run(handler(None, _Request(base + "?scenario=meteor")))
    assert status == 400 and data["error"] == "unknown_scenario"
    status, data, _ = asyncio.run(handler(None, _Request(base, method="POST")))
    assert status == 405


def test_worker_route_is_registered_lazy_and_catalogued():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert 'crisis_preservation = _LazyModule("crisis_preservation")' in entry
    runtime = (ROOT / "app" / "src" / "app_runtime.py").read_text(encoding="utf-8")
    assert '"/api/preservation/crisis-plan"' in runtime
    assert "crisis_preservation_plan_handler(self.env, request)" in runtime
    metrics = (ROOT / "app" / "src" / "api_metrics.py").read_text(encoding="utf-8")
    assert '("GET", "/api/preservation/crisis-plan",' in metrics
    footprint = (ROOT / "world" / "tools" / "build_worker_footprint.py").read_text(
        encoding="utf-8")
    assert '"crisis_preservation.py",' in footprint


def test_documentation_explains_every_scenario_reason_topic_and_budget():
    doc = DOC.read_text(encoding="utf-8")
    for scenario in cp.SCENARIOS:
        heading = "### `%s`" % scenario["id"]
        assert heading in doc, scenario["id"]
        section = doc.split(heading, 1)[1].split("\n### ", 1)[0]
        assert "Drill budget: **%d minutes**." % scenario["drillBudgetMinutes"] in section
        assert "Priority: %s." % ", ".join(
            "`%s`" % c for c in scenario["priority"]) in section
    for reason in cp.REASONS:
        assert "### `%s`" % reason in doc, reason
    for topic in cp.RISK_TOPICS:
        assert "### `%s`" % topic["id"] in doc, topic["id"]
    for state in cp.COVERAGE_STATES:
        assert "| `%s` |" % state in doc, state
    for code in cp.DRILL_FAILURES:
        assert "| `%s` |" % code in doc, code
    for route in cp.ROUTES:
        assert "| `%s` |" % route["id"] in doc, route["id"]
    readme = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(crisis-preservation.md)" in readme
    imports = (ROOT / "www" / "docs" / "repository-imports.md").read_text(encoding="utf-8")
    assert "(crisis-preservation.md)" in imports

    sys.path.insert(0, str(ROOT / "app" / "tools"))
    import docs_markdown

    rendered = docs_markdown.render(doc).html
    anchors = list(cp.SCENARIO_IDS) + list(cp.REASONS) + list(cp.TOPIC_IDS) + [
        "coverage-states", "drills"]
    for anchor in anchors:
        assert 'id="%s"' % anchor in rendered, anchor
