import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = world_scene_source()
CACHE = (WORLD / "world-resource-cache.js").read_text(encoding="utf-8")

NODE = shutil.which("node")

SCRIPT = r"""
import * as THREE from "./vendor/three-0.184.0/three.module.min.js";
import {
  INSTANCED_SOURCE_LAYER,
  collectRepeatedMeshes,
  commonAncestor,
  createInstancedItems,
  createInstancedMirrorSet,
  isSharedResource,
  resolveInstancedHits,
  sharedGeometry,
  sharedMaterial,
} from "./world-resource-cache.js";
import { auditWorldScene } from "./world-profiler.js";

const out = {};
const disposals = (resource) => {
  let count = 0;
  resource.addEventListener("dispose", () => { count += 1; });
  return () => count;
};

const box = sharedGeometry(THREE, "BoxGeometry", 0.282, 0.18, 0.282);
const boxCounter = disposals(box);
box.dispose();
out.geometry = {
  same: box === sharedGeometry(THREE, "BoxGeometry", 0.282, 0.18, 0.2820000000001),
  different: box !== sharedGeometry(THREE, "BoxGeometry", 0.282, 0.416, 0.282),
  shared: isSharedResource(box),
  disposeEvents: boxCounter(),
  usableAfterDispose: box.attributes.position.count,
  objectParameterUncached:
    sharedGeometry(THREE, "ShapeGeometry", new THREE.Shape()) !==
    sharedGeometry(THREE, "ShapeGeometry", new THREE.Shape()),
};

const texture = new THREE.Texture();
const matte = sharedMaterial(THREE, "MeshStandardMaterial", { color: "#FFFFFF", roughness: 0.5 });
const matteCounter = disposals(matte);
matte.dispose();
out.material = {
  same: matte === sharedMaterial(THREE, "MeshStandardMaterial", { roughness: 0.5, color: 0xffffff }),
  colorMatters: matte !== sharedMaterial(THREE, "MeshStandardMaterial", { color: "#fffffe", roughness: 0.5 }),
  typeMatters: matte !== sharedMaterial(THREE, "MeshLambertMaterial", { color: "#ffffff" }),
  textureKeyed:
    sharedMaterial(THREE, "MeshBasicMaterial", { map: texture }) ===
      sharedMaterial(THREE, "MeshBasicMaterial", { map: texture }) &&
    sharedMaterial(THREE, "MeshBasicMaterial", { map: texture }) !==
      sharedMaterial(THREE, "MeshBasicMaterial", { map: new THREE.Texture() }),
  disposeEvents: matteCounter(),
};

const scene = new THREE.Scene();
const bars = createInstancedItems(THREE, {
  geometry: sharedGeometry(THREE, "BoxGeometry", 1, 1, 1),
  material: sharedMaterial(THREE, "MeshStandardMaterial", { color: "#9ef7c6" }),
  items: ["users", "repositories", "world_events"].map((name, index) => ({
    name: `system-capacity-table:${name}`,
    position: new THREE.Vector3(index * 2, 0.5 + index, 0),
    scale: new THREE.Vector3(0.5, 1 + index * 2, 0.5),
    userData: { tableName: name, rowCount: index * 10, interactive: "system-capacity-table" },
  })),
  name: "bars",
});
scene.add(bars);
scene.updateMatrixWorld(true);
const raycaster = new THREE.Raycaster(new THREE.Vector3(4, 10, 0), new THREE.Vector3(0, -1, 0));
const rawHits = raycaster.intersectObjects([bars], false);
const hits = resolveInstancedHits(rawHits);
const worldPoint = hits[0].object.getWorldPosition(new THREE.Vector3());
out.items = {
  draws: 1,
  count: bars.count,
  rawInstanceId: rawHits[0].instanceId,
  table: hits[0].object.userData.tableName,
  interactive: hits[0].object.userData.interactive,
  name: hits[0].object.name,
  instancedObject: hits[0].instancedObject === bars,
  parentIsMesh: hits[0].object.parent === bars,
  worldPoint: [worldPoint.x, worldPoint.y, worldPoint.z],
  missKeepsPlainHits: resolveInstancedHits([{ object: scene }])[0].object === scene,
  cloneable: (() => { try { bars.clone(); return true; } catch { return false; } })(),
};

const mirrorScene = new THREE.Scene();
const layer = new THREE.Group();
layer.position.set(10, 0, 0);
mirrorScene.add(layer);
const disk = new THREE.CircleGeometry(1, 16);
const live = sharedMaterial(THREE, "MeshStandardMaterial", { color: "#77d9ff" });
const stub = sharedMaterial(THREE, "MeshStandardMaterial", { color: "#748a82" });
const portals = [0, 1, 2, 3].map((index) => {
  const node = new THREE.Group();
  node.position.set(index * 3, 2, 0);
  const mesh = new THREE.Mesh(disk, index === 3 ? stub : live);
  mesh.userData.repositoryPortal = { name: `repo${index}` };
  node.add(mesh);
  layer.add(node);
  return { node, mesh };
});
const mirror = createInstancedMirrorSet(THREE, layer, { name: "portals" });
portals.forEach(({ mesh }) => mirror.add(mesh));
mirrorScene.updateMatrixWorld(true);
const firstSync = mirror.sync();
const secondSync = mirror.sync();
const batches = layer.children.filter((child) => child.isInstancedMesh);
const liveBatch = batches.find((batch) => batch.material === live);
const instancePosition = (batch, index) => {
  const matrix = new THREE.Matrix4();
  batch.getMatrixAt(index, matrix);
  return new THREE.Vector3().setFromMatrixPosition(matrix).toArray();
};
const firstInstance = instancePosition(liveBatch, 0);
const camera = new THREE.PerspectiveCamera();
const drawnByCamera = portals.every(({ mesh }) => !mesh.layers.test(camera.layers));
const pick = new THREE.Raycaster(new THREE.Vector3(13, 2, 5), new THREE.Vector3(0, 0, -1));
const missesWithoutLayer = pick.intersectObjects(portals.map(({ mesh }) => mesh), false).length;
pick.layers.enable(INSTANCED_SOURCE_LAYER);
const pickHits = pick.intersectObjects([...portals.map(({ mesh }) => mesh), ...batches], false);
portals[1].node.visible = false;
portals[2].node.position.x += 1;
mirrorScene.updateMatrixWorld(true);
mirror.sync();
const afterMove = { count: liveBatch.count, second: instancePosition(liveBatch, 1) };
portals[0].mesh.material = stub;
mirror.sync();
const swapped = { live: liveBatch.count, stub: batches.find((batch) => batch.material === stub).count };
portals[0].mesh.material = live;
const sibling = new THREE.Group();
mirrorScene.add(sibling);
const outsider = new THREE.Mesh(disk, live);
sibling.add(outsider);
const hostRotors = new THREE.Group();
mirrorScene.add(hostRotors);
const rotorMirror = createInstancedMirrorSet(THREE, hostRotors, {});
rotorMirror.add(outsider);
mirrorScene.updateMatrixWorld(true);
rotorMirror.sync();
const rotorBatch = hostRotors.children.find((child) => child.isInstancedMesh);
const outsiderCounted = rotorBatch.count;
sibling.remove(outsider);
rotorMirror.sync();
const countWhenDetached = rotorBatch.count;
const crowd = createInstancedMirrorSet(THREE, mirrorScene, { cull: false });
const pins = [0, 1, 2].map((index) => {
  const pin = new THREE.Mesh(disk, live);
  pin.position.set(index, 0, 0);
  mirrorScene.add(pin);
  return pin;
});
mirrorScene.updateMatrixWorld(true);
crowd.sync(pins, { checked: true });
const crowdStats = crowd.stats();
const staticMirror = createInstancedMirrorSet(THREE, layer, { interval: 5 });
const lamp = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), live);
layer.add(lamp);
staticMirror.add(lamp);
mirrorScene.updateMatrixWorld(true);
staticMirror.sync();
lamp.visible = false;
const skipped = [1, 2, 3].map(() => staticMirror.sync());
staticMirror.invalidate();
staticMirror.sync();
const staticBatch = layer.children.filter((child) => child.isInstancedMesh).pop();
const repeated = new THREE.Group();
const crate = sharedGeometry(THREE, "BoxGeometry", 2, 2, 2);
const floorA = new THREE.Group();
const floorB = new THREE.Group();
repeated.add(floorA, floorB);
for (let index = 0; index < 5; index += 1) (index % 2 ? floorA : floorB).add(new THREE.Mesh(crate, live));
const glass = sharedMaterial(THREE, "MeshStandardMaterial", { transparent: true, opacity: 0.5 });
for (let index = 0; index < 5; index += 1) repeated.add(new THREE.Mesh(crate, glass));
const groups = collectRepeatedMeshes(THREE, repeated);
mirror.dispose();
out.mirror = {
  batches: batches.length,
  firstSync,
  secondSync,
  firstInstance,
  drawnByCamera,
  missesWithoutLayer,
  batchRaycasts: pickHits.every((hit) => !hit.object.isInstancedMesh),
  pickName: pickHits[0]?.object.userData.repositoryPortal?.name,
  liveBatchCount: afterMove.count,
  movedInstance: afterMove.second,
  swapped,
  outsiderCounted,
  countWhenDetached,
  crowd: crowdStats,
  skipped,
  staticHiddenAfterInvalidate: staticBatch.count,
  repeatedGroups: groups.map((group) => group.length),
  ancestor: commonAncestor(groups[0], null) === repeated,
  restored: portals.every(({ mesh }) => mesh.layers.test(camera.layers)),
  batchesRemoved: !layer.children.some((child) => child.isInstancedMesh && child.name.startsWith("portals")),
};

const hiddenMaterial = new THREE.MeshBasicMaterial({ visible: false });
const auditScene = new THREE.Scene();
for (let index = 0; index < 5; index += 1) auditScene.add(new THREE.Mesh(disk, hiddenMaterial));
auditScene.add(new THREE.Mesh(disk, live));
auditScene.updateMatrixWorld(true);
const audit = auditWorldScene({ scene: auditScene, renderer: { info: { memory: {}, programs: [] } } });
out.audit = { drawables: audit.stats.drawables };

console.log(JSON.stringify(out));
"""


@pytest.fixture(scope="module")
def result():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_geometry_cache_returns_one_instance_that_owners_cannot_dispose(result):
    geometry = result["geometry"]
    assert geometry["same"] is True
    assert geometry["different"] is True
    assert geometry["shared"] is True
    assert geometry["disposeEvents"] == 0
    assert geometry["usableAfterDispose"] == 24
    assert geometry["objectParameterUncached"] is True


def test_material_cache_is_keyed_by_normalized_parameters(result):
    material = result["material"]
    assert material["same"] is True
    assert material["colorMatters"] is True
    assert material["typeMatters"] is True
    assert material["textureKeyed"] is True
    assert material["disposeEvents"] == 0


def test_instanced_items_map_instance_id_back_to_the_item(result):
    items = result["items"]
    assert items["count"] == 3
    assert items["rawInstanceId"] == 2
    assert items["table"] == "world_events"
    assert items["interactive"] == "system-capacity-table"
    assert items["name"] == "system-capacity-table:world_events"
    assert items["instancedObject"] is True
    assert items["parentIsMesh"] is True
    assert items["worldPoint"] == [4, 2.5, 0]
    assert items["missKeepsPlainHits"] is True
    assert items["cloneable"] is True


def test_instanced_mirror_draws_sources_once_and_keeps_them_pickable(result):
    mirror = result["mirror"]
    assert mirror["batches"] == 2
    assert mirror["firstSync"] == 2
    assert mirror["secondSync"] == 0
    assert mirror["firstInstance"] == [0, 2, 0]
    assert mirror["drawnByCamera"] is True
    assert mirror["missesWithoutLayer"] == 0
    assert mirror["batchRaycasts"] is True
    assert mirror["pickName"] == "repo1"
    assert mirror["liveBatchCount"] == 2
    assert mirror["movedInstance"] == [7, 2, 0]
    assert mirror["swapped"] == {"live": 1, "stub": 2}
    assert mirror["outsiderCounted"] == 1
    assert mirror["countWhenDetached"] == 0
    assert {key: mirror["crowd"][key] for key in ("draws", "instances", "sources")} == {
        "draws": 1,
        "instances": 3,
        "sources": 0,
    }
    assert mirror["skipped"] == [0, 0, 0]
    assert mirror["staticHiddenAfterInvalidate"] == 0
    assert mirror["repeatedGroups"] == [5]
    assert mirror["ancestor"] is True
    assert mirror["restored"] is True
    assert mirror["batchesRemoved"] is True

def test_audit_skips_meshes_that_are_never_drawn(result):
    assert result["audit"]["drawables"] == 1


def test_scene_uses_the_caches_for_the_profiled_offenders():
    assert 'sharedGeometry(THREE, "BoxGeometry", 0.2, 1.2, 0.2)' in SCENE
    assert 'sharedGeometry(THREE, "IcosahedronGeometry", 1, 0)' in SCENE
    assert "markSharedResource(shared);" in SCENE
    assert "createGeometrySharing" not in SCENE
    assert SCENE.count("export function indexGeometry(") == 1
    assert "indexGeometry" not in CACHE
    assert "raycaster.layers.enable(INSTANCED_SOURCE_LAYER);" in SCENE
    assert "groundProbe.layers.enable(INSTANCED_SOURCE_LAYER);" in SCENE
    assert "if (object.userData?.instancedMirror === true) return false;" in SCENE


def test_avatar_details_and_repeated_static_meshes_are_instanced():
    assert "details.sync(detailSources, { checked: true });" in SCENE
    assert "if (pin && branchVisible(pin, avatar)) detailSources.push(pin);" in SCENE
    assert "instanceRepeatedStaticMeshes();" in SCENE
    assert "interval: STATIC_MIRROR_SYNC_FRAMES," in SCENE
    assert SCENE.count("invalidateStaticMirrors();") >= 3
    for element in (
        '"avatar-crowd-batches"',
        '"member-circle-figures"',
        '"office-participants"',
    ):
        assert element in SCENE


def test_every_interactive_raycast_resolves_instanced_hits():
    raw = re.findall(r"raycaster\.intersectObjects\(interactive, false\)", SCENE)
    wrapped = re.findall(
        r"resolveInstancedHits\(\s*raycaster\.intersectObjects\(interactive, false\),?\s*\)",
        SCENE,
    )
    assert len(raw) == 3
    assert len(wrapped) == 3


def test_portal_ring_and_quadcopter_rotors_are_mirrored_into_instanced_draws():
    assert "repositoryPortalMirror = createInstancedMirrorSet(THREE, layer, {" in SCENE
    assert "repositoryPortalMirror.dispose();" in SCENE
    assert "rotors.forEach((rotor) => quadcopterRotorMirror.add(rotor));" in SCENE
    assert "for (const mirror of instancedMirrorSets) mirror.sync();" in SCENE
    assert "mesh.raycast = () => {};" in CACHE


def test_small_props_use_modest_segment_counts():
    assert "TorusGeometry(radius, 0.045, 6, 256)" not in SCENE
    assert "new THREE.TorusGeometry(radius, 0.045, 4, 128)" in SCENE
    assert 'sharedGeometry(THREE, "TorusGeometry", 4.15, 0.38, 12, 48)' in SCENE


def test_animated_or_tinted_materials_stay_unique():
    assert "coalMaterial.customProgramCacheKey = () => \"campfire-glowing-coals\";" in SCENE
    assert "const skin = new THREE.MeshBasicMaterial({" in SCENE
    assert 'makeSharedMaterial(THREE, "#7d766c"' in SCENE
