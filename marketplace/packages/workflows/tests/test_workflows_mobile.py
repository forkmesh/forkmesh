import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "marketplace" / "tools"))

import workflows_mobile_bundle as bundle  # noqa: E402

HARNESS = Path(__file__).with_name("engine_conformance.js")


def test_phone_bundles_match_the_server_engine():
    stale = bundle.stale(bundle.outputs())
    assert not stale, "run python3 marketplace/tools/workflows_mobile_bundle.py: " + ", ".join(
        str(path.relative_to(ROOT)) for path in stale)


def test_cases_cover_every_integration_operation_and_template():
    cases = json.loads((bundle.IOS_FIXTURES / bundle.CASES_NAME).read_text(encoding="utf-8"))
    names = {case["name"] for case in cases["runs"]}
    for key, app in bundle.integrations.INTEGRATIONS.items():
        for op in app["operations"]:
            assert f"op {key}.{op}" in names
    for template in bundle.templates.TEMPLATES:
        assert "template " + template["key"] in names
    assert len(cases["catalog"]["integrations"]) == len(bundle.integrations.INTEGRATIONS)


def test_the_manifest_ships_the_phone_engine():
    manifest = json.loads((bundle.PACKAGE / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["entrypoints"]["mobile-engine"] == "content/mobile/workflow-engine.js"
    sockets = {item["socket"]: item for item in manifest["contributions"]}
    assert sockets["mobile.engine"]["entrypoint"] == "mobile-engine"
    assert {"network.workflows.device", "device.workflows.credentials"} <= {p["id"] for p in manifest["permissions"]}


@pytest.mark.skipif(shutil.which("node") is None, reason="node is not installed")
def test_the_javascript_engine_runs_every_case_like_the_server():
    result = subprocess.run(
        ["node", str(HARNESS), str(bundle.ENGINE), str(bundle.IOS_RESOURCES / bundle.SPEC_NAME),
         str(bundle.IOS_FIXTURES / bundle.CASES_NAME)],
        capture_output=True, text=True, timeout=120, check=True)
    report = json.loads(result.stdout)
    assert report["checked"] > 300
    assert report["failures"] == [], json.dumps(report["failures"][:3])[:4000]
