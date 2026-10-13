#!/usr/bin/env python3
"""Focused repository issue/pull workbench contracts.

The open repository district deliberately has no in-world PR or issue towers. Record
work stays in the signed web workbench, which keeps the scene legible while
retaining the same commit-pinned record access.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
WORLD = ROOT.parent / "world" / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()

DESK = SCENE[
    SCENE.index("function updateRepositoryRecordDesk("):
    SCENE.index("function updateRepositoryGraph(")
]


def _function_body(source, function):
    start = source.index(f"  {function}(")
    depth = 0
    opened = False
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
            opened = True
        elif source[index] == "}" and opened:
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"missing closing brace for {function}")


def test_scene_clears_any_legacy_record_desk_without_creating_new_towers():
    # The shared scene refresh still has a safe teardown entry point, but it
    # exits before selecting records, allocating cards, or mounting a layer.
    return_at = DESK.index("return;")
    assert "removeGeneratedLayer(previousMount, previousLayer, interactive)" in DESK
    assert "world.userData.repositoryRecordDeskLayer = null" in DESK
    assert "world.userData.repositoryRecordDeskData = null" in DESK
    assert "const owner" not in DESK[:return_at]
    assert 'layer.name = "repository-record-desk"' not in DESK[:return_at]
    assert "new THREE.InstancedMesh(" not in DESK[:return_at]


def test_repository_scene_refresh_does_not_submit_records_to_the_3d_scene():
    refresh = _function_body(APP, "syncRepositoryScene")
    assert "updateRepositoryRecordDesk" not in refresh
    assert "repositoryPullRecords(active)" not in refresh
    assert "expandedIssue: this.expandedRepositoryIssuePage" not in refresh


def test_commit_pinned_issue_and_pull_workbenches_remain_available():
    loader = APP[
        APP.index("  async loadRepositoryEntityRecords("):
        APP.index("  flagshipCatalogCommits(")
    ]
    assert "selectedIssues" in loader
    assert "this.loadRepositoryBlobBatches(" in loader
    assert "issue metadata batch commit mismatch" in loader
    assert "record.metadataAvailable = true" in loader

    issue_workbench = _function_body(APP, "openRepositoryIssueWorkbench")
    assert 'this.openRepositoryRecordWebWorkbench("issue", page)' in issue_workbench
    workbench = _function_body(APP, "openRepositoryRecordWebWorkbench")
    assert '"pulls" : "issues"' in workbench
    assert "data-world-repository-web-workbench" in workbench
    assert "<iframe" in workbench


def test_pull_records_still_route_to_the_canonical_web_workbench():
    pull_workbench = _function_body(APP, "openRepositoryPullWorkbench")
    assert 'this.openRepositoryRecordWebWorkbench("pull"' in pull_workbench
    generic = _function_body(APP, "openRepositoryRecordWebWorkbench")
    assert 'detail.dataset.openLandmark = `repository-${recordKind}`' in generic
    assert '"pulls" : "issues"' in generic
    assert "Repository portals" not in generic
