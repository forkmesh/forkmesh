"""The deploy stage serves a minified dashboard bundle; app/public keeps the
readable build (tools/build_dashboard_assets.py minify_staged)."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
for path in (ROOT / "src", ROOT / "tools"):
    if str(path) not in sys.path:
        sys.path.insert(0, str(path))

import build_dashboard_assets  # noqa: E402
import dashboard_bundle  # noqa: E402
import dashboard_shell  # noqa: E402

TOOLS = (ROOT / "tools" / "build_site_assets.py").read_text(encoding="utf-8")


def _esbuild():
    configured = os.environ.get("FORKMESH_ESBUILD", "")
    binary = Path(configured) if configured else (
        ROOT.parent / "world" / "tools" / "node_modules" / ".bin" / "esbuild")
    if not binary.exists():
        pytest.skip("esbuild is not installed (npm ci in world/tools)")
    return binary


@pytest.fixture(scope="module")
def stage(tmp_path_factory):
    esbuild = _esbuild()
    stage = tmp_path_factory.mktemp("stage") / "dist"
    stage.mkdir()
    names = ["dashboard.js"] + [
        dashboard_bundle.chunk_asset(chunk) for chunk in dashboard_bundle.LAZY_CHUNKS]
    for name in names:
        shutil.copy2(PUBLIC / name, stage / name)
    (stage / "dashboard-plugin-host.js").write_text("// not a bundle file\n")
    minified = build_dashboard_assets.minify_staged(stage, esbuild)
    return stage, names, minified


def test_only_the_bundle_and_its_chunks_are_minified(stage):
    stage, names, minified = stage
    assert sorted(minified) == sorted(names)
    assert (stage / "dashboard-plugin-host.js").read_text() == "// not a bundle file\n"
    # app/public keeps the readable build the tests and developers read.
    assert "\n  const state = {\n" in (PUBLIC / "dashboard.js").read_text(encoding="utf-8")


def test_minified_bundle_is_smaller_and_parses(stage):
    stage, names, _ = stage
    source = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
    served = (stage / "dashboard.js").read_text(encoding="utf-8")
    assert len(served) < 0.65 * len(source)
    assert "\n  const state = {\n" not in served
    if shutil.which("node"):
        for name in names:
            subprocess.run(["node", "--check", str(stage / name)], check=True)


def test_window_and_chunk_contracts_survive_renaming(stage):
    stage, _, _ = stage
    served = (stage / "dashboard.js").read_text(encoding="utf-8")
    # Names other scripts and the chunks reach stay as written: window
    # globals, the chunk registry, and the property names of the scope a
    # chunk factory receives.
    for kept in ("__forkmeshDashboardChunks", "ForkMeshComposer",
                 "__forkmeshInstanceInfo", "__forkmeshEarly",
                 "forkmesh:composer-host"):
        assert kept in served, kept
    plans = dashboard_bundle.plan_chunks({
        name: (PUBLIC / dashboard_bundle.fragment_path(name)).read_text(encoding="utf-8")
        for name in dashboard_bundle.FRAGMENTS})
    for chunk, (imports, _, exports) in plans.items():
        text = (stage / dashboard_bundle.chunk_asset(chunk)).read_text(encoding="utf-8")
        assert "window.__forkmeshDashboardChunks" in text and chunk in text
        for name in exports:
            assert name in text, (chunk, name)
        for name in imports:
            assert name in served, (chunk, name)
    # The deferred dialog markup is string data and keeps its hooks.
    for spec in dashboard_bundle.DEFERRED_MARKUP.values():
        assert spec["marker"].lstrip("#") in served
    for hook in ("data-feedback-form", "data-agent-login-surface",
                 "data-repo-agent-new-form", "data-profile-about-textarea"):
        assert hook in served, hook


def test_source_map_points_at_the_readable_build(stage):
    stage, names, _ = stage
    for name in names:
        served = (stage / name).read_text(encoding="utf-8")
        assert served.rstrip().endswith("//# sourceMappingURL=%s.map" % name)
        source_map = json.loads((stage / (name + ".map")).read_text(encoding="utf-8"))
        assert source_map["sources"] == ["app/public/" + name]
        assert source_map["sourcesContent"] == [
            (PUBLIC / name).read_text(encoding="utf-8")]


def test_version_query_hashes_the_served_bytes(stage):
    stage, _, _ = stage
    versions = dashboard_shell.staged_asset_versions(stage)
    served = (stage / "dashboard.js").read_text(encoding="utf-8")
    assert versions["dashboard.js"] == dashboard_shell.content_version(served)
    assert versions["dashboard.js"] != dashboard_shell.content_version(
        (PUBLIC / "dashboard.js").read_text(encoding="utf-8"))


def _function(name):
    start = TOOLS.index("def %s(" % name)
    return TOOLS[start:TOOLS.index("\ndef ", start + 1)]


def test_app_and_cutover_stages_minify_before_stamping_versions():
    for stager in ("stage_app", "stage_cutover"):
        body = _function(stager)
        minify = body.index("minify_dashboard(destination)")
        assert minify < body.index("finish_stage(destination"), stager


def test_a_stage_without_esbuild_keeps_the_readable_bundle(tmp_path, monkeypatch):
    import build_site_assets

    def missing():
        raise RuntimeError("World build needs npm to install esbuild (world/tools)")

    monkeypatch.setattr(build_site_assets, "esbuild_binary", missing)
    (tmp_path / "dashboard.js").write_text("const readable = 1;\n")
    assert build_site_assets.minify_dashboard(tmp_path) == []
    assert (tmp_path / "dashboard.js").read_text() == "const readable = 1;\n"
