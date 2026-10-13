import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_module


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
NODE = shutil.which("node")

MERGE_SCRIPT = r"""
import * as THREE from "./vendor/three-0.184.0/three.core.min.js";
import { mergeMeshesByMaterial } from "./scene/batching.js";

const root = new THREE.Group();
root.position.set(10, 0, 0);
const red = new THREE.MeshStandardMaterial({ color: "#ff0000" });
const blue = new THREE.MeshStandardMaterial({ color: "#0000ff" });
const box = new THREE.BoxGeometry(1, 1, 1);
const a = new THREE.Mesh(box, red);
a.position.set(2, 0, 0);
const b = new THREE.Mesh(box, red);
b.scale.set(-1, 1, 1);
const c = new THREE.Mesh(box, blue);
const named = new THREE.Group();
named.add(new THREE.Mesh(box, blue));
root.add(a, b, c, named);
const merged = mergeMeshesByMaterial(THREE, root);
const meshes = [];
root.traverse((object) => {
  if (object.isMesh) meshes.push(object);
});
const redMesh = meshes.find((mesh) => mesh.material === red);
const box3 = new THREE.Box3().setFromBufferAttribute(redMesh.geometry.attributes.position);
console.log(JSON.stringify({
  merged,
  meshes: meshes.length,
  materials: meshes.map((mesh) => mesh.material.color.getHexString()).sort(),
  redVertices: redMesh.geometry.attributes.position.count,
  redMin: box3.min.toArray(),
  redMax: box3.max.toArray(),
  redParent: redMesh.parent === root,
  counts: meshes.map((mesh) => mesh.userData.mergedMeshCount || 0).sort(),
}));
"""


@pytest.fixture(scope="module")
def merge():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", MERGE_SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_meshes_sharing_a_material_merge_in_the_root_space(merge):
    assert merge["merged"] == 2
    assert merge["meshes"] == 2
    assert merge["materials"] == ["0000ff", "ff0000"]
    assert merge["redVertices"] == 48
    assert merge["redMin"] == [-0.5, -0.5, -0.5]
    assert merge["redMax"] == [2.5, 0.5, 0.5]
    assert merge["redParent"] is True
    assert merge["counts"] == [2, 2]


def test_the_coral_garden_is_merged_and_sways_in_the_vertex_shader():
    aquarium = (WORLD / "scene" / "office-aquarium.js").read_text(encoding="utf-8")
    for name in ("staghorn", "plate", "brain"):
        assert f"mergeMeshesByMaterial(THREE, {name});" in aquarium
    assert aquarium.count("mergeMeshesByMaterial(THREE, coral);") == 2
    assert "mergeMeshesByMaterial(THREE, sand);" in aquarium
    assert aquarium.count("mergeSwayingParts(THREE, ") == 3
    assert 'material.customProgramCacheKey = () => "forkmesh-aquarium-sway";' in aquarium
    assert "vec3 transformed = swayPivot + aquariumSway * swayLocal;" in aquarium
    assert "tentacle.rotation.x =" not in aquarium
    assert "restRotationZ" not in aquarium
    pair = aquarium[aquarium.index("const coralMaterialPair = "):]
    pair = pair[: pair.index("});")]
    assert "transparent" not in pair


def test_the_logo_reflection_skips_the_aquarium_decor():
    aquarium = (WORLD / "scene" / "office-aquarium.js").read_text(encoding="utf-8")
    for name in ("sand", "reef", "coralGarden", "userFishAnchors", "bubbles", "food", "waterSurface", "particles", "caustics"):
        assert f"{name}.userData.logoReflectionSkip = true;" in aquarium
    displays = world_scene_module("districts/office-displays.js")
    mark = displays[displays.index("function markLogoReflectionLayer() {"):]
    mark = mark[: mark.index("\n  function renderLogoReflectionFace(")]
    assert "if (object.userData?.logoReflectionSkip === true) continue;" in mark
    diagnostics = world_scene_module("runtime/diagnostics.js")
    assert "if (object.userData?.logoReflectionSkip === true) continue;" in diagnostics


def test_reflection_faces_render_without_a_matrix_walk_or_shadow_refresh():
    displays = world_scene_module("districts/office-displays.js")
    face = displays[displays.index("function renderLogoReflectionFace(face) {"):]
    face = face[: face.index("\n  function logoCubeOnScreen(")]
    render = face.index("renderer.render(ctx.scene, reflectionCamera.children[face]);")
    assert face.index("ctx.scene.matrixWorldAutoUpdate = false;") < render
    assert face.index("renderer.shadowMap.needsUpdate = false;") < render
    assert face.index("ctx.scene.matrixWorldAutoUpdate = matrixAutoUpdate;") > render
    assert face.index("renderer.shadowMap.needsUpdate = shadowUpdate;") > render


def test_the_first_capture_waits_for_its_programs_and_an_on_screen_cube():
    displays = world_scene_module("districts/office-displays.js")
    update = displays[displays.index("function updateOfficeLogoReflection(time) {"):]
    update = update[: update.index("\n  function applyOfficeLogoEnvironment(")]
    assert "void fns.warmLogoReflection().then(() => {" in update
    assert 'if (logoReflectionWarm === "pending" || !logoCubeOnScreen()) return;' in update
    diagnostics = world_scene_module("runtime/diagnostics.js")
    warm = diagnostics[diagnostics.index("function warmLogoReflection("):]
    warm = warm[: warm.index("\n  function scheduleGpuWarmup(")]
    assert "renderer.setRenderTarget(target, 0);" in warm
    assert "pending = ctx.programPacer.paced(() =>" in warm
    assert "if (!ctx.uploadBudget.takeTexture(texture)) break;" in warm
    assert "renderer.properties.get(material).currentProgram = program;" in warm
