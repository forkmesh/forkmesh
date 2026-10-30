"""Portable social signals v1 (issue #493): both implementations, vectors, CLI."""

from __future__ import annotations

import ast
import copy
import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "social_signals.py"
VECTORS = ROOT / "conformance" / "social-signals" / "v1" / "vectors.json"
DOC = ROOT / "www" / "docs" / "social-signals.md"

spec = importlib.util.spec_from_file_location("social_signals", TOOL)
social = importlib.util.module_from_spec(spec)
sys.modules["social_signals"] = social
spec.loader.exec_module(social)


@pytest.fixture(scope="module")
def vectors():
    return json.loads(VECTORS.read_text(encoding="utf-8"))


@pytest.fixture(scope="module")
def scenario_store(vectors):
    scenario = vectors["scenarios"][0]
    store = social.SignalStore()
    for bundle in scenario["bundles"]:
        store.import_bundle(bundle, scenario["asOf"])
    return store


def _seed(vectors, name):
    return bytes.fromhex(vectors["keys"][name]["seed"])


def _bundle(signals=(), revocations=(), exported_at=0):
    return {"schema": social.BUNDLE_SCHEMA, "exportedAt": exported_at,
            "signals": list(signals), "revocations": list(revocations)}


def test_committed_vectors_match_reference_generation():
    assert social._read_vectors_text() == social._render_vectors(social.build_vectors())


def test_python_implementation_passes_every_vector(vectors):
    results = social.run_vectors(vectors)
    assert results and all(item["ok"] for item in results), [item for item in results if not item["ok"]]


def test_javascript_implementation_passes_the_same_vectors(vectors):
    assert shutil.which("node"), "Node.js is required by the social-signal gate"
    output = social.run_javascript("vectors")
    results = output["results"]
    assert all(item["ok"] for item in results), [item for item in results if not item["ok"]][:5]
    python_ids = [(item["group"], item["id"]) for item in social.run_vectors(vectors)]
    assert [(item["group"], item["id"]) for item in results] == python_ids


def test_signals_round_trip_between_python_and_javascript(vectors):
    results = social.interop_round_trip(vectors)
    assert {item["id"] for item in results} >= {
        "python-verifies-javascript-bundle",
        "javascript-revocation-applies-in-python",
        "javascript-signature-verifies-in-python",
        "states-agree",
    }
    assert all(item["ok"] for item in results), results


def test_python_verifier_uses_only_the_standard_library():
    tree = ast.parse(TOOL.read_text(encoding="utf-8"))
    imported = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            imported.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom):
            imported.add((node.module or "").split(".")[0])
    assert imported <= set(sys.stdlib_module_names) | {"__future__"}


def test_signature_matches_rfc8032_test_vector_one():
    seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
    assert social.b64url_decode(social.public_key(seed), 32, "x").hex() == (
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
    assert social.ed25519_sign(seed, b"").hex() == (
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555"
        "fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")


# The vectors are produced by the Python reference, so pin the load-bearing
# semantics by hand as well: a generator bug must not grade itself.
@pytest.mark.parametrize(("case_id", "error"), [
    ("reject-star-claims-attestation-tier", "tier_mismatch"),
    ("reject-endorsement-claims-interest-tier", "tier_mismatch"),
    ("reject-star-with-scope", "interest_claims_scope"),
    ("reject-star-with-evidence", "interest_claims_scope"),
    ("reject-endorsement-without-evidence", "evidence_required"),
    ("reject-evidence-plain-http", "invalid_evidence"),
    ("reject-mirror-without-expiry", "commitment_requires_expiry"),
    ("reject-self-follow", "self_follow"),
    ("reject-note-edited-after-signing", "signal_id_mismatch"),
    ("reject-signer-substituted", "invalid_signature"),
    ("reject-small-order-signer", "invalid_signature"),
    ("reject-signature-s-plus-l", "invalid_signature"),
    ("reject-fractional-timestamp", "invalid_number"),
    ("reject-issued-in-future", "issued_in_future"),
])
def test_rejections_carry_the_specified_code(vectors, case_id, error):
    case = next(item for item in vectors["records"] if item["id"] == case_id)
    assert case["expect"] == {"valid": False, "error": error}


def test_high_trust_endorsements_display_scope_and_evidence(vectors):
    summaries = {item["id"]: item["expect"].get("summary") for item in vectors["records"]}
    assert summaries["interest-star"] == "Star"
    review = summaries["attestation-security-review"]
    assert review.startswith("Security review: manual-audit of ") and "findings resolved (2 evidence)" in review
    assert summaries["attestation-usage-endorsement"] == "Usage endorsement: production use of 2.4.1 (1 evidence)"
    assert summaries["attestation-mirror-commitment"].startswith("Mirror commitment: https://")
    for kind, tier in social.TIERS.items():
        assert (tier == "attestation") == (kind in social.SCOPE_KEYS)


def test_merge_is_order_independent_and_reports_rejections_without_aborting(vectors, scenario_store):
    scenario = vectors["scenarios"][0]
    reports = scenario["expect"]["imports"]
    assert reports[1]["duplicate"] == 1
    assert {item["error"] for item in reports[3]["rejected"]} == {"signal_id_mismatch", "invalid_reason"}
    assert reports[3]["accepted"] == 3
    digests = set()
    for order in ([3, 2, 1, 0], [1, 3, 0, 2, 1, 3]):
        store = social.SignalStore()
        for index in order:
            store.import_bundle(scenario["bundles"][index], scenario["asOf"])
        digests.add(store.digest())
    assert digests == {scenario_store.digest()}


def test_revocation_applies_only_for_the_original_signer(vectors, scenario_store):
    state = scenario_store.fold(vectors["scenarios"][0]["asOf"])
    statuses = sorted(item["status"] for item in state["revocations"])
    assert statuses == ["applied", "pending", "signer_mismatch"]
    by_id = {item["signalId"]: item for item in state["signals"]}
    mismatch = next(item for item in state["revocations"] if item["status"] == "signer_mismatch")
    assert by_id[mismatch["revokes"]]["status"] == "active"
    applied = next(item for item in state["revocations"] if item["status"] == "applied")
    assert by_id[applied["revokes"]]["status"] == "revoked"


def test_pending_revocation_applies_when_the_record_arrives_later(vectors):
    bob = _seed(vectors, "bob")
    as_of = vectors["scenarios"][0]["asOf"]
    star = social.sign_signal(bob, {"kind": "star", "subject": social._repository("later"), "issuedAt": as_of - 10})
    revocation = social.sign_revocation(bob, star["signalId"], as_of - 5)
    store = social.SignalStore()
    store.import_bundle(_bundle(revocations=[revocation]), as_of)
    assert store.fold(as_of)["revocations"][0]["status"] == "pending"
    store.import_bundle(_bundle(signals=[star]), as_of)
    assert store.fold(as_of)["signals"][0]["status"] == "revoked"


def test_revoking_the_latest_record_does_not_resurrect_an_older_one(vectors):
    carol = _seed(vectors, "carol")
    subject = social._repository("resurrect")
    old = social.sign_signal(carol, {"kind": "star", "subject": subject, "issuedAt": 1000})
    new = social.sign_signal(carol, {"kind": "star", "subject": subject, "issuedAt": 2000})
    store = social.SignalStore()
    store.import_bundle(_bundle([old, new], [social.sign_revocation(carol, new["signalId"], 3000)]), 10_000)
    assert {item["issuedAt"]: item["status"] for item in store.fold(10_000)["signals"]} == {
        1000: "superseded", 2000: "revoked"}


def test_exports_respect_signed_visibility(vectors, scenario_store):
    as_of = vectors["scenarios"][0]["asOf"]
    for audience, allowed in social.AUDIENCES.items():
        bundle = scenario_store.export_bundle(audience, as_of)
        assert {item["visibility"] for item in bundle["signals"]} <= set(allowed)
    public = scenario_store.export_bundle("public", as_of)
    assert "private" not in {item["visibility"] for item in public["signals"]}
    self_bundle = scenario_store.export_bundle("self", as_of)
    assert any(item["visibility"] == "private" for item in self_bundle["signals"])
    widened = copy.deepcopy(next(item for item in self_bundle["signals"] if item["visibility"] == "private"))
    widened["visibility"] = "public"
    report = social.SignalStore().import_bundle(_bundle([widened]), as_of)
    assert report["rejected"][0]["error"] == "signal_id_mismatch"


def test_recommendations_can_be_disabled_and_explain_themselves(vectors):
    outputs = {case["id"]: case["output"] for case in vectors["scenarios"][0]["expect"]["recommendations"]}
    assert outputs["disabled"]["enabled"] is False and outputs["disabled"]["recommendations"] == []
    assert outputs["disabled"]["considered"] == 0
    top = outputs["default"]["recommendations"][0]
    assert top["supporters"] == 2 and top["score"] == 6
    for reason in top["reasons"]:
        assert {"signalId", "signer", "kind", "tier", "weight", "summary", "scope", "evidence"} <= set(reason)
        assert reason["tier"] == "attestation" and reason["evidence"]
    assert outputs["stranger-viewer"]["recommendations"] == []
    assert outputs["default"]["excluded"]["untrusted"] >= 1
    muted = vectors["keys"]["carol"]["publicKey"]
    assert all(reason["signer"] != muted for entry in outputs["mute-carol"]["recommendations"]
               for reason in entry["reasons"])


def test_sybil_keys_and_repeated_records_cannot_inflate_a_score(vectors):
    as_of = vectors["scenarios"][0]["asOf"]
    alice, bob = _seed(vectors, "alice"), _seed(vectors, "bob")
    subject = social._repository("sybil")
    records = [social.sign_signal(alice, {"kind": "follow", "issuedAt": 1,
                                          "subject": social.IDENTITY_PREFIX + social.public_key(bob)})]
    records += [social.sign_signal(bob, {"kind": kind, "subject": subject, "issuedAt": 10})
                for kind in ("star", "watch", "recommendation")]
    records += [social.sign_signal(bytes([index]) * 32, {"kind": "recommendation", "subject": subject, "issuedAt": 10})
                for index in range(1, 21)]
    store = social.SignalStore()
    assert not store.import_bundle(_bundle(records), as_of)["rejected"]
    output = store.recommend({"viewer": social.public_key(alice), "enabled": True, "asOf": as_of, "limit": 5,
                              "mutedSigners": [], "mutedSubjects": []})
    assert [(entry["score"], entry["supporters"]) for entry in output["recommendations"]] == [(2, 1)]
    assert output["excluded"]["untrusted"] == 20


def test_cli_signs_revokes_merges_verifies_and_recommends(tmp_path):
    seed = tmp_path / "social.seed"
    run = lambda *args, **kw: subprocess.run(  # noqa: E731
        [sys.executable, str(TOOL), *args], capture_output=True, text=True, check=False, cwd=ROOT, timeout=120, **kw)
    created = run("keygen", "--out", str(seed))
    assert created.returncode == 0
    if os.name != "nt":
        assert seed.stat().st_mode & 0o077 == 0
    assert run("keygen", "--out", str(seed)).returncode != 0, "keygen must never overwrite a seed"
    unsigned = tmp_path / "star.json"
    unsigned.write_text(json.dumps({"kind": "star", "subject": social._repository("cli"), "issuedAt": 1000}))
    signed = json.loads(run("sign", "--seed-file", str(seed), str(unsigned)).stdout)
    revocation = json.loads(run("revoke", "--seed-file", str(seed), signed["signalId"], "--revoked-at", "2000").stdout)
    first, second = tmp_path / "a.json", tmp_path / "b.json"
    first.write_text(json.dumps(_bundle([signed])))
    second.write_text(json.dumps(_bundle([signed], [revocation])))
    merged = tmp_path / "merged.json"
    result = run("merge", str(first), str(second), "--as-of", "5000", "--audience", "public", "--output", str(merged))
    assert result.returncode == 0, result.stderr
    verified = run("verify", str(merged), "--as-of", "5000")
    assert verified.returncode == 0
    assert json.loads(verified.stdout)["state"]["signals"][0]["status"] == "revoked"
    tampered = json.loads(merged.read_text())
    tampered["signals"][0]["note"] = "edited"
    broken = tmp_path / "broken.json"
    broken.write_text(json.dumps(tampered))
    assert run("verify", str(broken), "--as-of", "5000").returncode == 1
    disabled = run("recommend", str(merged), "--viewer", created.stdout.strip(), "--disable", "--as-of", "5000")
    assert json.loads(disabled.stdout)["enabled"] is False


def test_documentation_covers_the_contract():
    text = DOC.read_text(encoding="utf-8")
    for requirement in range(1, 18):
        assert f"FM-SOC-{requirement:03d}" in text
    for phrase in ("tools/social_signals.py", "app/public/social-signals.js", "--disable",
                   "Failure and recovery", "signer_mismatch", "Not yet"):
        assert phrase in text
    assert "(social-signals.md)" in (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    gates = json.loads((ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text(encoding="utf-8"))
    gate = next(item for item in gates["gates"] if item["id"] == "social-signal-portability")
    assert gate["command"][1:] == ["tools/social_signals.py", "gate"]
    assert all("social-signal-portability" in gates["profiles"][name] for name in ("core", "release"))
