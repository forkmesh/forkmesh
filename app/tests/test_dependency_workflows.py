#!/usr/bin/env python3
"""Stable repository identity, verified dependency routes, and stock-client
recipes (issue #495, www/docs/dependency-workflows.md)."""

import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app" / "src"))

import collaboration  # noqa: E402
import repository_index  # noqa: E402
import stable_repository as stable  # noqa: E402
import urls  # noqa: E402


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


TOOL = _load("forkmesh_dependency_conformance", ROOT / "tools" / "dependency_conformance.py")
INDEX_TESTS = _load("forkmesh_repository_index_tests", Path(__file__).with_name("test_repository_index.py"))
ROOT_COMMIT = "3fa1c0de" * 5
IDENTITY = stable.repository_identity(ROOT_COMMIT)
SIGNED = "a" * 64
UNSIGNED = "b" * 64
COMMIT = "c0ffee00" * 5


# --- Identity ---------------------------------------------------------------


def test_identity_is_fm_id_001_over_the_root_commit():
    assert IDENTITY == collaboration.repository_id("git-root-commit:" + ROOT_COMMIT)
    assert stable.valid_identity(IDENTITY)
    assert stable.repository_identity(ROOT_COMMIT.upper()) == IDENTITY
    assert stable.valid_identity(stable.repository_identity("ab" * 32))


def test_identity_ignores_every_routing_hint():
    source = {"owner": "alice", "name": "widget", "rootCommit": ROOT_COMMIT,
              "cloneUrl": "", "sshUrl": "ssh://git@forkmesh.com/alice/widget.git"}
    moved = {"owner": "acme-org", "name": "widget-renamed", "rootCommit": ROOT_COMMIT,
             "cloneUrl": "https://mirror.example/alice/widget",
             "sshUrl": "ssh://git@other.example:2222/acme-org/widget-renamed.git"}
    assert stable.record_identity(source) == stable.record_identity(moved) == IDENTITY
    assert stable.record_identity(dict(source, rootCommit="4" * 40)) != IDENTITY


@pytest.mark.parametrize("root", ["", None, "abc123", "root-acme-widget", "g" * 40, "a" * 41, "a" * 63])
def test_identity_fails_closed_without_a_full_root_commit(root):
    assert stable.repository_identity(root) == ""
    assert stable.record_identity({"owner": "acme", "name": "widget", "rootCommit": root}) == ""


def test_public_index_results_carry_the_identity_shared_by_mirrors():
    source = INDEX_TESTS.repo("alice", "widget", rootCommit=ROOT_COMMIT)
    mirror = INDEX_TESTS.repo("bob", "widget", rootCommit=ROOT_COMMIT, source="remote-clone",
                              cloneUrl="https://forkmesh.com/alice/widget")
    rootless = INDEX_TESTS.repo("carol", "tool")
    result = INDEX_TESTS.run([source, mirror, rootless], limit=50)
    by_name = {item["fullName"]: item for item in result["results"]}
    assert by_name["alice/widget"]["identity"] == IDENTITY
    assert by_name["carol/tool"]["identity"] == ""
    assert "bob/widget" not in by_name


# --- Route selection and pins ---------------------------------------------------


def _route(label, **overrides):
    route = {"route": label, "reachable": True, "identity": IDENTITY, "stateHash": SIGNED,
             "hasCommit": True, "refs": {"refs/tags/v1.0.0": COMMIT}}
    route.update(overrides)
    return route


def test_first_declared_route_with_a_signed_state_is_selected():
    report = stable.select_route(IDENTITY, [_route("origin"), _route("mirror")], {SIGNED})
    assert report["status"] == "resolved"
    assert report["selected"] == "origin"
    assert report["fallback"] is False
    assert report["attempts"] == [{"route": "origin", "outcome": "selected", "reason": None}]


def test_failover_is_declared_order_and_lists_every_skipped_route():
    routes = [_route("origin", reachable=False), _route("tampered", stateHash=UNSIGNED),
              _route("mirror-a"), _route("mirror-b")]
    report = stable.select_route(IDENTITY, routes, {SIGNED})
    assert report["status"] == "failed_over"
    assert report["selected"] == "mirror-a"
    assert report["fallback"] is True
    assert [(item["route"], item["reason"]) for item in report["attempts"]] == [
        ("origin", "unreachable"), ("tampered", "state_unverified"), ("mirror-a", None)]
    assert stable.select_route(IDENTITY, copy.deepcopy(routes), {SIGNED}) == report
    swapped = routes[:2] + [routes[3], routes[2]]
    assert stable.select_route(IDENTITY, swapped, {SIGNED})["selected"] == "mirror-b"


def test_unverified_or_foreign_routes_are_never_substituted():
    routes = [_route("origin", reachable=False), _route("tampered", stateHash=UNSIGNED),
              _route("fork", identity=stable.repository_identity("5" * 40)),
              _route("unhashed", stateHash="")]
    report = stable.select_route(IDENTITY, routes, {SIGNED, "", "not-a-digest", None})
    assert report["status"] == "unavailable"
    assert report["selected"] is None
    assert report["errors"] == ["no_verified_route"]
    assert [item["reason"] for item in report["attempts"]] == [
        "unreachable", "state_unverified", "identity_mismatch", "state_unverified"]
    assert stable.select_route(IDENTITY, [], {SIGNED})["status"] == "unavailable"
    assert stable.select_route(IDENTITY, [_route("origin", reachable="yes")], {SIGNED})["selected"] is None


@pytest.mark.parametrize("commit", ["main", "refs/heads/main", "v1.0.0", COMMIT[:12], "", None])
def test_branch_tag_and_abbreviated_pins_are_rejected_before_any_route(commit):
    report = stable.resolve({"identity": IDENTITY, "commit": commit}, [_route("origin")], {SIGNED})
    assert report["status"] == "rejected"
    assert report["errors"] == ["mutable_pin"]
    assert report["attempts"] == []


def test_malformed_identity_and_ref_are_rejected():
    report = stable.resolve({"identity": "forkmesh.com/alice/widget", "commit": COMMIT, "ref": "HEAD"},
                            [_route("origin")], {SIGNED})
    assert report["errors"] == ["identity_invalid", "ref_invalid"]
    assert stable.select_route("alice/widget", [_route("origin")], {SIGNED})["errors"] == ["identity_invalid"]
    assert stable.resolve("not a pin", [], {SIGNED})["status"] == "rejected"


def test_pins_report_missing_commits_and_moved_refs():
    pin = {"identity": IDENTITY, "commit": COMMIT, "ref": "refs/tags/v1.0.0"}
    missing = stable.resolve(pin, [_route("origin", hasCommit=False)], {SIGNED})
    assert (missing["status"], missing["attempts"][0]["reason"]) == ("unavailable", "commit_missing")
    moved = stable.resolve(pin, [_route("retagged", refs={"refs/tags/v1.0.0": "d" * 40})], {SIGNED})
    assert (moved["status"], moved["attempts"][0]["reason"]) == ("unavailable", "ref_moved")
    recovered = stable.resolve(pin, [_route("retagged", refs={"refs/tags/v1.0.0": "d" * 40}),
                                     _route("mirror")], {SIGNED})
    assert (recovered["status"], recovered["selected"]) == ("failed_over", "mirror")
    unpinned_ref = stable.resolve({"identity": IDENTITY, "commit": COMMIT},
                                  [_route("origin", refs={})], {SIGNED})
    assert unpinned_ref["status"] == "resolved"


# --- Routes -------------------------------------------------------------------------


@pytest.mark.parametrize("pattern, suffix", [
    (urls.GIT_INFO_RE, "info/refs"),
    (urls.GIT_PACK_RE, "git-upload-pack"),
    (urls.GIT_RECEIVE_RE, "git-receive-pack"),
])
def test_git_routes_accept_the_dot_git_spelling_as_the_same_repository(pattern, suffix):
    for path, expected in (
        ("/acme/widget/" + suffix, ("acme", "widget")),
        ("/acme/widget.git/" + suffix, ("acme", "widget")),
        ("/acme/my.repo.git/" + suffix, ("acme", "my.repo")),
        ("/acme/my.repo/" + suffix, ("acme", "my.repo")),
    ):
        match = pattern.match(path)
        assert match and match.groups() == expected, path
    assert pattern.match("/acme/.git/" + suffix).group(2) == ".git"
    assert pattern.match("/acme/widget.git/extra/" + suffix) is None


# --- Contract, documentation, CI ---------------------------------------------------------


def test_contract_agrees_with_reference_documentation_and_workflow():
    assert TOOL.validate_contract(TOOL.load_contract()) == []
    assert TOOL.main(["validate"]) == 0


def test_contract_validation_catches_drift():
    contract = TOOL.load_contract()
    for mutate in (
        lambda value: value["scenarios"].pop(),
        lambda value: value["routeReasons"].reverse(),
        lambda value: value["clients"][0].update(id="svn"),
        lambda value: value.update(documentation="www/docs/missing.md"),
        lambda value: value["scenarios"][0].update(expect="maybe"),
    ):
        broken = copy.deepcopy(contract)
        mutate(broken)
        assert TOOL.validate_contract(broken)


def test_quality_gate_and_docs_index_include_the_suite():
    config = json.loads((ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text(encoding="utf-8"))
    gate = next(item for item in config["gates"] if item["id"] == "dependency-workflow-conformance")
    assert "app/tests/test_dependency_workflows.py" in gate["command"]
    assert "dependency-workflow-conformance" in config["profiles"]["core"]
    assert "(dependency-workflows.md)" in (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "`identity`" in (ROOT / "www" / "docs" / "repository-index.md").read_text(encoding="utf-8")


def test_probe_rejects_unknown_clients():
    with pytest.raises(TOOL.ConformanceError, match="unknown clients"):
        TOOL.run_probe(clients=["svn"])


# --- Stock clients ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def report():
    executables = TOOL._client_executables()
    if not executables["git"]:
        pytest.skip("git is not installed")
    return TOOL.run_probe(client_label="pytest")


def test_every_available_client_meets_every_scenario(report):
    available = [item["id"] for item in report["clients"] if item["available"]]
    assert {"git", "git-submodule", "pip"} <= set(available)
    assert [row for row in report["results"] if not row["ok"]] == []
    for client in available:
        scenarios = {row["scenario"] for row in report["results"] if row["client"] == client}
        expected = set(TOOL.SCENARIOS) - (set() if client == "go" else {"moved-tag"})
        assert scenarios == expected, client
    summary = report["summary"]
    assert summary["unexpectedResults"] == 0
    assert summary["lockChecks"] == summary["reproducibleLocks"] == 3 * len(available)
    # Every relay request whose origin was lost either failed over or was refused.
    assert summary["failoverAttempts"] == summary["failoverSuccess"] + (
        summary["resolutionAttempts"] - summary["resolutionSuccess"])
    assert summary["failoverSuccess"] > 0
    assert summary["verificationFailures"] > 0


def test_probe_resolutions_show_verified_failover_and_refusal(report):
    rows = {}
    for row in report["resolutions"]:
        rows.setdefault(row["scenario"], []).append(row)
    assert {row["status"] for row in rows["origin-lost"]} == {"failed_over"}
    assert rows["origin-lost"][0]["attempts"] == [
        {"route": "origin", "outcome": "skipped", "reason": "unreachable"},
        {"route": "mirror", "outcome": "selected", "reason": None}]
    assert {row["status"] for row in rows["unverified-fallback"]} == {"unavailable"}
    assert rows["unverified-fallback"][0]["attempts"][1]["reason"] == "state_unverified"
    assert {row["status"] for row in rows["rollback-recovery"]} == {"resolved"}
    assert rows["missing-commit"][0]["attempts"][0]["reason"] == "commit_missing"
    assert rows["moved-tag"][0]["attempts"][0]["reason"] == "ref_moved"
    refused = [row for row in report["results"] if row["scenario"] == "unverified-fallback"]
    assert refused and all(row["outcome"] == "refused" and row["lockDigest"] is None for row in refused)


def test_report_is_private_and_self_describing(report):
    text = json.dumps(report)
    for private in ("127.0.0.1", "[::1]", "forkmesh-deps-", tempfile.gettempdir(), 'VALUE = "pinned"'):
        assert private not in text
    assert report["schema"] == "forkmesh-dependency-conformance-report-v1"
    assert report["identity"].startswith(stable.IDENTITY_PREFIX)
    assert set(TOOL.load_contract()["telemetry"]["report"]) <= set(report)
    assert set(TOOL.load_contract()["telemetry"]["summary"]) == set(report["summary"])
