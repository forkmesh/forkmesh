#!/usr/bin/env python3
"""External dependency and observability map contracts (issue #503).

The manifest in ``conformance/external-dependencies/v1`` must classify every
outside host, provider credential, and stored provider identifier the
production source contains; the published page and the privacy notice are
checked against that same manifest.
"""

import copy
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "external_dependency_inventory.py"
MANIFEST = ROOT / "conformance" / "external-dependencies" / "v1" / "manifest.json"
PRIVACY = ROOT / "www" / "public" / "privacy.html"
BROWSER_COMPONENTS = {"www-browser", "app-browser", "world-browser"}


def _tool():
    spec = importlib.util.spec_from_file_location("external_dependency_inventory", TOOL)
    module = importlib.util.module_from_spec(spec)
    # dataclasses resolves annotations through sys.modules at class creation.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _manifest():
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def _fake_repository(tmp_path, source, secrets="DATA_KEY", schema=""):
    (tmp_path / "app" / "src").mkdir(parents=True)
    (tmp_path / "app" / "deploy.sh").write_text(
        f'push_secrets() {{\n    local allowed=" {secrets} "\n}}\n', encoding="utf-8")
    (tmp_path / "app" / "src" / "schema.py").write_text(
        f"SCHEMA_STATEMENTS = ({schema!r},)\n", encoding="utf-8")
    (tmp_path / "app" / "src" / "feature.py").write_text(source, encoding="utf-8")
    return tmp_path


def _minimal_manifest(**overrides):
    manifest = {
        "schema": "forkmesh-external-dependency-map-v1",
        "documentation": "missing.md",
        "inventoryScope": {"include": ["app/src/*.py"], "exclude": []},
        "firstParty": {"suffixes": ["forkmesh.com"], "secrets": ["DATA_KEY"]},
        "storedIdentifierTablePattern": "discord",
        "dependencies": [],
        "references": [],
        "storedIdentifiers": [],
    }
    manifest.update(overrides)
    return manifest


def test_inventory_of_production_source_matches_the_manifest():
    result = subprocess.run(
        [sys.executable, str(TOOL), "check"],
        cwd=ROOT, check=False, capture_output=True, text=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr


def test_manifest_classifies_every_dependency_with_the_required_facts():
    tool = _tool()
    manifest = _manifest()
    tool.validate_manifest(manifest)
    assert manifest["issue"] == 503
    categories = {c for dep in manifest["dependencies"] for c in dep["categories"]}
    # The issue's scope names these classes of outside infrastructure.
    assert {
        "dns", "certificates", "cdn-edge", "forge-api", "identity", "email",
        "payments", "package-registry", "telemetry", "hosted-runner",
    } <= categories


def test_mandatory_hosted_dependencies_name_a_self_host_path_and_a_drill():
    for dep in _manifest()["dependencies"]:
        if dep["necessity"] != "mandatory":
            continue
        assert dep["drills"], dep["id"]
        assert re.search(r"self-host|mirror node|own", dep["alternative"], re.I), dep["id"]


def test_an_unlisted_outside_host_fails_the_inventory(tmp_path):
    tool = _tool()
    root = _fake_repository(
        tmp_path,
        'await fetch("https://api.new-tracker.net/v1/collect")\n'
        'HOME = "https://app.forkmesh.com/"\n'
        'FIXTURE = "https://mirror.example.test/"\n',
    )
    report = tool.build_report(_minimal_manifest(), root)
    assert not report["ok"]
    assert list(report["problems"]["unclassifiedHosts"]) == ["api.new-tracker.net"]
    assert report["measures"]["undocumentedDependencies"] == 1


def test_a_link_only_reference_loaded_as_a_subresource_fails(tmp_path):
    tool = _tool()
    root = _fake_repository(
        tmp_path,
        'SHARE = "<a href=\\"https://x.com/intent/post\\">Share</a>"\n'
        'PIXEL = "<img src=\\"https://x.com/pixel.gif\\">"\n',
    )
    manifest = _minimal_manifest(references=[
        {"host": "x.com", "kind": "link", "why": "share intent"}])
    report = tool.build_report(manifest, root)
    assert report["problems"]["unclassifiedHosts"] == {}
    assert list(report["problems"]["referenceHostsFetchedAsSubresources"]) == ["x.com"]


def test_unclaimed_credentials_and_provider_tables_fail(tmp_path):
    tool = _tool()
    root = _fake_repository(
        tmp_path,
        'token = _env_text(env, "NEW_PROVIDER_API_TOKEN")\n',
        secrets="DATA_KEY NEW_WEBHOOK_SECRET",
        schema="CREATE TABLE IF NOT EXISTS discord_links (discord_bi TEXT)",
    )
    report = tool.build_report(_minimal_manifest(), root)
    assert report["problems"]["unclaimedCredentials"] == [
        "NEW_PROVIDER_API_TOKEN", "NEW_WEBHOOK_SECRET"]
    assert report["problems"]["unclaimedIdentifierTables"] == ["discord_links"]


def test_stale_drills_and_hosts_that_left_the_source_fail(tmp_path):
    tool = _tool()
    root = _fake_repository(tmp_path, "NOTHING = 1\n")
    dependency = {
        "id": "gone", "name": "Gone", "categories": ["email"],
        "components": ["app-worker"], "necessity": "optional",
        "hosts": ["api.gone.net"], "observes": "x", "retention": "x",
        "jurisdiction": "x", "canonicalData": "x", "outage": "x",
        "recovery": "x", "alternative": "x", "evidence": ["app/src/feature.py"],
        "drills": ["app/tests/test_gone.py::test_removed"],
    }
    report = tool.build_report(_minimal_manifest(dependencies=[dependency]), root)
    assert report["problems"]["manifestHostsNotFoundInSource"] == ["gone:api.gone.net"]
    assert report["problems"]["missingDrillTests"] == [
        "gone:app/tests/test_gone.py::test_removed"]


def test_a_dependency_without_a_drill_must_record_its_gap():
    tool = _tool()
    manifest = _manifest()
    broken = copy.deepcopy(manifest)
    target = next(dep for dep in broken["dependencies"] if dep.get("drills"))
    target["drills"] = []
    target.pop("gap", None)
    try:
        tool.validate_manifest(broken)
    except tool.InventoryError as exc:
        assert "failure drill" in str(exc)
    else:
        raise AssertionError("a dependency without a drill or gap validated")


def test_published_page_block_is_rendered_from_the_manifest():
    tool = _tool()
    manifest = _manifest()
    page = (ROOT / manifest["documentation"]).read_text(encoding="utf-8")
    assert tool.extract_block(page) == tool.render_block(manifest)
    for dep in manifest["dependencies"]:
        assert f'<a id="{dep["id"]}"></a>' in page


def _privacy_text():
    html = PRIVACY.read_text(encoding="utf-8")
    return " ".join(re.sub(r"<[^>]+>", " ", html).split())


def test_privacy_notice_is_checked_against_the_same_manifest():
    privacy = _privacy_text()
    manifest = _manifest()
    findings = {item["dependency"] for item in manifest["privacyFindings"]}
    for dep in manifest["dependencies"]:
        disclosure = dep.get("privacyDisclosure")
        observes_visitors = bool(BROWSER_COMPONENTS & set(dep["components"])) or (
            "telemetry" in dep["categories"]) or dep.get("personalData", False)
        if disclosure:
            assert disclosure in privacy, (dep["id"], disclosure)
        elif observes_visitors and dep["necessity"] != "build":
            # A service that sees visitors but is not named in the notice has
            # to be an open, published privacy finding instead.
            assert dep["id"] in findings, dep["id"]
    for item in manifest["privacyFindings"]:
        assert item["contradicts"] in privacy, item["dependency"]
        page = (ROOT / manifest["documentation"]).read_text(encoding="utf-8")
        assert item["dependency"] in page


def test_self_hosting_documentation_links_the_map():
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    self_hosting = readme.split("## Self-hosting and operations", 1)[1].split("\n## ", 1)[0]
    assert "www/docs/external-dependency-map.md" in self_hosting
    docs_index = (ROOT / "www" / "docs" / "README.md").read_text(encoding="utf-8")
    assert "(external-dependency-map.md)" in docs_index


def test_quality_gate_and_workflow_run_the_map():
    gates = json.loads(
        (ROOT / "www" / "docs" / "engineering" / "quality-gates.json").read_text(encoding="utf-8"))
    gate = next(g for g in gates["gates"] if g["id"] == "external-dependency-map")
    assert "app/tests/test_external_dependency_map.py" in gate["command"]
    assert "external-dependency-map" in gates["profiles"]["core"]
    workflow = (ROOT / ".github" / "workflows" / "external-dependency-map.yml").read_text(
        encoding="utf-8")
    for os_name in ("ubuntu-latest", "macos-latest", "windows-latest"):
        assert os_name in workflow
    assert "tools/external_dependency_inventory.py report" in workflow
