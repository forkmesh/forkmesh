"""Scoped community trust lists (issue #514): reference resolver and parity.

The shared vectors bind the Python, JavaScript, Dart, and desktop C++
resolvers; this module checks the reference, the Node resolver, the vector
generator, the compromised-list drill, and the product rules that must not
regress (no global scope, no essential path through a list, count-only
telemetry).
"""

from __future__ import annotations

import json
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / "conformance" / "trust-lists" / "v1"
VECTORS = json.loads((SUITE / "vectors.json").read_text(encoding="utf-8"))
CASES = {case["id"]: case for case in VECTORS["cases"]}
sys.path.insert(0, str(ROOT / "app" / "src"))
sys.path.insert(0, str(SUITE))

import generate_vectors as gen  # noqa: E402
import trust_lists as tl  # noqa: E402


def _resolve(data, *, documents=None, follows=None, overrides=None):
    return tl.resolve(
        data["documents"] if documents is None else documents,
        now=data["now"],
        follows=data["follows"] if follows is None else follows,
        overrides=data["overrides"] if overrides is None else overrides,
    )


@pytest.mark.parametrize("case_id", sorted(CASES))
def test_reference_matches_vectors_in_any_order(case_id):
    case = CASES[case_id]
    data = case["input"]
    assert _resolve(data) == case["expect"]
    rng = random.Random(case_id)
    for _ in range(3):
        documents = list(data["documents"])
        follows = list(data["follows"])
        overrides = list(data["overrides"])
        rng.shuffle(documents)
        rng.shuffle(follows)
        rng.shuffle(overrides)
        assert _resolve(data, documents=documents, follows=follows, overrides=overrides) == case["expect"]


def test_vectors_are_current():
    assert gen.serialize(gen.build()) == (SUITE / "vectors.json").read_text(encoding="utf-8")


def test_vectors_cover_required_behaviour():
    required = {
        "follow-single-list", "unfollowed-lists-grant-nothing", "follow-narrowing",
        "include-composes-within-intersection", "delegation-expiry",
        "conflict-visible-and-local-override", "override-ignore-and-local-only",
        "version-history-head", "history-fork-recovers", "equivocation-recovers",
        "equivocation-at-first-version-conflicted",
        "compromise-drill-before-revocation", "compromise-drill-revoked-and-recovered",
        "unauthorized-revocations-rejected", "issuer-retires-list", "expired-list",
        "missing-followed-list", "revoked-include-drops-out", "cycles-and-depth",
        "appeal-open", "appeal-resolved-forged-and-unattached", "rejections",
    }
    assert required <= set(CASES)
    states = {row["state"] for case in CASES.values() for row in case["expect"]["lists"]}
    assert states == set(tl.LIST_STATES)
    codes = {item["code"] for case in CASES.values() for item in case["expect"]["rejected"]}
    assert {
        "invalid_json", "noncanonical", "unsupported_schema", "invalid_keys",
        "invalid_signature", "entry_outside_scope", "unsorted_entries", "invalid_previous",
        "invalid_time", "invalid_edge", "edge_outside_scope", "invalid_revocation_keys",
        "invalid_scope", "issued_in_future", "unauthorized_revoker",
    } <= codes


def test_javascript_resolver_matches_every_vector():
    node = shutil.which("node")
    assert node, "Node.js is required for web trust-list parity"
    runner = ROOT / "app" / "tests" / "run_trust_lists_js.js"
    for case in VECTORS["cases"]:
        for reverse in (False, True):
            data = dict(case["input"])
            if reverse:
                data = {**data, **{k: list(reversed(data[k])) for k in ("documents", "follows", "overrides")}}
            completed = subprocess.run(
                [node, str(runner)], input=json.dumps(data), capture_output=True, text=True,
                timeout=60, check=False,
            )
            assert completed.returncode == 0, completed.stderr
            assert json.loads(completed.stdout) == case["expect"], (case["id"], reverse)


def test_compromise_drill_command_recovers_locally():
    completed = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "forkmesh_trust_lists.py"), "drill"],
        capture_output=True, text=True, timeout=120, check=False,
    )
    assert completed.returncode == 0, completed.stdout + completed.stderr
    report = json.loads(completed.stdout)
    assert report["before"] == {"state": "active", "headVersion": 3}
    assert report["after"]["state"] == "recovered"
    assert report["after"]["headVersion"] == 2
    assert report["after"]["successor"] is not None
    assert report["assertionsRemoved"] == 2


def test_revocation_is_monotonic_and_later_attacker_versions_stay_cut():
    case = CASES["compromise-drill-revoked-and-recovered"]
    data = case["input"]
    v3 = next(raw for raw in data["documents"] if json.loads(raw).get("version") == 3)
    v4 = gen.trust_list(
        "alice", "security", roles=[gen.SR], communities=["org:forkmesh"], version=4,
        previous=v3, issued_at=gen.ISSUED + 3 * gen.DAY,
        entries=[gen.entry("mallory", gen.SR, "org:forkmesh")],
    )
    retract = gen.revocation("alice", "security", from_version=9, revoked_by="alice", reason="superseded")
    result = _resolve(data, documents=data["documents"] + [v4, retract])
    row = next(r for r in result["lists"] if r["name"] == "security" and r["issuer"] == gen.KEYS["alice"])
    assert row["state"] == "recovered" and row["headVersion"] == 2 and row["cutoffVersion"] == 3
    assert gen.KEYS["mallory"] not in {a["subject"] for a in result["assertions"]}


def test_malicious_list_cannot_claim_global_or_out_of_scope_authority():
    for communities in (["*"], ["global"], ["repo:*"], ["org:"]):
        with pytest.raises(tl.TrustListError) as error:
            tl.verify_document(gen.trust_list("alice", "global", roles=[gen.SR], communities=communities))
        assert error.value.code == "invalid_scope"
    with pytest.raises(tl.TrustListError) as error:
        tl.verify_document(gen.trust_list(
            "alice", "narrow", roles=["reviewer"], communities=["repo:forkmesh/app"],
            entries=[gen.entry("bob", "reviewer", "org:forkmesh")],
        ))
    assert error.value.code == "entry_outside_scope"
    assert tl.intersect_scopes(["org:forkmesh"], ["repo:forkmesh/app"]) == ["repo:forkmesh/app"]
    assert tl.intersect_scopes(["org:forkmesh"], ["org:other"]) == []
    assert not tl.covers("repo:forkmesh/app", "org:forkmesh")
    assert not tl.covers("org:fork", "repo:forkmesh/app")


def test_single_character_mutations_never_verify():
    raw = CASES["follow-single-list"]["input"]["documents"][0]
    rng = random.Random(514)
    alphabet = "abcAB019_-:{}[]\",x "
    for _ in range(150):
        index = rng.randrange(len(raw))
        replacement = rng.choice(alphabet)
        if replacement == raw[index]:
            continue
        mutated = raw[:index] + replacement + raw[index + 1:]
        with pytest.raises(tl.TrustListError):
            tl.verify_document(mutated)


def test_telemetry_is_counts_only():
    secrets = set(VECTORS["testKeys"].values())
    for case in VECTORS["cases"]:
        telemetry = case["expect"]["telemetry"]
        text = json.dumps(telemetry)
        assert not any(secret in text for secret in secrets), case["id"]
        assert "urn:forkmesh:trust-list" not in text and "sha256:" not in text

        def numbers_only(value):
            if isinstance(value, dict):
                return all(numbers_only(v) for v in value.values())
            return isinstance(value, int) and not isinstance(value, bool)

        assert numbers_only(telemetry), case["id"]


def test_no_essential_path_depends_on_trust_lists():
    """Access, identity, and contribution code must never consult a list."""

    allowed = {"trust_lists.py"}
    for path in (ROOT / "app" / "src").glob("*.py"):
        if path.name in allowed:
            continue
        assert not re.search(r"\btrust_lists\b", path.read_text(encoding="utf-8")), path
    for pattern in ("app/public/**/*.js", "www/worker.js"):
        for path in ROOT.glob(pattern):
            if path.name == "trust-lists.js" or "node_modules" in path.parts:
                continue
            assert "ForkMeshTrustLists" not in path.read_text(encoding="utf-8", errors="ignore"), path
    empty = tl.resolve(CASES["follow-single-list"]["input"]["documents"], now=gen.NOW)
    assert empty["assertions"] == [] and empty["telemetry"]["followedLists"] == 0


def test_documented_clients_exist_and_docs_link_the_protocol():
    guide = (ROOT / "www" / "docs" / "trust-lists.md").read_text(encoding="utf-8")
    spec = (ROOT / "www" / "docs" / "design" / "scoped-trust-lists.md").read_text(encoding="utf-8")
    for relative in (
        "app/src/trust_lists.py", "app/public/trust-lists.js",
        "native/ios/ForkMeshCore/Sources/FMAccount/TrustLists.swift",
        "native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/TrustLists.kt",
        "desktop/src/TrustLists.cpp",
    ):
        assert relative in guide and relative in spec
        assert (ROOT / relative).exists(), relative
    assert "(trust-lists.md)" in (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "forkmesh-trust-list-tests" in (ROOT / "desktop" / "CMakeLists.txt").read_text(encoding="utf-8")
    for code in re.findall(r"`([a-z_]+)`", spec):
        if code.startswith(("invalid_", "unsorted_")) or code in {"too_large", "noncanonical", "list_mismatch"}:
            assert code in (ROOT / "app" / "src" / "trust_lists.py").read_text(encoding="utf-8"), code


def test_large_composed_resolution_stays_within_budget():
    subjects = [f"subject-{index}" for index in range(400)]
    keys = {name: gen.public_key(name) for name in subjects}
    gen.KEYS.update(keys)
    try:
        entries = [gen.entry(name, "reviewer", "org:forkmesh") for name in subjects]
        edges = [gen.edge("include", "bob", f"part-{i}", ["reviewer"], ["org:forkmesh"]) for i in range(8)]
        documents = [gen.trust_list("alice", "big", roles=["reviewer"], communities=["org:forkmesh"],
                                    entries=entries, edges=edges)]
        documents += [
            gen.trust_list("bob", f"part-{i}", roles=["reviewer"], communities=["org:forkmesh"],
                           entries=entries[i * 50:(i + 1) * 50])
            for i in range(8)
        ]
        started = time.perf_counter()
        result = tl.resolve(documents, now=gen.NOW, follows=[gen.follow("alice", "big")])
        elapsed = time.perf_counter() - started
    finally:
        for name in subjects:
            gen.KEYS.pop(name, None)
    assert result["telemetry"]["assertions"] == 400
    assert all(len(a["sources"]) == 2 for a in result["assertions"])
    assert elapsed < 5.0, elapsed
