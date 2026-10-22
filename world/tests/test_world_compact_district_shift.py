import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
NODE = shutil.which("node")

SCRIPT = r"""
globalThis.window = globalThis;
globalThis.document = { createElement: () => ({ getContext: () => null }) };
const THREE = await import(__THREE__);
const { functions } = await import("./scene/districts/compact-districts.js");
const { COMPACT_DISTRICT_RESIDENCY } = await import("./scene/quality.js");

const scene = new THREE.Scene();
const world = new THREE.Group();
scene.add(world);
const town = new THREE.Group();
world.add(town);
const beach = new THREE.Group();
const sand = new THREE.Mesh(new THREE.BoxGeometry(40, 2, 40), new THREE.MeshBasicMaterial());
beach.add(sand);
const centre = COMPACT_DISTRICT_RESIDENCY.beach;
beach.position.set(centre.x, 0, centre.z);
town.add(beach);
const camera = new THREE.PerspectiveCamera(60, 1, 0.1, 4000);
const ctx = {
  compactRenderer: true,
  compactDistricts: new Map([["beach", {
    ...centre, roots: new Set(), resident: null, inView: false,
    bounds: new THREE.Box3(), boundsDirty: true, distance: Infinity,
    evictions: 0, releasedGeometries: 0, releasedTextures: 0,
  }]]),
  compactTextureSlots: ["map"],
  officeFarSharedTextures: new Set(),
  player: { position: new THREE.Vector3(centre.x, 0, centre.z) },
  world,
  camera,
  officeSceneMode: "town",
  beachSceneActive: false,
};
world.userData = {};
const fns = {};
Object.assign(fns, functions({ THREE, reducedMotion: false, ctx, fns }));
scene.updateMatrixWorld(true);
fns.registerCompactDistrictRoot("beach", beach);
fns.updateCompactDistrictResidency(true);
const district = ctx.compactDistricts.get("beach");
const look = (x, z) => {
  camera.position.set(x, 30, z + 120);
  camera.lookAt(x, 0, z);
  camera.updateMatrixWorld(true);
  fns.updateCompactDistrictView();
};
const out = {};
out.before = { resident: district.resident, shown: beach.visible };
const shift = { x: 730, z: -420 };
town.position.set(shift.x, 0, shift.z);
scene.updateMatrixWorld(true);
out.moved = fns.moveCompactDistrictsWithin(town, shift.x, shift.z);
out.unrelated = fns.moveCompactDistrictsWithin(new THREE.Group(), 5, 5);
ctx.player.position.set(centre.x + shift.x, 0, centre.z + shift.z);
fns.updateCompactDistrictResidency();
look(centre.x + shift.x, centre.z + shift.z);
out.atShifted = {
  resident: district.resident,
  inView: district.inView,
  shown: beach.visible,
  centre: [district.x, district.z],
};
ctx.player.position.set(centre.x, 0, centre.z);
fns.updateCompactDistrictResidency();
camera.position.set(centre.x + shift.x, 30, centre.z + shift.z + 200);
camera.lookAt(centre.x + shift.x, 30, centre.z + shift.z + 400);
camera.updateMatrixWorld(true);
fns.updateCompactDistrictView();
out.leftBehind = { resident: district.resident, shown: beach.visible };
town.position.set(0, 0, 0);
scene.updateMatrixWorld(true);
fns.moveCompactDistrictsWithin(town, -shift.x, -shift.z);
out.home = { centre: [district.x, district.z], resident: district.resident, shown: beach.visible };
process.stdout.write(JSON.stringify(out));
"""


def test_districts_moved_next_door_stay_resident_and_drawn_where_they_now_stand():
    if NODE is None:
        pytest.skip("node is required")
    script = SCRIPT.replace("__THREE__", json.dumps(THREE.as_uri()))
    run = subprocess.run([NODE, "--input-type=module", "-e", script],
                         cwd=WORLD, capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    out = json.loads(run.stdout)
    assert out["before"] == {"resident": True, "shown": True}
    assert out["moved"] == 1
    assert out["unrelated"] == 0
    shifted = out["atShifted"]
    assert shifted["resident"] is True
    assert shifted["inView"] is True
    assert shifted["shown"] is True
    assert shifted["centre"] != out["home"]["centre"]
    assert out["leftBehind"] == {"resident": False, "shown": False}
    assert out["home"]["resident"] is True
    assert out["home"]["shown"] is True
