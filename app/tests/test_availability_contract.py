"""forkmesh-availability-contract-report-v1: reference model, fixture, and CLI.

Pins the reference implementation to its model document and shared fixture,
and checks the contract's promises: only demonstrated capacity is admissible,
every SLO miss and remedy is visible and arithmetically consistent, a simulated
operator or regional failure keeps the promised quorum, reports reproduce from
their evidence and signatures, and private evidence never leaks.
"""

import base64
import copy
import json
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / "conformance" / "availability-contract" / "v1"
DOC = ROOT / "www" / "docs" / "availability-contract.md"
CLI = ROOT / "tools" / "forkmesh_availability_contract.py"
sys.path.insert(0, str(ROOT / "app" / "src"))

import availability_contract  # noqa: E402
import availability_proof  # noqa: E402


MODEL = json.loads((SUITE / "model.json").read_text(encoding="utf-8"))
FIXTURE = json.loads((SUITE / "fixture.json").read_text(encoding="utf-8"))
CASES = {case["id"]: case for case in FIXTURE["cases"]}


def test_model_document_matches_the_reference_vocabulary():
    assert MODEL["inputSchema"] == availability_contract.BUNDLE_SCHEMA
    assert MODEL["outputSchema"] == availability_contract.SCHEMA
    assert MODEL["signedSchema"] == availability_contract.SIGNED_SCHEMA
    assert MODEL["signatureDomain"] == availability_contract.SIGNATURE_DOMAIN
    policy = MODEL["policy"]
    assert policy["minVerificationIntervalMs"] == availability_contract.MIN_VERIFICATION_INTERVAL_MS
    assert policy["maxSlots"] == availability_contract.MAX_SLOTS
    assert policy["maxFailureTolerance"] == availability_contract.MAX_FAILURE_TOLERANCE
    assert policy["minContractedOperators"] == availability_proof.QUORUM_TARGET
    assert tuple(MODEL["transports"]) == availability_contract.TRANSPORTS
    assert tuple(MODEL["capacityClasses"]) == availability_contract.CAPACITY_CLASSES
    assert tuple(MODEL["exclusionKinds"]) == availability_contract.EXCLUSION_KINDS
    assert tuple(MODEL["exclusionReasons"]) == availability_contract.EXCLUSION_REASONS
    assert tuple(MODEL["checks"]) == availability_contract.CHECK_CODES
    assert tuple(MODEL["slos"]) == availability_contract.SLOS
    assert [(m["code"], m["slo"]) for m in MODEL["misses"]] == list(availability_contract.MISSES)
    assert tuple(MODEL["drillStates"]) == availability_contract.DRILL_STATES
    assert tuple(MODEL["responseStates"]) == availability_contract.RESPONSE_STATES
    assert tuple(MODEL["admissionReasons"]) == availability_contract.ADMISSION_REASONS
    assert MODEL["disclosures"] == [d["code"] for d in availability_contract.DISCLOSURES]
    terms = CASES["met-three-operators"]["expect"]["terms"]
    assert [term["id"] for term in MODEL["terms"]] == list(terms)
    for implementation in MODEL["implementations"]:
        assert (ROOT / implementation["path"]).is_file(), implementation
        assert (ROOT / implementation["test"]).is_file(), implementation


def test_committed_fixture_matches_reference_generation():
    completed = subprocess.run(
        [sys.executable, str(SUITE / "generate_fixture.py"), "--check"],
        capture_output=True, text=True, check=False, cwd=ROOT, timeout=120,
    )
    assert completed.returncode == 0, completed.stderr


@pytest.mark.parametrize("case_id", list(CASES))
def test_reference_reproduces_every_fixture_case(case_id):
    case = CASES[case_id]
    report = availability_contract.evaluate_contract(case["bundle"])
    assert report == case["expect"]
    assert report["outcome"] == case["expectOutcome"]
    assert report["admission"]["reasons"] == case["expectAdmissionReasons"]
    assert sorted(m["code"] for m in report["misses"]) == case["expectMissCodes"]
    assert sorted(d["code"] for d in report["disclosures"]) == case["expectDisclosures"]


def test_fixture_covers_every_code():
    reports = [case["expect"] for case in CASES.values()]
    assert {code for r in reports for code, n in r["checks"].items() if n} == set(
        availability_contract.CHECK_CODES)
    assert {m["code"] for r in reports for m in r["misses"]} == set(
        availability_contract.MISS_SLO)
    assert {n["drill"] for r in reports for n in r["capacity"]["nodes"]} == set(
        availability_contract.DRILL_STATES)
    assert {i["response"] for r in reports for i in r["incidents"]} == set(
        availability_contract.RESPONSE_STATES)
    assert {reason for r in reports for reason in r["admission"]["reasons"]} == set(
        availability_contract.ADMISSION_REASONS)
    assert {d["code"] for r in reports for d in r["disclosures"]} == {
        d["code"] for d in availability_contract.DISCLOSURES}
    assert {e["reason"] for r in reports for e in r["exclusions"] if e["reason"]} == set(
        availability_contract.EXCLUSION_REASONS)
    assert {s["scope"] for r in reports for s in r["simulations"]} == {
        "none", "operator", "region"}
    assert {r["outcome"] for r in reports} == {"met", "missed"}
    assert any(r["remedies"]["capped"] for r in reports)
    assert any(r["evidence"]["ignoredObservations"] for r in reports)


def test_contracts_admit_only_demonstrated_capacity():
    for case_id, case in CASES.items():
        report = case["expect"]
        terms = report["terms"]
        nodes = report["capacity"]["nodes"]
        demonstrated = [n for n in nodes if n["demonstrated"]]
        for node in nodes:
            assert node["demonstrated"] == (
                node["probesDemonstrated"] and node["drill"] == "ok"), (case_id, node)
            if node["probesDemonstrated"]:
                assert node["verifiedBps"] >= terms["uptimeBps"], (case_id, node)
        summary = report["capacity"]["demonstrated"]
        assert summary["copies"] == len(demonstrated)
        assert summary["operators"] == len({n["operator"] for n in demonstrated})
        for simulation in report["simulations"]:
            assert simulation["remainingCopies"] <= summary["copies"], case_id
        if report["admission"]["admissible"]:
            assert summary["copies"] >= terms["copies"], case_id
            assert summary["operators"] >= terms["operators"], case_id
            assert summary["regions"] >= terms["regions"], case_id
            assert report["simulations"] and all(
                s["passed"] for s in report["simulations"]), case_id
    # Verified copies with a slow or stale latest drill never count.
    drills = CASES["restore-drills-gate-capacity"]["expect"]
    assert [n["node"] for n in drills["capacity"]["nodes"] if n["demonstrated"]] == ["mirror-a"]
    assert drills["checks"] == {code: 0 for code in availability_contract.CHECK_CODES}


def test_every_miss_and_remedy_is_visible_and_consistent():
    for case_id, case in CASES.items():
        report = case["expect"]
        slots = report["slots"]
        assert slots["counted"] == slots["total"] - slots["excluded"]
        assert slots["met"] + slots["missed"] == slots["counted"]
        assert sum(i["slots"] for i in report["incidents"]) == slots["missed"], case_id
        misses = {m["id"]: m for m in report["misses"]}
        for incident in report["incidents"]:
            linked = [m["code"] for m in report["misses"] if m["incident"] == incident["id"]]
            assert linked.count("availability_window") == 1, case_id
            assert ("recovery_objective_exceeded" in linked) == (
                not incident["recoveryWithinObjective"]), case_id
            assert ("response_" + incident["response"] in linked) == (
                incident["response"] in ("late", "missing")), case_id
            assert incident["codes"] and incident["recoveryMs"] >= incident["end"] - incident["start"]

        slos = {slo["id"]: slo for slo in report["slos"]}
        availability = slos["availability"]
        expected = slots["met"] * 10000 // slots["counted"] if slots["counted"] else 0
        assert availability["attained"] == expected
        assert availability["met"] == (slots["counted"] > 0 and expected >= availability["target"])
        for slo in ("recovery", "incident-response", "restore-drills"):
            assert slos[slo]["met"] == (not any(m["slo"] == slo for m in misses.values()))
        assert (report["outcome"] == "met") == all(slo["met"] for slo in report["slos"])

        remedies = report["remedies"]
        lines = {line["id"]: line for line in remedies["lines"]}
        for miss in misses.values():
            if miss["slo"] != "availability" or not availability["met"]:
                assert miss["remedy"] in lines, (case_id, miss)
                assert miss["id"] in lines[miss["remedy"]]["misses"]
            else:
                assert miss["remedy"] is None, (case_id, miss)
        fee = remedies["feeMicrousd"]
        for line in lines.values():
            assert line["creditMicrousd"] == fee * line["creditBps"] // 10000
            assert all(misses[miss_id]["remedy"] == line["id"] for miss_id in line["misses"])
        assert remedies["uncappedCreditBps"] == sum(l["creditBps"] for l in lines.values())
        assert remedies["totalCreditBps"] == min(
            remedies["uncappedCreditBps"], remedies["terms"]["maxCreditBps"])
        assert remedies["capped"] == (remedies["uncappedCreditBps"] > remedies["totalCreditBps"])
        assert remedies["totalCreditMicrousd"] == fee * remedies["totalCreditBps"] // 10000
    # A listed miss does not become a credit when its SLO is still met.
    exclusions = CASES["exclusions-late-unknown-over-budget"]["expect"]
    assert [m["code"] for m in exclusions["misses"]] == ["availability_window"]
    assert exclusions["remedies"]["lines"] == []


def test_simulated_regional_or_operator_failure_keeps_the_promised_quorum():
    report = CASES["regional-outage-retains-quorum"]["expect"]
    promised = report["terms"]["operators"]
    simulations = report["simulations"]
    assert {s["scope"] for s in simulations} == {"operator", "region"}
    assert len([s for s in simulations if s["scope"] == "operator"]) == 4
    assert sorted(s["failed"][0] for s in simulations if s["scope"] == "region") == [
        "eu-west", "us-east"]
    for simulation in simulations:
        assert simulation["passed"] and simulation["retained"]
        assert simulation["quorumOperators"] >= promised
        assert simulation["recoveryMs"] <= report["terms"]["recoveryTimeObjectiveMs"]
    assert report["admission"] == {"admissible": True, "reasons": []}
    # The live regional outage loses copies and region diversity, but never
    # the operator quorum, and recovers inside the objective.
    [incident] = report["incidents"]
    assert "quorum_below_contract" not in incident["codes"]
    assert incident["recoveryWithinObjective"] and incident["response"] == "ok"
    assert report["outcome"] == "met"

    beyond = CASES["operator-failure-beyond-tolerance"]["expect"]
    failed = [s for s in beyond["simulations"] if not s["passed"]]
    assert failed and all(not s["retained"] for s in failed)
    assert beyond["outcome"] == "met" and not beyond["admission"]["admissible"]

    # State disagreement cannot manufacture a quorum under failure either.
    disagreement = CASES["state-disagreement-no-false-quorum"]["expect"]
    assert any(s["scope"] == "operator" and not s["retained"]
               for s in disagreement["simulations"])


def test_reports_never_echo_private_evidence():
    assert FIXTURE["privateStrings"]
    for case_id, case in CASES.items():
        text = json.dumps(case["expect"])
        for secret in FIXTURE["privateStrings"]:
            assert secret not in text, (case_id, secret)
    # Every planted string really is in the evidence, so the check above bites.
    bundles = json.dumps([case["bundle"] for case in CASES.values()])
    assert all(secret in bundles for secret in FIXTURE["privateStrings"])


def test_evidence_digests_change_when_any_evidence_changes():
    case = CASES["met-three-operators"]
    base = case["expect"]["evidence"]
    for field, mutate in (
        ("observationsSha256", lambda b: b["observations"][0]["mirrors"][0].update(checkedAt=1)),
        ("drillsSha256", lambda b: b["drills"][0].update(durationMs=1)),
        ("capacitySha256", lambda b: b["capacity"][0].update({"class": "contracted"})),
        ("contractSha256", lambda b: b["contract"].update(feeMicrousd=1)),
        ("responsesSha256", lambda b: b["responses"].append({"incidentStart": 1})),
    ):
        bundle = copy.deepcopy(case["bundle"])
        mutate(bundle)
        assert availability_contract.evaluate_contract(bundle)["evidence"][field] != base[field], field


@pytest.mark.parametrize("mutate,code", [
    (lambda b: b.update(schema="other"), "invalid_schema"),
    (lambda b: b.update(contract=None), "invalid_contract"),
    (lambda b: b["contract"].update(id=""), "invalid_id"),
    (lambda b: b["contract"].update(periodEnd=b["contract"]["periodStart"]), "invalid_periodEnd"),
    (lambda b: b["contract"].update(feeMicrousd=-1), "invalid_feeMicrousd"),
    (lambda b: b["contract"]["terms"].update(operators=1), "invalid_operators"),
    (lambda b: b["contract"]["terms"].update(operators=4), "invalid_operators"),
    (lambda b: b["contract"]["terms"].update(uptimeBps=10001), "invalid_uptimeBps"),
    (lambda b: b["contract"]["terms"].update(uptimeBps=True), "invalid_uptimeBps"),
    (lambda b: b["contract"]["terms"].update(verificationIntervalMs=1000), "invalid_verificationIntervalMs"),
    (lambda b: b["contract"]["terms"].update(verificationIntervalMs=60000), "too_many_slots"),
    (lambda b: b["contract"]["terms"].update(transports=["ftp"]), "invalid_transports"),
    (lambda b: b["contract"]["terms"].update(refCoverage="yes"), "invalid_refCoverage"),
    (lambda b: b["contract"]["terms"].update(failureTolerance=3), "invalid_failureTolerance"),
    (lambda b: b["contract"]["remedies"]["availabilityTiers"].append(
        {"belowBps": 9950, "creditBps": 100}), "invalid_belowBps"),
    (lambda b: b["contract"]["remedies"].update(maxCreditBps=None), "invalid_maxCreditBps"),
    (lambda b: b["contract"]["exclusions"].append({"kind": "legal-hold"}), "invalid_start"),
])
def test_malformed_contracts_fail_closed(mutate, code):
    bundle = copy.deepcopy(CASES["met-three-operators"]["bundle"])
    if code == "too_many_slots":
        bundle["contract"]["periodEnd"] = bundle["contract"]["periodStart"] + 10001 * 60000
    mutate(bundle)
    with pytest.raises(availability_contract.ContractError) as raised:
        availability_contract.evaluate_contract(bundle)
    assert raised.value.code == code


def test_malformed_evidence_is_ignored_not_trusted():
    bundle = copy.deepcopy(CASES["met-three-operators"]["bundle"])
    bundle["capacity"] = [None, {"node": "mirror-b", "class": "paid"}]
    bundle["observations"] = [None, {"generatedAt": "soon"}]
    bundle["drills"] = [None, {"drill": True, "node": "mirror-a", "completedAt": "yesterday",
                               "durationMs": 1, "repositories": 1},
                        {"drill": False, "node": "mirror-a", "completedAt": 1,
                         "durationMs": 1, "repositories": 1}]
    bundle["responses"] = "later"
    report = availability_contract.evaluate_contract(bundle)
    assert report["capacity"]["nodes"] == [dict(
        report["capacity"]["nodes"][0], node="mirror-b", **{"class": "community"})]
    assert report["evidence"]["drills"] == 0
    assert report["evidence"]["ignoredObservations"] == 2
    assert report["slots"]["met"] == 0 and report["outcome"] == "missed"
    assert "no_evidence" in report["admission"]["reasons"]


def test_go_restore_drill_timestamps_parse_to_milliseconds():
    parse = availability_contract._timestamp_ms
    assert parse("2027-01-15T08:00:00.123456789Z") == 1800000000123
    assert parse("2027-01-15T10:00:00+02:00") == 1800000000000
    assert parse("2027-01-15T08:00:00Z") == 1800000000000
    assert parse("2027-02-30T08:00:00Z") is None
    assert parse(True) is None and parse(0) is None


def _cli(*args, stdin=None):
    return subprocess.run(
        [sys.executable, str(CLI), *args], input=stdin, capture_output=True,
        text=True, check=False, cwd=ROOT, timeout=120,
    )


def test_cli_explains_and_verifies_the_fixture():
    completed = _cli("fixture")
    assert completed.returncode == 0, completed.stderr
    assert json.loads(completed.stdout) == {"cases": len(CASES), "mismatches": []}

    met = CASES["met-three-operators"]
    completed = _cli("explain", "-", stdin=json.dumps(met["bundle"]))
    assert completed.returncode == 0
    assert completed.stdout.strip() == met["expect"]["summary"]

    missed = CASES["single-operator-not-admissible"]
    completed = _cli("explain", "--json", "-", stdin=json.dumps(missed["bundle"]))
    assert completed.returncode == 3
    assert json.loads(completed.stdout) == missed["expect"]

    assert _cli("explain", "-", stdin="not json").returncode == 2
    invalid = copy.deepcopy(met["bundle"])
    invalid["contract"]["terms"]["operators"] = 1
    completed = _cli("explain", "-", stdin=json.dumps(invalid))
    assert completed.returncode == 2 and "invalid_operators" in completed.stderr


def test_cli_signed_reports_reproduce_and_reject_tampering(tmp_path):
    ed25519 = pytest.importorskip("cryptography.hazmat.primitives.asymmetric.ed25519")
    serialization = pytest.importorskip("cryptography.hazmat.primitives.serialization")

    def encode(raw):
        return base64.urlsafe_b64encode(raw).rstrip(b"=").decode("ascii")

    key = ed25519.Ed25519PrivateKey.generate()
    public = encode(key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))
    other = encode(ed25519.Ed25519PrivateKey.generate().public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))
    seed = tmp_path / "seed.txt"
    seed.write_text(encode(key.private_bytes_raw()), encoding="utf-8")

    case = CASES["unverified-slots-late-response"]
    bundle = tmp_path / "bundle.json"
    bundle.write_text(json.dumps(case["bundle"]), encoding="utf-8")
    completed = _cli("sign", str(bundle), "--private-key", str(seed), "--key-id", "test")
    assert completed.returncode == 0, completed.stderr
    signed = json.loads(completed.stdout)
    assert signed["schema"] == availability_contract.SIGNED_SCHEMA
    assert signed["report"] == case["expect"]
    assert signed["reportHash"] == availability_contract.report_hash(case["expect"])
    assert signed["signature"]["publicKey"] == public
    signed_path = tmp_path / "signed.json"
    signed_path.write_text(json.dumps(signed), encoding="utf-8")

    completed = _cli("verify", str(bundle), str(signed_path), "--public-key", public)
    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.startswith("reproduced: ")
    assert "not pinned" in _cli("verify", str(bundle), str(signed_path)).stdout

    wrong_key = _cli("verify", str(bundle), str(signed_path), "--public-key", other)
    assert wrong_key.returncode == 1 and "different key" in wrong_key.stderr

    # Hide a miss and its credit, re-hash honestly: the report no longer
    # reproduces from the evidence and the signature no longer matches.
    tampered = copy.deepcopy(signed)
    tampered["report"]["misses"] = tampered["report"]["misses"][:1]
    tampered["report"]["remedies"]["totalCreditBps"] = 0
    tampered["reportHash"] = availability_contract.report_hash(tampered["report"])
    tampered_path = tmp_path / "tampered.json"
    tampered_path.write_text(json.dumps(tampered), encoding="utf-8")
    completed = _cli("verify", str(bundle), str(tampered_path), "--public-key", public)
    assert completed.returncode == 1
    assert "does not reproduce" in completed.stderr
    assert "signature does not verify" in completed.stderr

    # Evidence edited after signing is caught too.
    edited = copy.deepcopy(case["bundle"])
    edited["responses"] = []
    edited_path = tmp_path / "edited.json"
    edited_path.write_text(json.dumps(edited), encoding="utf-8")
    completed = _cli("verify", str(edited_path), str(signed_path), "--public-key", public)
    assert completed.returncode == 1 and "does not reproduce" in completed.stderr


def test_documentation_explains_every_code_and_is_linked():
    doc = DOC.read_text(encoding="utf-8")
    codes = (
        list(availability_contract.CHECK_CODES)
        + list(availability_contract.MISS_SLO)
        + list(availability_contract.ADMISSION_REASONS)
        + list(availability_contract.EXCLUSION_REASONS)
        + list(availability_contract.EXCLUSION_KINDS)
        + list(availability_contract.SLOS)
        + [term["id"] for term in MODEL["terms"]]
    )
    for code in codes:
        assert "`%s`" % code in doc, code
    for disclosure in availability_contract.DISCLOSURES:
        assert "### `%s`" % disclosure["code"] in doc, disclosure["code"]
        assert disclosure["title"] and disclosure["detail"].endswith(".")
    for case in CASES.values():
        for disclosure in case["expect"]["disclosures"]:
            assert disclosure["href"] == availability_contract.DOCS_URL + "#" + disclosure["code"]
    readme = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(availability-contract.md)" in readme
    business = (ROOT / "www" / "docs" / "business-model.md").read_text(encoding="utf-8")
    assert "(availability-contract.md)" in business
    assert "app/tests/test_availability_contract.py" in business

    sys.path.insert(0, str(ROOT / "app" / "tools"))
    import docs_markdown

    rendered = docs_markdown.render(doc).html
    for disclosure in availability_contract.DISCLOSURES:
        assert 'id="%s"' % disclosure["code"] in rendered, disclosure["code"]


def test_reference_stays_out_of_worker_startup():
    footprint = (ROOT / "world" / "tools" / "build_worker_footprint.py").read_text(encoding="utf-8")
    assert '"availability_contract.py",' in footprint
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert "availability_contract" not in entry
