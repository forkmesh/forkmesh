"""Truthful, accessible construction-state markers in ForkMesh World."""

from pathlib import Path
from world_scene_source import world_scene_source


PUBLIC = Path(__file__).resolve().parents[2] / "world" / "public" / "world"
APP = (PUBLIC / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (PUBLIC / "world.css").read_text(encoding="utf-8")


def test_unknown_remote_integrations_fail_closed_but_local_features_start_live():
    assert "const LOCAL_LIVE_LANDMARKS = new Set([" in APP
    for landmark in ("campfire", "neighborhood", "broadcast"):
        assert f'  "{landmark}",' in APP
    for removed in ("support", "workshops"):
        assert f'  "{removed}",' not in APP
    assert "live: LOCAL_LIVE_LANDMARKS.has(landmark.id)" in APP
    assert "This integration has not been verified in this session." in APP


def test_live_state_requires_loaded_schema_or_usable_capability_evidence():
    assert "hasRepositoryCatalogSchema(reposResult.value)" in APP
    assert "hasFediverseDirectorySchema(directoryResult.value)" in APP
    assert "Array.isArray(orgResult.value?.organizations)" in APP
    assert "Array.isArray(eventsResult.value?.events)" in APP
    assert "hasCompletedSecurityScan(this.securityScan)" in APP
    # Mirror cabinets are live infrastructure, not a synthetic landmark whose
    # state can be inferred from one remote response.
    assert "liveNodeRecordsWithActions(" in APP
    assert "this.mirrorActionRunsByNode" in APP
    assert "this.world?.updateNetworkNodes(" in APP
    assert "LANDMARK_CONSTRUCTION_REASONS.workshops" not in APP
    assert "/^[1-9A-HJ-NP-Za-km-z]{32,44}$/" in APP
    assert "statusTone" not in APP[
        APP.index("function initialLandmarkCapabilities"):
        APP.index("function accountBadgeCopy")
    ]


def test_markers_are_accessible_on_map_and_panels():
    assert 'role="img"' in APP
    assert 'aria-label="Under construction:' in APP
    assert 'data-world-construction-marker="${escapeHTML(id)}"' in APP
    assert "world-construction-mark-map" in APP
    assert "world-construction-mark-panel" in APP
    assert ".world-construction-mark[hidden]" in CSS
    assert ".world-map-label-copy" in CSS
    assert ".world-status-row" in CSS


def test_markers_never_disable_landmark_navigation():
    construction = APP[
        APP.index("function constructionMarkerHTML"):
        APP.index("function accountBadgeCopy")
    ]
    assert "disabled" not in construction
    assert 'data-world-landmark="${escapeHTML(landmark.id)}"' in APP
