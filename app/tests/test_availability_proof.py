"""forkmesh-availability-proof-v1: reference model, shared fixture, and clients.

Pins the reference implementation to its model document and fixture, and checks
the CLI, the Worker mirrors API wiring, the web rendering, and the user
documentation against the same vocabulary.
"""

import json
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / "conformance" / "availability-proof" / "v1"
DOC = ROOT / "www" / "docs" / "availability-proof.md"
sys.path.insert(0, str(ROOT / "app" / "src"))

import availability_proof  # noqa: E402


MODEL = json.loads((SUITE / "model.json").read_text(encoding="utf-8"))
FIXTURE = json.loads((SUITE / "fixture.json").read_text(encoding="utf-8"))


def _entries():
    for case in FIXTURE["cases"]:
        yield case["id"], case
    for sequence in FIXTURE["sequences"]:
        for index, step in enumerate(sequence["steps"]):
            yield "%s#%d" % (sequence["id"], index), step


ENTRIES = list(_entries())


def test_model_document_matches_the_reference_vocabulary():
    assert MODEL["outputSchema"] == availability_proof.SCHEMA
    policy = MODEL["policy"]
    assert policy["freshnessObjectiveMs"] == availability_proof.FRESHNESS_OBJECTIVE_MS
    assert policy["probeFreshMs"] == availability_proof.PROBE_FRESH_MS
    assert policy["quorumTarget"] == availability_proof.QUORUM_TARGET
    assert {c["id"] for c in MODEL["copyClasses"]} == set(availability_proof.COPY_CLASSES)
    assert tuple(MODEL["reasons"]) == availability_proof.REASONS
    assert [p["id"] for p in MODEL["proofStates"]] == list(availability_proof.PROOF_STATES)
    for state in MODEL["proofStates"]:
        assert state["label"] == availability_proof.PROOF_LABELS[state["id"]]
        assert state["headline"] == availability_proof.PROOF_HEADLINES[state["id"]]
    assert [(l["minScore"], l["id"], l["label"]) for l in MODEL["levels"]] == list(
        availability_proof.LEVELS)
    assert [
        (r["code"], r["severity"], r["weight"], r["action"]) for r in MODEL["risks"]
    ] == [
        (r["code"], r["severity"], r["weight"], r["action"]["kind"])
        for r in availability_proof.RISKS
    ]
    for implementation in MODEL["implementations"]:
        assert (ROOT / implementation["path"]).is_file(), implementation
        assert (ROOT / implementation["test"]).is_file(), implementation


def test_freshness_objective_matches_the_router_receipt_window():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert "HTTPS_MIRROR_STATUS_FRESH_MS = 10 * 60 * 1000" in entry
    assert availability_proof.FRESHNESS_OBJECTIVE_MS == 10 * 60 * 1000
    mirrors = (ROOT / "app" / "src" / "mirrors.py").read_text(encoding="utf-8")
    assert "PEER_QUORUM_MIN_NODES = %d" % availability_proof.QUORUM_TARGET in mirrors


def test_committed_fixture_matches_reference_generation():
    completed = subprocess.run(
        [sys.executable, str(SUITE / "generate_fixture.py"), "--check"],
        capture_output=True, text=True, check=False, cwd=ROOT, timeout=60,
    )
    assert completed.returncode == 0, completed.stderr


@pytest.mark.parametrize("case_id,entry", ENTRIES, ids=[e[0] for e in ENTRIES])
def test_reference_reproduces_every_fixture_entry(case_id, entry):
    report = availability_proof.evaluate_mirrors_payload(entry["payload"])
    assert report == entry["expect"], case_id
    assert report["proof"] == entry["expectProof"], case_id
    assert sorted(r["code"] for r in report["risks"]) == sorted(entry["expectRisks"])
    assert [c["class"] for c in report["copies"]] == entry["expectClasses"]


def test_fixture_covers_every_class_proof_and_risk():
    reports = [entry["expect"] for _, entry in ENTRIES]
    assert {c["class"] for r in reports for c in r["copies"]} == set(
        availability_proof.COPY_CLASSES)
    assert {r["proof"] for r in reports} == set(availability_proof.PROOF_STATES)
    assert {k["code"] for r in reports for k in r["risks"]} == {
        risk["code"] for risk in availability_proof.RISKS}
    assert {reason for r in reports for c in r["copies"] for reason in c["reasons"]} == set(
        availability_proof.REASONS)
    assert {r["level"] for r in reports} >= {"low", "moderate", "critical"}


def test_every_failure_sequence_ends_recovered_and_changes_predictably():
    ids = {sequence["id"] for sequence in FIXTURE["sequences"]}
    assert {"mirror-outage-recovers", "source-host-outage-recovers",
            "integrity-failure-recovers"} <= ids
    for sequence in FIXTURE["sequences"]:
        steps = [step["expect"] for step in sequence["steps"]]
        assert steps[0]["proof"] == "quorum" and steps[0]["risks"] == []
        assert any(step["risks"] for step in steps[1:-1]), sequence["id"]
        assert steps[-1]["proof"] == "quorum" and steps[-1]["risks"] == [], sequence["id"]
        assert steps[-1]["score"] == 0


def test_no_false_quorum():
    weights = {risk["code"]: risk["weight"] for risk in availability_proof.RISKS}
    for case_id, entry in ENTRIES:
        report = entry["expect"]
        quorum = report["quorum"]
        verified = [c for c in report["copies"] if c["class"] == "verified"]
        if quorum["met"]:
            assert report["proof"] == "quorum", case_id
            # Distinct operators among verified copies, never nodes.
            assert len({c["operator"] for c in verified}) >= quorum["target"], case_id
            assert quorum["operators"] >= quorum["target"], case_id
        else:
            assert "quorum_not_met" in {r["code"] for r in report["risks"]}, case_id
        assert quorum["copies"] <= len(verified), case_id
        assert report["score"] == min(100, sum(weights[r["code"]] for r in report["risks"]))
        assert report["risks"] == sorted(report["risks"], key=lambda r: -r["weight"])
    case = next(c for _, c in ENTRIES if c.get("id") == "single-operator-no-false-quorum")
    assert case["expect"]["quorum"] == {"met": False, "target": 2, "operators": 1, "copies": 2}


def test_source_host_outage_does_not_remove_a_mirror_quorum():
    base = next(c for _, c in ENTRIES if c.get("id") == "quorum-two-operators")["expect"]
    outage = next(c for _, c in ENTRIES
                  if c.get("id") == "source-offline-mirrors-hold-quorum")["expect"]
    assert outage["quorum"] == base["quorum"]
    assert outage["score"] == base["score"] + 10
    assert [r["code"] for r in outage["risks"]] == ["source_offline"]


def test_report_never_echoes_private_evidence():
    assert FIXTURE["privateStrings"]
    for case_id, entry in ENTRIES:
        text = json.dumps(entry["expect"])
        payload = json.dumps(entry["payload"])
        for secret in FIXTURE["privateStrings"]:
            assert secret not in text, (case_id, secret)
        assert any(secret in payload for secret in FIXTURE["privateStrings"]) or (
            entry["expect"]["counts"]["total"] == 0) or case_id.startswith(
                "source-offline-declared-only")


def test_every_risk_is_explained_with_a_reachable_remediation():
    doc = DOC.read_text(encoding="utf-8")
    assert "## Risk codes" in doc
    for risk in availability_proof.RISKS:
        assert "### `%s`" % risk["code"] in doc, risk["code"]
        section = doc.split("### `%s`" % risk["code"], 1)[1].split("\n### ", 1)[0]
        assert "Weight %d, %s." % (risk["weight"], risk["severity"]) in section
        assert "**Remediation:**" in section
        assert risk["title"] and risk["detail"].endswith(".")
        assert risk["action"]["label"]
    for _, entry in ENTRIES:
        for risk in entry["expect"]["risks"]:
            assert risk["action"]["href"] == (
                "https://forkmesh.com/docs/availability-proof/#" + risk["code"])
    readme = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(availability-proof.md)" in readme


def test_docs_anchors_resolve_for_every_risk_code():
    sys.path.insert(0, str(ROOT / "app" / "tools"))
    import docs_markdown

    rendered = docs_markdown.render(DOC.read_text(encoding="utf-8")).html
    for risk in availability_proof.RISKS:
        assert 'id="%s"' % risk["code"] in rendered, risk["code"]


def test_malformed_evidence_fails_closed():
    for payload in (None, [], {"mirrors": "x"}, {"generatedAt": True, "mirrors": [None, 3]}):
        report = availability_proof.evaluate_mirrors_payload(payload)
        assert report["proof"] == "none"
        assert report["quorum"]["met"] is False
        assert report["level"] == "critical"


def _cli(*args, stdin=None):
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "forkmesh_availability_proof.py"), *args],
        input=stdin, capture_output=True, text=True, check=False, cwd=ROOT,
        timeout=60,
    )


def test_cli_verifies_fixture_explains_and_reproduces_published_reports():
    completed = _cli("fixture")
    assert completed.returncode == 0, completed.stderr
    report = json.loads(completed.stdout)
    assert report["mismatches"] == [] and report["cases"] == len(ENTRIES)

    proven = next(c for _, c in ENTRIES if c.get("id") == "quorum-two-operators")
    completed = _cli("explain", "-", stdin=json.dumps(proven["payload"]))
    assert completed.returncode == 0
    assert completed.stdout.strip() == "Quorum proven: " + proven["expect"]["summary"]

    at_risk = next(c for _, c in ENTRIES if c.get("id") == "mirror-integrity-rejected")
    completed = _cli("explain", "--json", "-", stdin=json.dumps(at_risk["payload"]))
    assert completed.returncode == 3
    assert json.loads(completed.stdout) == at_risk["expect"]

    published = dict(at_risk["payload"], availability=at_risk["expect"])
    assert _cli("verify", "-", stdin=json.dumps(published)).returncode == 0
    tampered = dict(published, availability=dict(at_risk["expect"], score=0))
    assert _cli("verify", "-", stdin=json.dumps(tampered)).returncode == 1
    assert _cli("explain", "-", stdin="not json").returncode == 2


def test_worker_mirrors_api_publishes_availability_after_the_probe_overlay():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert '_LazyModule("availability_proof")' in entry
    handler = entry[
        entry.index("async def repo_mirrors_handler"):
        entry.index("# --- Release download counts")
    ]
    line = 'payload["availability"] = mirrors_payload_availability(payload)'
    assert line in handler
    # Computed after every endpoint receipt / direct probe field is overlaid.
    assert handler.index("**direct_fields,") < handler.index(line)
    assert handler.index(line) < handler.index("json_response(payload, cache_seconds=3)")


def test_web_renders_the_api_availability_proof():
    fragment = (ROOT / "app" / "public" / "dashboard" / "js"
                / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    bundle = (ROOT / "app" / "public" / "dashboard.js").read_text(encoding="utf-8")
    for source in (fragment, bundle):
        assert 'data.availability.schema === "forkmesh-availability-proof-v1"' in source
        assert "data-repo-availability-proof" in source
        assert "data-repo-availability-risk" in source
        assert "data-repo-availability-action" in source
        assert "data-repo-availability-evidence" in source
        assert "${renderRepoAvailabilityProof()}" in source
        body = source[
            source.index("function availabilityDocsHref("):
            source.index("function renderRepoMirrorLists(")
        ]
        # Everything from the API is escaped, and links only leave for the docs.
        assert "escapeHtml(proof.summary)" in body
        assert "escapeHtml(risk.detail)" in body
        assert 'startsWith("https://forkmesh.com/docs/")' in body
        # Operators are shown by opaque number only.
        assert "ownerUser" not in body
