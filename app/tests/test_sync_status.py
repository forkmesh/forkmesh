"""forkmesh-sync-status-v1: reference model, shared fixture, and client parity.

The Go mirror node (server/syncstatus_test.go), desktop
(forkmesh-control-tests), and the native iOS and Android apps run
the same fixture in their own suites; this file pins the reference, the model
document, the CLI, the Worker API, and the web rendering.
"""

import json
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / "conformance" / "sync-status" / "v1"
sys.path.insert(0, str(ROOT / "app" / "src"))

import sync_status  # noqa: E402


MODEL = json.loads((SUITE / "model.json").read_text(encoding="utf-8"))
FIXTURE = json.loads((SUITE / "fixture.json").read_text(encoding="utf-8"))


def _entries():
    for case in FIXTURE["cases"]:
        yield case["id"], case["expect"], sync_status.evaluate(case["evidence"])
    for sequence in FIXTURE["sequences"]:
        for index, step in enumerate(sequence["steps"]):
            yield ("%s#%d" % (sequence["id"], index), step["expect"],
                   sync_status.evaluate(step["evidence"]))
    for case in FIXTURE["mirrorsPayloadCases"]:
        yield (case["id"], case["expect"],
               sync_status.mirrors_payload_status(case["payload"]))


def test_model_document_matches_the_reference_vocabulary():
    states = {state["id"]: state for state in MODEL["states"]}
    assert tuple(states) == sync_status.STATES
    assert set(states) == {
        "queued", "syncing", "verified", "stale", "degraded", "conflicted",
        "paused", "external-only", "failed",
    }
    for state_id, state in states.items():
        assert state["label"] == sync_status.LABELS[state_id]
        assert state["severity"] == sync_status.SEVERITY[state_id]
        assert state["headline"] == sync_status.HEADLINES[state_id]
        assert state["current"] is (state_id in ("verified", "degraded"))
    assert {e["code"]: e["action"] for e in MODEL["errors"]} == sync_status.ACTIONS
    assert tuple(MODEL["reportedErrors"]) == sync_status.REPORTED_ERRORS
    assert MODEL["policy"]["staleAfterMs"] == sync_status.STALE_AFTER_MS
    assert MODEL["policy"]["syncDeadlineMs"] == sync_status.SYNC_DEADLINE_MS
    assert MODEL["outputSchema"] == sync_status.SCHEMA
    for implementation in MODEL["implementations"]:
        assert (ROOT / implementation["path"]).is_file(), implementation
        assert (ROOT / implementation["test"]).is_file(), implementation


def test_committed_fixture_matches_reference_generation():
    completed = subprocess.run(
        [sys.executable, str(SUITE / "generate_fixture.py"), "--check"],
        capture_output=True, text=True, check=False, cwd=ROOT, timeout=60,
    )
    assert completed.returncode == 0, completed.stderr


@pytest.mark.parametrize("case_id,expected,actual", list(_entries()),
                         ids=lambda value: value if isinstance(value, str) else "")
def test_reference_matches_every_fixture_entry(case_id, expected, actual):
    assert actual == expected, case_id


def test_fixture_covers_every_state_and_failure_path():
    states = {expected["state"] for _, expected, _ in _entries()}
    assert states == set(sync_status.STATES)
    errors = {expected["error"] for _, expected, _ in _entries()}
    assert {
        "conflict", "rejected", "unreachable", "sync_delayed",
        "confirmation_expired", "unknown", "provider_unavailable",
    } <= errors
    for sequence in FIXTURE["sequences"]:
        # Every transition sequence ends recovered.
        assert sequence["steps"][-1]["expect"]["state"] == "verified", sequence["id"]


def test_no_false_success():
    for case_id, expected, _ in _entries():
        if expected["current"]:
            assert expected["verifiedCopies"] >= 1, case_id
        if expected["state"] == "verified":
            assert expected["verifiedCopies"] == expected["totalCopies"], case_id
            assert expected["error"] is None and expected["lagMs"] == 0, case_id
        else:
            assert expected["summary"].split(".")[0] != sync_status.HEADLINES["verified"]
        if expected["severity"] in ("warning", "error"):
            assert expected["action"], case_id


def test_status_never_echoes_private_evidence():
    assert FIXTURE["privateStrings"]
    for case_id, expected, _ in _entries():
        text = json.dumps(expected)
        for secret in FIXTURE["privateStrings"]:
            assert secret not in text, (case_id, secret)


def test_malformed_evidence_fails_closed():
    for evidence in (None, [], {"copies": "x"}, {"now": True, "copies": [None, 3]}):
        status = sync_status.evaluate(evidence)
        assert status["state"] == "stale" and status["current"] is False
    assert sync_status.mirrors_payload_status(None)["state"] == "stale"


def _cli(*args, stdin=None):
    return subprocess.run(
        [sys.executable, str(ROOT / "tools" / "forkmesh_sync_status.py"), *args],
        input=stdin, capture_output=True, text=True, check=False, cwd=ROOT,
        timeout=60,
    )


def test_cli_verifies_the_fixture_and_explains_evidence():
    completed = _cli("fixture")
    assert completed.returncode == 0, completed.stderr
    report = json.loads(completed.stdout)
    assert report["mismatches"] == [] and report["cases"] >= 40

    case = next(c for c in FIXTURE["cases"] if c["id"] == "rejected-signature")
    completed = _cli("explain", "--json", "-", stdin=json.dumps(case["evidence"]))
    assert completed.returncode == 3
    assert json.loads(completed.stdout) == case["expect"]

    case = next(c for c in FIXTURE["mirrorsPayloadCases"] if c["id"] == "mirrors-all-serving")
    completed = _cli("explain", "--mirrors", "-", stdin=json.dumps(case["payload"]))
    assert completed.returncode == 0
    assert completed.stdout.strip() == "Verified: " + case["expect"]["summary"]


def test_worker_mirrors_api_publishes_sync_status():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert '_LazyModule("sync_status")' in entry
    handler = entry[
        entry.index("async def repo_mirrors_handler"):
        entry.index("# --- Release download counts")
    ]
    assert 'payload["syncStatus"] = mirrors_payload_status(payload)' in handler


def test_web_renders_the_api_sync_status():
    fragment = (ROOT / "app" / "public" / "dashboard" / "js"
                / "07-repo-compose-branch.js").read_text(encoding="utf-8")
    bundle = (ROOT / "app" / "public" / "dashboard.js").read_text(encoding="utf-8")
    for source in (fragment, bundle):
        assert 'data.syncStatus.schema === "forkmesh-sync-status-v1"' in source
        assert "data-repo-sync-status" in source
        assert "escapeHtml(sync.summary)" in source
