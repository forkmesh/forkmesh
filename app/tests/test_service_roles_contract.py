#!/usr/bin/env python3
"""Service-role contract v1: manifest parity, independent start, failure verdicts."""

import copy
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys
import threading

import pytest


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "forkmesh_service_roles.py"
MANIFEST = ROOT / "conformance" / "service-roles" / "v1" / "manifest.json"
DOC = ROOT / "www" / "docs" / "operations" / "service-roles.md"
MIRROR_NODE_MAIN = ROOT / "server" / "cmd" / "forkmesh-mirror-node" / "main.go"
ISSUE_ROLES = (
    "web-ui", "relay", "public-index", "search", "identity-resolver",
    "collaboration-relay", "automation-broker", "organization-service",
)


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def roles():
    return _load("forkmesh_service_roles", TOOL)


@pytest.fixture(scope="module")
def manifest(roles):
    value = roles.load_manifest(MANIFEST)
    roles.validate_manifest(value)
    return value


def _role(manifest, role_id):
    return next(role for role in manifest["roles"] if role["id"] == role_id)


# ------------------------------------------------------------ manifest parity


def test_manifest_declares_every_role_the_issue_names(manifest):
    assert [role["id"] for role in manifest["roles"]] == list(ISSUE_ROLES)


def test_no_role_overclaims_independence(roles, manifest):
    summary = roles.gap_summary(manifest)
    for role in summary["roles"]:
        if role["status"] != "independent":
            assert role["gaps"], role["id"]
    hosted_only = {role["id"] for role in summary["roles"]
                   if role["status"] == "hosted-only"}
    assert hosted_only == {"search", "organization-service"}


def test_every_forkmesh_default_flag_is_declared_with_its_override(manifest):
    source = MIRROR_NODE_MAIN.read_text(encoding="utf-8")
    defaults = {
        match.group(1): match.group(2)
        for match in re.finditer(
            r'flag\.String\("([a-z-]+)", "(https://[^"]*forkmesh\.com[^"]*)"',
            source)
    }
    assert defaults, "mirror-node flags moved; update this parity test"
    declared = {
        endpoint["override"]["name"].lstrip("-"): endpoint["value"]
        for role in manifest["roles"]
        for endpoint in role["defaultEndpoints"]
    }
    assert declared == defaults


def test_documentation_lists_each_role_with_its_manifest_status(manifest):
    text = DOC.read_text(encoding="utf-8")
    for role in manifest["roles"]:
        assert "| `%s` | %s |" % (role["id"], role["status"]) in text
    for role in manifest["roles"]:
        for endpoint in role["defaultEndpoints"]:
            assert "`%s`" % endpoint["override"]["name"] in text
            assert "`%s`" % endpoint["value"] in text


def _mutated(manifest, change):
    value = copy.deepcopy(manifest)
    change(value)
    return value


@pytest.mark.parametrize(
    ("change", "message"),
    [
        (lambda m: m["roles"].append(dict(m["roles"][0], id="mystery")),
         "unknown role"),
        (lambda m: m["roles"].pop(), "organization-service: not declared"),
        (lambda m: m["roles"][0]["hosted"]["paths"].append("app/src/gone.py"),
         "app/src/gone.py does not exist"),
        (lambda m: m["roles"][0]["contract"][0]["source"].update(
            marker="def removed_route"), "no longer contains"),
        (lambda m: m["roles"][0].update(status="independent"),
         "cannot list gaps"),
        (lambda m: m["roles"][3].update(status="independent", gaps=[]),
         "cannot require a proprietary dependency"),
        (lambda m: m["roles"][0]["dependencies"]["optional"][0].pop(
            "substitute"), "needs a documented substitute"),
        (lambda m: m["roles"][1]["defaultEndpoints"][0].pop("override"),
         "needs an override"),
        (lambda m: m["roles"][0]["independentProfile"].update(
            runbook="www/docs/operations/mirror-node.md#no-such-heading"),
         "has no heading #no-such-heading"),
        (lambda m: m["roles"][2].update(gaps=[]), "must list its gaps"),
        (lambda m: m["roles"][0]["contract"][0].update(method="POST"),
         "read-only"),
        (lambda m: m["roles"][2].pop("independentProfile"),
         "needs an independentProfile"),
    ],
)
def test_validator_rejects_drift_and_overclaims(roles, manifest, change, message):
    errors = roles.manifest_errors(_mutated(manifest, change))
    assert any(message in error for error in errors), errors


# ------------------------------------------- independent start against a real service


@pytest.fixture
def independent_gateway(tmp_path):
    dashboard = _load("service_roles_dashboard_fixture",
                      ROOT / "app" / "tests" / "test_mirror_dashboard.py")
    gateway = dashboard.gateway
    bare = dashboard._make_assets_repository(tmp_path)
    config_path = dashboard._write_config(tmp_path, bare)
    assert "forkmesh.com" not in config_path.read_text(encoding="utf-8")
    application = gateway.GatewayApplication(
        gateway.load_config(config_path),
        verifier=dashboard._Verifier(),
        health_signer=dashboard._Signer(),
        clock_ms=lambda: dashboard.NOW,
        log=lambda _entry: None,
    )
    server = gateway.MirrorGatewayServer(("127.0.0.1", 0), application)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = "http://127.0.0.1:%d" % server.server_address[1]

    def stop():
        server.shutdown()
        server.server_close()
        thread.join(timeout=10)

    state = {"base_url": base_url, "stop": stop, "stopped": False}
    yield state
    if not state["stopped"]:
        stop()
    application.close()


@pytest.mark.parametrize("role_id", ["web-ui", "relay", "public-index"])
def test_gateway_profile_starts_and_passes_role_contract(
        roles, manifest, independent_gateway, role_id):
    report = roles.probe_role(
        manifest, role_id, independent_gateway["base_url"],
        params={"owner": "forkmesh", "repository": "forkmesh"}, timeout=10)
    assert report["verdict"] == "pass", report["checks"]
    assert report["defaultPublicService"] is False
    assert report["schema"] == "forkmesh-service-role-report-v1"
    assert len(report["checks"]) == len(_role(manifest, role_id)["contract"])
    assert report["telemetry"]["requests"] == len(report["checks"])
    assert report["telemetry"]["transportErrors"] == 0


def test_service_loss_is_reported_unreachable_not_empty(
        roles, manifest, independent_gateway):
    independent_gateway["stop"]()
    independent_gateway["stopped"] = True
    report = roles.probe_role(manifest, "public-index",
                              independent_gateway["base_url"], timeout=2)
    assert report["verdict"] == "unreachable"
    assert report["telemetry"]["transportErrors"] == 1
    assert roles.VERDICT_EXIT[report["verdict"]] == roles.EXIT_UNREACHABLE


# ----------------------------------------------------------- failure verdicts


def _fetcher(responses):
    def fetch(url, method, timeout):
        for suffix, response in responses.items():
            if url.split("?", 1)[0].endswith(suffix):
                status, content_type, body = response
                return status, content_type, json.dumps(body).encode() \
                    if not isinstance(body, bytes) else body
        raise OSError("connection refused")
    return fetch


def test_explicit_503_is_degraded_and_anything_else_fails(roles, manifest):
    degraded = roles.probe_role(
        manifest, "search", "https://index.example.org",
        params={"query": "mesh"},
        fetcher=_fetcher({"/api/repositories/search": (
            503, "application/json", {"error": "catalog_unavailable"})}))
    assert degraded["verdict"] == "degraded"
    assert degraded["checks"][0]["error"] == "catalog_unavailable"

    broken = roles.probe_role(
        manifest, "public-index", "https://index.example.org",
        fetcher=_fetcher({"/api/repositories": (
            500, "text/html", b"<h1>oops</h1>")}))
    assert broken["verdict"] == "fail"
    assert broken["checks"][0]["problems"]

    # An empty 200 without the contract fields is a failure, not a pass.
    empty = roles.probe_role(
        manifest, "public-index", "https://index.example.org",
        fetcher=_fetcher({"/api/repositories": (200, "application/json", {})}))
    assert empty["verdict"] == "fail"


def test_partial_outage_is_fail_not_unreachable(roles, manifest):
    report = roles.probe_role(
        manifest, "web-ui", "https://mirror.example.org",
        fetcher=_fetcher({"/health": (
            200, "application/json", {"ok": True, "integrity": "ok"})}))
    assert report["verdict"] == "fail"
    assert [check["result"] for check in report["checks"]][:2] == [
        "pass", "unreachable"]


def test_report_never_prints_credentials_or_queries(roles, manifest):
    report = roles.probe_role(
        manifest, "public-index",
        "https://user:secret@app.forkmesh.com/?token=abc",
        fetcher=_fetcher({"/api/repositories": (
            200, "application/json", {"ok": True, "repositories": []})}))
    serialized = json.dumps(report)
    assert "secret" not in serialized and "token" not in serialized
    assert report["baseUrl"] == "https://app.forkmesh.com/"
    assert report["defaultPublicService"] is True


def test_missing_path_parameter_is_a_usage_error(roles, manifest):
    with pytest.raises(roles.ManifestError, match="missing --param owner"):
        roles.probe_role(manifest, "relay", "https://mirror.example.org",
                         fetcher=_fetcher({}))


# ------------------------------------------------------------------------ CLI


def test_cli_validate_and_gaps_run_from_repository_root():
    validate = subprocess.run(
        [sys.executable, str(TOOL), "validate"], cwd=ROOT,
        capture_output=True, text=True, check=False)
    assert validate.returncode == 0, validate.stderr
    assert "8 roles" in validate.stdout

    gaps = subprocess.run(
        [sys.executable, str(TOOL), "gaps", "--json"], cwd=ROOT,
        capture_output=True, text=True, check=False)
    assert gaps.returncode == 0, gaps.stderr
    summary = json.loads(gaps.stdout)
    assert sum(summary["counts"].values()) == 8


def test_cli_probe_exits_unreachable_for_a_closed_port():
    result = subprocess.run(
        [sys.executable, str(TOOL), "probe", "--role", "public-index",
         "--base-url", "http://127.0.0.1:9", "--timeout", "2"],
        cwd=ROOT, capture_output=True, text=True, check=False)
    assert result.returncode == 4, result.stderr
    assert json.loads(result.stdout)["verdict"] == "unreachable"
