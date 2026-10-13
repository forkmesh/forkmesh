#!/usr/bin/env python3
"""Contracts for the restored Office parcel and remote district packages."""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2]
TOWER = ROOT / "world" / "public" / "world" / "world-office-tower.js"
SCENE = ROOT / "world" / "public" / "world" / "world-scene.js"
DATA = ROOT / "world" / "public" / "world" / "world-data.js"
APP = ROOT / "world" / "public" / "world" / "world.js"
BACKEND = ROOT / "world" / "src" / "world.py"


def source(path):
    return path.read_text(encoding="utf-8")


def tower_contract():
    script = f"""
      import * as office from {json.dumps(TOWER.as_uri())};
      process.stdout.write(JSON.stringify({{
        center: office.OFFICE_ISLAND_CENTER,
        radius: office.OFFICE_ISLAND_RADIUS,
        bridgeStart: office.OFFICE_BRIDGE_START_Z,
        bridgeEnd: office.OFFICE_BRIDGE_END_Z,
        bridgeWidth: office.OFFICE_BRIDGE_WIDTH,
        budget: office.OFFICE_DISTANT_DRAW_BUDGET,
      }}));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_existing_office_returns_to_its_original_walkable_parcel():
    contract = tower_contract()
    assert contract == {
        "center": [0, 0, -207],
        "radius": 57,
        "bridgeStart": -86,
        "bridgeEnd": -108,
        "bridgeWidth": 12,
        "budget": 64,
    }
    assert 'position: [0, 0, -207]' in source(DATA)


def test_existing_office_retires_its_hyperloop_and_restores_the_bridge():
    scene = world_scene_source()
    for contract in (
        'officeBridge.name = "forkmesh-office-bridge"',
        '"forkmesh-town-path-north-office"',
        "world.add(officeBridge)",
        '"causeways", "Causeways & bridges", "Terrain", officeBridge',
    ):
        assert contract in scene
    assert "createOfficeTransport" not in scene
    assert "officeTransport" not in scene
    assert "forkmesh-office-hyperloop" not in scene
    assert not (TOWER.parent / "world-office-transport.js").exists()


def test_main_office_and_its_separate_interior_share_one_distance_lod():
    scene = world_scene_source()
    for contract in (
        'high.name = "forkmesh-office-high-fidelity"',
        'low.name = "forkmesh-office-low-poly"',
        'lod.name = "forkmesh-office-distance-lod"',
        "lod.addLevel(highLevel, 0);",
        "lod.addLevel(low, OFFICE_EXTERIOR_LOD_DISTANCE);",
        "group.userData.officeDistantDrawBudget = OFFICE_DISTANT_DRAW_BUDGET;",
        "office.userData.officeDistantDrawCount = proxy.userData.drawCount;",
        "officeExteriorHigh.add(officeInterior)",
        "officeInterior.visible = true;",
        'registerCompactDistrictRoot("office", officeHighFidelity)',
    ):
        assert contract in scene
    assert 'landmarkObjects.get("office"),\n    officeLandscaping' not in scene


def test_only_remote_district_views_replace_mainland_with_bounded_proxy():
    scene = world_scene_source()
    for contract in (
        "const MAINLAND_DISTANT_TRIANGLE_BUDGET = 120;",
        'lod.name = "forkmesh-mainland-distance-lod"',
        "lod.autoUpdate = false",
        'high.name = "forkmesh-mainland-high-fidelity"',
        'low.name = "forkmesh-mainland-low-poly"',
        "roundedHexagonGeometry(\n      THREE,\n      CONTINUOUS_CITY_RADIUS,\n      CONTINUOUS_CITY_CORNER_RADIUS,\n      1,\n    )",
        "lod.userData.mainlandDistantTriangleCount = triangleWeight(low)",
        'const remoteHorizon = next === "remote-district";',
        'const panoramic = next === "office";',
        "enclosureExteriorIsCostly(root, roots[0])",
        'mainlandLow.visible = remoteHorizon;',
        'world.userData.mainlandDetailLevel = remoteHorizon ? "low" : "detailed";',
    ):
        assert contract in scene


def test_presence_keeps_legacy_office_spaces_and_district_package_envelope():
    app = source(APP)
    backend = source(BACKEND)
    for space in (
        "office-campus",
        "office-lobby",
        "office-marketing",
        "office-engineering",
        "office-infrastructure",
        "office-rooftop",
    ):
        assert f'"{space}"' in backend
    assert "const POSITION_RADIUS = 16_000;" in app
    assert '"office-campus": 0.38' in app
    assert "WORLD_COORD_LIMIT = 16_000.0" in backend
    # District package spaces share one namespaced shape on every tier, so a
    # package can add floors without a relay or shell release.
    assert 'WORLD_PACKAGE_SPACE_RE = re.compile(r"pkg-[a-z0-9]+(?:-[a-z0-9]+){1,8}")' in backend
    assert "const WORLD_PACKAGE_SPACE_RE = /^pkg-[a-z0-9]+(?:-[a-z0-9]+){1,8}$/;" in app
    assert "function knownWorldSpace(space)" in app
    # Only the helper itself consults the core roster; every caller asks it.
    assert app.count("WORLD_SPACE_IDS.has(") == 1
    assert "export function packageSpace(value)" in world_scene_source()
    assert "forkmesh-sky-office" not in world_scene_source()
    assert "forkmesh-sky-office" not in app
    assert "forkmesh-sky-office" not in backend
