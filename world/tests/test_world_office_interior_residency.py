import json
import shutil
import subprocess
from pathlib import Path

import pytest


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_DIR = WORLD / "scene"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
NODE = shutil.which("node")


def node(script: str) -> dict:
    if not NODE:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        timeout=60,
    )
    return json.loads(result.stdout)


def module_text(relative: str) -> str:
    return (SCENE_DIR / relative).read_text(encoding="utf-8")


def block(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin)]


ATLAS_HARNESS = """
  import * as THREE from {three};
  import {{ mergeObjectBatches, refreshBatchAtlas, disposeBatchAtlas }} from {batching};
  const draws = [];
  const fakeCanvas = (width, height) => ({{
    width,
    height,
    getContext: () => ({{
      clearRect() {{}},
      drawImage(image, ...args) {{ draws.push([image.id, ...args]); }},
    }}),
  }});
  globalThis.document = {{ createElement: () => fakeCanvas(0, 0) }};
  const sign = (id, width, height, x, options = {{}}) => {{
    const image = fakeCanvas(width, height);
    image.id = id;
    const texture = new THREE.CanvasTexture(image);
    const mesh = new THREE.Mesh(
      new THREE.PlaneGeometry(2, 1),
      new THREE.MeshBasicMaterial({{ map: texture, ...options }}),
    );
    mesh.position.x = x;
    return mesh;
  }};
  const root = new THREE.Group();
  root.add(
    sign("a", 512, 128, 0),
    sign("b", 256, 256, 3),
    sign("c", 300, 60, 6, {{ color: "#ff0000" }}),
    sign("d", 128, 128, 9, {{ transparent: true, opacity: 0.5 }}),
  );
  const repeated = sign("e", 64, 64, 12);
  repeated.material.map.repeat.set(2, 1);
  root.add(repeated);
  {body}
"""


def atlas_run(body: str) -> dict:
    return node(
        ATLAS_HARNESS.format(
            three=json.dumps(THREE.as_uri()),
            batching=json.dumps((SCENE_DIR / "batching.js").as_uri()),
            body=body,
        )
    )


def test_far_view_signs_share_one_atlas_texture_per_material_state():
    result = atlas_run(
        """
  const plain = mergeObjectBatches(THREE, root);
  const merged = mergeObjectBatches(THREE, root, { atlasScale: 0.5 });
  const atlasMeshes = merged.children.filter((mesh) => mesh.userData.batchAtlas);
  const opaque = atlasMeshes.find((mesh) => !mesh.material.transparent);
  const uv = Array.from(opaque.geometry.attributes.uv.array);
  const colors = Array.from(opaque.geometry.attributes.color.array);
  const atlas = opaque.userData.batchAtlas;
  const cells = [...atlas.cells.values()].map((cell) => [cell.texture.image.id, cell.width, cell.height]);
  process.stdout.write(JSON.stringify({
    plainDraws: plain.userData.drawCount,
    mergedDraws: merged.userData.drawCount,
    atlasMeshes: atlasMeshes.length,
    atlasCount: merged.userData.atlases.length,
    opaqueVertices: opaque.geometry.attributes.position.count,
    uvInside: uv.every((value) => value > 0 && value < 1),
    reds: colors.filter((value, index) => index % 4 === 0 && value === 1).length,
    greens: colors.filter((value, index) => index % 4 === 1 && value === 0).length,
    vertexColors: opaque.material.vertexColors,
    mapIsAtlas: opaque.material.map === atlas.texture,
    width: atlas.width,
    height: atlas.height,
    cells,
    shared: merged.userData.sharedTextures.size,
  }));
"""
    )
    assert result["plainDraws"] == 5
    assert result["mergedDraws"] == 3
    assert result["atlasMeshes"] == 2
    assert result["atlasCount"] == 2
    assert result["opaqueVertices"] == 12
    assert result["uvInside"] is True
    assert result["reds"] == 12
    assert result["greens"] == 4
    assert result["vertexColors"] is True
    assert result["mapIsAtlas"] is True
    assert result["width"] >= 256 and result["height"] >= 1
    assert sorted(result["cells"]) == [["a", 256, 64], ["b", 128, 128], ["c", 150, 30]]
    assert result["shared"] == 1


def test_atlas_cells_follow_their_repainted_source_and_dispose_with_the_proxy():
    result = atlas_run(
        """
  const merged = mergeObjectBatches(THREE, root, { atlasScale: 0.5 });
  const atlas = merged.children.find((mesh) => mesh.userData.batchAtlas && !mesh.material.transparent).userData.batchAtlas;
  const before = atlas.texture.version;
  const idle = refreshBatchAtlas(atlas);
  const source = [...atlas.cells.keys()].find((texture) => texture.image.id === "b");
  source.needsUpdate = true;
  draws.length = 0;
  const redrawn = refreshBatchAtlas(atlas);
  let disposed = 0;
  atlas.texture.addEventListener("dispose", () => { disposed += 1; });
  atlas.material.addEventListener("dispose", () => { disposed += 1; });
  disposeBatchAtlas(atlas);
  process.stdout.write(JSON.stringify({
    idle,
    redrawn,
    redrawnIds: [...new Set(draws.map((call) => call[0]))],
    bumped: atlas.texture.version > before,
    disposed,
    cellsLeft: atlas.cells.size,
  }));
"""
    )
    assert result["idle"] == 0
    assert result["redrawn"] == 1
    assert result["redrawnIds"] == ["b"]
    assert result["bumped"] is True
    assert result["disposed"] == 2
    assert result["cellsLeft"] == 0


def test_office_far_proxy_atlases_its_signs_and_keeps_them_current():
    floors = module_text("districts/office-floors.js")
    begin = block(floors, "  function beginOfficeFarProxy() {", "\n  function finishOfficeFarProxy(")
    assert "atlasScale: OFFICE_FAR_ATLAS_SCALE," in begin
    finish = block(floors, "  function finishOfficeFarProxy(", "\n  function rebuildOfficeFarProxy(")
    assert "officeFarAtlases.forEach(disposeBatchAtlas);" in finish
    assert finish.index("officeFarAtlases.forEach(disposeBatchAtlas);") < finish.index(
        "officeFarAtlases = proxy.userData.atlases;"
    )
    level = block(floors, "  function syncOfficeDistanceLevel(detailed) {", "\n  function officeFloorFinishMaterial(")
    assert "if (!detailed) officeFarAtlases.forEach(refreshBatchAtlas);" in level


def test_scene_construction_no_longer_builds_the_aquarium():
    floors = module_text("districts/office-floors.js")
    build = floors[floors.index("export async function build({"):]
    assert "createOfficeMarineAquariumSliced" not in build
    assert "createAquariumControlPanel" not in build
    assert "ctx.officeAquarium" not in build
    core = module_text("core.js")
    construction = block(core, "await officeFloorsDistrict.build({", "});")
    assert "aquariumSavedToggle" not in construction
    assert "animated," not in construction


def test_aquarium_residency_builds_on_approach_and_releases_with_hysteresis():
    floors = module_text("districts/office-floors.js")
    assert "const OFFICE_AQUARIUM_RELEASE_MARGIN = 80;" in floors
    assert "const OFFICE_AQUARIUM_RELEASE_MS = 15_000;" in floors
    sync = block(floors, "  function syncOfficeAquariumResidency(", "\n  function setOfficeAquariumUsers(")
    assert 'if (ctx.officeSceneMode !== "town" || cameraDistance <= detailDistance) {' in sync
    assert "if (!ctx.officeAquarium) whenOfficeAquariumReady();" in sync
    assert "cameraDistance <= detailDistance + OFFICE_AQUARIUM_RELEASE_MARGIN" in sync
    assert "now - officeAquariumFarSince < OFFICE_AQUARIUM_RELEASE_MS" in sync
    assert sync.index("officeAquariumFarSince = now;") < sync.index("releaseOfficeAquarium();")
    quality = module_text("runtime/quality-environment.js")
    visibility = block(quality, "  function syncOfficeFloorVisibility() {", "\n  function applyRendererResolution(")
    assert visibility.index("fns.syncOfficeAquariumResidency(cameraDistance, exteriorDetailDistance);") < visibility.index(
        "if (floorKey === officeFloorVisibilityKey) return;"
    )


def test_aquarium_build_and_release_hold_the_point_light_count():
    floors = module_text("districts/office-floors.js")
    build = block(floors, "  async function buildOfficeAquarium(pause) {", "\n  function disposeOfficeAquariumGroup(")
    assert build.index("object.visible = false;") < build.index("await fns.warmBeforeReveal(officeAquarium.group);")
    assert build.index("ctx.officeInterior.add(officeAquarium.group);") < build.index(
        "lights.forEach((light) => ctx.localPointLights.add(light));"
    )
    assert "fns.updateLocalPointLightBudget();" in build
    assert "officeAquarium.group.userData.farProxySkip = true;" in build
    assert "interactive: targets," in build
    assert build.index("fns.instanceRepeatedMeshesWithin(officeAquarium.group);") < build.index(
        "await fns.warmBeforeReveal(officeAquarium.group);"
    )
    assert build.index("ctx.interactive.push(...targets);") > build.index("ctx.officeInterior.add(officeAquarium.group);")
    release = block(floors, "  function releaseOfficeAquarium() {", "\n  function whenOfficeAquariumReady(")
    for step in (
        "ctx.animated.splice(index, 1);",
        "removeInteractiveObject(ctx.interactive, group);",
        "fns.releaseStaticMirrorsWithin(group);",
        "ctx.localPointLights.delete(object);",
        "group.removeFromParent();",
        "fns.updateLocalPointLightBudget();",
        "disposeOfficeAquariumGroup(group);",
    ):
        assert step in release, step
    assert release.index("ctx.localPointLights.delete(object);") < release.index("fns.updateLocalPointLightBudget();")
    assert release.index("group.removeFromParent();") < release.index("fns.updateLocalPointLightBudget();")
    quality = module_text("runtime/quality-environment.js")
    assert "    updateLocalPointLightBudget,\n" in quality


def test_aquarium_disposal_frees_owned_resources_only():
    floors = module_text("districts/office-floors.js")
    dispose = block(floors, "  function disposeOfficeAquariumGroup(group) {", "\n  function releaseOfficeAquarium(")
    assert "if (object.isInstancedMesh) object.dispose();" in dispose
    assert "material.userData?.forkmeshSharedResource === true" in dispose
    assert "texture.userData?.forkmeshSharedResource !== true" in dispose
    assert "geometries.forEach(disposeOwnedGeometry);" in dispose
    assert '(slot) => slot !== "envMap"' in floors


def test_aquarium_callers_tolerate_a_released_tank():
    animate = module_text("runtime/animate.js")
    assert "    if (ctx.officeAquarium) {\n      ctx.officeAquarium.setFishBodiesVisible(" in animate
    exits = module_text("runtime/office-exits.js")
    assert exits.count("if (ctx.officeAquarium) ctx.officeAquarium.setFishBodiesVisible(false);") == 2
    presence = module_text("runtime/office-presence.js")
    assert "if (ctx.officeAquarium) ctx.officeAquarium.setFishBodiesVisible(true);" in presence
    remote = module_text("runtime/remote-players.js")
    assert 'fns.queueLoungeTask("aquarium", () => fns.setOfficeAquariumUsers(members));' in remote
    floors = module_text("districts/office-floors.js")
    users = block(floors, "  function setOfficeAquariumUsers(members) {", "\n  function aquariumInCameraView(")
    assert "officeAquariumUsers = members;" in users
    build = block(floors, "  async function buildOfficeAquarium(pause) {", "\n  function disposeOfficeAquariumGroup(")
    assert "if (officeAquariumUsers) officeAquarium.setUsers(officeAquariumUsers);" in build
    for name, guard in (
        ("  function feedOfficeAquarium(", "if (!ctx.officeAquarium) return false;"),
        ("  function getOfficeAquariumState(", "if (!ctx.officeAquarium) return null;"),
        ("  function updateOfficeAquariumProximity(", "if (!ctx.officeAquarium) {"),
    ):
        body = floors[floors.index(name):]
        assert body[: body.index("\n  }\n")].count(guard) == 1, name
    core = module_text("core.js")
    api = core[core.rindex("  return {"):]
    assert "    whenOfficeAquariumReady,\n" in api


def test_lazy_aquarium_meshes_are_instanced_like_construction_time_ones():
    diagnostics = module_text("runtime/diagnostics.js")
    every = block(diagnostics, "  function instanceRepeatedStaticMeshes() {", "\n  function mirrorRepeatedMeshes(")
    assert "for (const root of roots) added += mirrorRepeatedMeshes(root);" in every
    within = block(diagnostics, "  function instanceRepeatedMeshesWithin(root) {", "\n  function releaseStaticMirrorsWithin(")
    assert "mirrors.forEach((mirror) => mirror.sync(null, { force: true }));" in within
    release = block(diagnostics, "  function releaseStaticMirrorsWithin(root) {", "\n  function staticMirrorFor(")
    assert "ctx.staticMirrorSets.delete(ancestor);" in release
    assert "ctx.instancedMirrorSets.delete(mirror);" in release
    assert "mirror.dispose();" in release
    assert "sources.forEach((source) => mirror.remove(source));" in release
