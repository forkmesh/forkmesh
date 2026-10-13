#!/usr/bin/env python3
"""Contracts for file-leaf activation in the repository size map."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DASHBOARD_SOURCE = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
DASHBOARD_URL_HELPERS = "\n".join(
    (
        (ROOT / "public" / "dashboard" / "js" / "02-helpers.js").read_text(
            encoding="utf-8"
        ),
        (
            ROOT / "public" / "dashboard" / "js" / "05-repo-list-explorer.js"
        ).read_text(encoding="utf-8"),
    )
)
DASHBOARD_BUNDLE = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
MIRROR_GATEWAY = (ROOT.parent / "tools" / "mirror_gateway.py").read_text(
    encoding="utf-8"
)


def _assert_leaf_contract(source):
    assert 'child.type === "file"' in source
    assert 'filePath: child.type === "file" ? String(child.path || "")' in source
    assert 'role="button" aria-label="${escapeHtml(' in source
    assert 'tabindex="0"' in source
    assert '["Enter", " "]' in source
    assert 'chart.addEventListener("click"' in source
    assert "activateSizeMapTarget(event.target)" in source
    assert "loadRepositoryBlob(repo, seg.filePath)" in source
    assert "repoLiveUrl(repo, \"blob\", { path })" in source
    assert "Pointer, touch, Enter, and Space are supported" in source


def test_authored_and_built_dashboard_open_leaf_at_selected_ref():
    _assert_leaf_contract(DASHBOARD_SOURCE)
    _assert_leaf_contract(DASHBOARD_BUNDLE)
    for source in (DASHBOARD_URL_HELPERS, DASHBOARD_BUNDLE):
        assert 'query.set("ref", repoSelectedBranch(repo))' in source
        assert 'query.set(key, String(value ?? ""))' in source
        assert 'path.split("/").map(encodeURIComponent).join("/")' in source


def test_direct_https_gateway_emits_bounded_file_leaf_paths():
    assert '"path": path' in MIRROR_GATEWAY
    assert '"type": "file"' in MIRROR_GATEWAY
    assert '"type": "directory"' in MIRROR_GATEWAY
    assert "children[:max_children]" in MIRROR_GATEWAY
    assert "max_children = 40" in MIRROR_GATEWAY
    assert "max_depth = 8" in MIRROR_GATEWAY
