#!/usr/bin/env python3
"""Open repository district and focused-workbench presentation contracts."""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
APP = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(encoding="utf-8")


def test_repository_circle_keeps_its_charts_in_an_open_district():
    district = SCENE[
        SCENE.index("function createRepositoryDistrict("):
        SCENE.index("function createOrganizationQuarter(")
    ]
    assert 'content.name = "repository-district-content"' in district
    assert "group.add(content);" in district
    assert "content.add(\n    createDistrictGroundCircle(" in district
    assert "content.add(portal);" in district
    assert "content.add(importKiosk);" in district
    assert "group.userData.repositoryContent = content;" in district
    assert "createRepositoryGeodesicDome" not in SCENE
    assert "repository-geodesic-dome" not in SCENE
    assert "REPOSITORY_DOME_" not in SCENE


def test_repository_catalog_has_no_enclosure_residency_or_collision_gate():
    catalog = SCENE[
        SCENE.index("  function updateRepositoryCatalog("):
        SCENE.index("  function setRepositoryImportState(")
    ]
    assert 'layer.name = "repository-perimeter-portals"' in catalog
    assert "world.add(layer);" in catalog
    assert 'registerCompactDistrictRoot("repositories", layer);' in catalog
    assert "repositoryDomeInterior" not in catalog
    assert "updateRepositoryDomeOccupancy" not in SCENE
    assert "constrainRepositoryDome" not in SCENE
    assert "setSlidingEnclosureDoorOpen" not in SCENE


def test_repository_refresh_no_longer_feeds_the_tall_pr_and_issue_lists():
    refresh = APP[
        APP.index("  syncRepositoryScene() {"):
        APP.index("  repositoryTreeSizePreview(")
    ]
    desk = SCENE[
        SCENE.index("function updateRepositoryRecordDesk("):
        SCENE.index("function setRepositoryIssuePageExpanded(")
    ]
    assert "updateRepositoryRecordDesk" not in refresh
    assert desk.index("return;") < desk.index("const owner")
    assert "repository-record-desk" not in desk[:desk.index("return;")]


def test_repo_exhibit_has_an_angled_named_pedestal_without_an_agent_control_dock():
    assert "repository-commit-activity-pedestal" in SCENE
    assert "backing.rotation.x = -Math.PI / 4;" in SCENE
    assert "repositoryCommitActivityTexture(" in SCENE
    assert "`${owner}/${name}`" in SCENE
    assert "repository-agent-control-dock:" not in SCENE
    assert "function createRepositoryAgentTerminal(" not in SCENE
    assert "repository-agent-robot-body:" not in SCENE
    assert "repository-agent-robot-wheel:" not in SCENE
    assert "repository-agent-robot-arm:" not in SCENE
    assert "repository-agent-terminal-screen:" not in SCENE
    assert "repositoryAgentTasksByRepository" in SCENE
    assert 'task.status === "running"' in SCENE


def test_agent_and_fediverse_counts_share_the_repository_placard():
    texture = SCENE[
        SCENE.index("function repositoryCommitActivityTexture("):
        SCENE.index("const REPOSITORY_RECORD_STATE_COLORS")
    ]
    assert 'label: "AGENT CONTROL"' in texture
    assert "runningAgents.toLocaleString" in texture
    assert 'label: "FEDIVERSE"' in texture
    assert '"FOLLOWERS"' in texture
    activity = SCENE[
        SCENE.index("function updateRepositoryActivity("):
        SCENE.index("function updateRepositorySizeMap(")
    ]
    assert "repositoryAgentTasksByRepository.get(repositoryKey)" in activity
    assert "portalRecord?.record?.fediverseFollowerCount" in activity
    assert "{ runningAgents, fediverseFollowers }" in activity
    assert "repository-agent-control-label:" not in SCENE
    assert "repository-fediverse-follower-caption:" not in SCENE


def test_cabinet_faces_are_swapped_and_agent_sides_are_provider_specific():
    cabinet = SCENE[
        SCENE.index("function createMirrorServerCabinet"):
        SCENE.index("\n}\n", SCENE.index("function createMirrorServerCabinet"))
    ]
    agents = SCENE[
        SCENE.index("function mirrorAgentTaskPanelTexture"):
        SCENE.index("function createServedVisitorFigure")
    ]
    assert 'panel.position.set(0, 1.72, -0.735)' in cabinet
    assert 'rearPanel.position.set(0, 1.72, 0.735)' in cabinet
    assert 'const provider = side < 0 ? "claude-code" : "codex";' in cabinet
    assert "task.provider === providerLabel" in agents


def test_top_right_identity_control_uses_account_avatar_not_country_flag():
    assert "data-world-shirt-avatar" in APP
    assert "data-world-shirt-initial" not in APP
    assert "data-world-shirt-flag" not in APP
    badge = CSS[
        CSS.index(".world-shirt-badge {"):
        CSS.index(".world-shirt-badge:hover")
    ]
    assert "width: 40px" in badge
    assert "height: 40px" in badge
    assert ".world-shirt-avatar" in CSS
    assert "object-fit: cover" in CSS
