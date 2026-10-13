#!/usr/bin/env python3
"""Frame-cost contracts for the World renderer (issues 393 and 397).

Profiling the live page on a real GPU, and again under a 4x CPU throttle,
found four per-frame costs that had nothing to do with what was on screen:

* one physically "transmissive" waterfall made three.js redraw every opaque
  object into an offscreen buffer each frame, doubling draw calls and flipping
  every shader between two output variants;
* three.js recomposed and multiplied the matrix of every object each frame,
  moved or not;
* shaders compiled and canvas plates uploaded on the frame a district first
  came into view (a 400-600ms stall looking toward the Office);
* the point-light budget changed the number of visible lights as a visitor
  walked, and three.js recompiles every lit shader for each new count.

Issue 397 followed the remaining hitches (200-700ms frames in the first
seconds, and whenever a new view came up) to synchronous GPU round trips
queued behind shader compiles:

* three.js reads a program's uniforms on its first draw, which waits for every
  compile ahead of it; the shader compile guard skips such draws for a frame;
* the idle warm-up queued whole districts of shaders at once; it now builds a
  few materials per slice and waits for them;
* the shadow-flipped warm-up pass left unlit materials drawing with a variant
  that had only just started compiling;
* the Office logo's live cube map made three.js filter an empty target inside
  the warm-up, compiling and drawing the filter shaders in one frame;
* shader error checks, the anisotropy limit and the GPU label were further
  synchronous reads first made behind the warm-up.

The matrix walk and the compile guard are exercised in Node (real three.js
for the walk, a stand-in renderer for the guard); the rest is wiring, so it
is pinned at the source.
"""

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_module, world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
THREE_MODULE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
SCENE = world_scene_source()


def run_matrix_script(body):
    script = f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      import {{ installSettledMatrixUpdate }} from {json.dumps(SCENE_MODULE.as_uri())};
      const settled = new THREE.Scene();
      installSettledMatrixUpdate(THREE, settled);
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


# Two identical random hierarchies, one updated by three.js itself and one by
# the settled walk, driven through the same edits.
MIRRORED_HIERARCHIES = """
  const reference = new THREE.Scene();
  const nodes = { settled: [], reference: [] };
  let seed = 7;
  const random = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
  for (const [root, list] of [[settled, nodes.settled], [reference, nodes.reference]]) {
    seed = 7;
    list.push(root);
    for (let index = 0; index < 60; index += 1) {
      const node = index % 3 ? new THREE.Group() : new THREE.Mesh();
      node.position.set(random() * 10, random() * 10, random() * 10);
      node.rotation.set(random(), random(), random());
      node.scale.setScalar(0.5 + random());
      node.visible = random() > 0.3;
      list[Math.floor(random() * list.length)].add(node);
      list.push(node);
    }
  }
  const worst = () => {
    let error = 0;
    nodes.settled.forEach((node, index) => {
      const a = node.matrixWorld.elements;
      const b = nodes.reference[index].matrixWorld.elements;
      for (let k = 0; k < 16; k += 1) error = Math.max(error, Math.abs(a[k] - b[k]));
    });
    return error;
  };
  const both = (edit) => {
    edit(nodes.settled, settled);
    edit(nodes.reference, reference);
    settled.updateMatrixWorld();
    reference.updateMatrixWorld();
    return worst();
  };
"""


def test_settled_walk_matches_three_through_moves_hides_and_reparenting():
    result = run_matrix_script(
        MIRRORED_HIERARCHIES
        + """
      const errors = [];
      errors.push(both(() => {}));
      errors.push(both((list) => { list[4].position.x += 3; }));
      errors.push(both((list) => { list[9].rotation.y = 1.2; list[20].scale.set(2, 1, 1); }));
      // A hidden branch whose ancestor moves must still be current: picking
      // proxies and ground probes read hidden objects.
      errors.push(both((list) => { list[12].visible = false; list[1].position.z -= 5; }));
      errors.push(both((list) => { list[12].visible = true; }));
      // Reparenting keeps the local transform and must still be noticed.
      errors.push(both((list) => { list[30].parent.remove(list[30]); list[2].add(list[30]); }));
      // Manual matrices are honoured through matrixWorldNeedsUpdate.
      errors.push(both((list) => {
        list[40].matrixAutoUpdate = false;
        list[40].matrix.makeTranslation(1, 2, 3);
        list[40].matrixWorldNeedsUpdate = true;
      }));
      process.stdout.write(JSON.stringify(errors));
    """
    )
    assert all(error < 1e-9 for error in result), result


def test_settled_walk_skips_unmoved_objects_and_ignores_get_world_position():
    result = run_matrix_script(
        MIRRORED_HIERARCHIES
        + """
      settled.updateMatrixWorld();
      let multiplies = 0;
      const multiply = THREE.Matrix4.prototype.multiplyMatrices;
      THREE.Matrix4.prototype.multiplyMatrices = function (...args) {
        multiplies += 1;
        return multiply.apply(this, args);
      };
      settled.updateMatrixWorld();
      const idle = multiplies;
      // getWorldPosition raises matrixWorldNeedsUpdate on every ancestor. It
      // must not make the whole scene look dirty on the next frame.
      multiplies = 0;
      nodes.settled[50].getWorldPosition(new THREE.Vector3());
      const afterQuery = multiplies;
      multiplies = 0;
      settled.updateMatrixWorld();
      const afterQueryFrame = multiplies;
      multiplies = 0;
      nodes.settled[5].position.y += 1;
      let subtree = 0;
      nodes.settled[5].traverse(() => { subtree += 1; });
      settled.updateMatrixWorld();
      const moved = multiplies;
      THREE.Matrix4.prototype.multiplyMatrices = multiply;
      process.stdout.write(JSON.stringify({ idle, afterQuery, afterQueryFrame, moved, subtree }));
    """
    )
    assert result["idle"] == 0
    assert result["afterQueryFrame"] == 0
    assert result["moved"] == result["subtree"]


def test_quiet_objects_freeze_and_still_follow_every_kind_of_edit():
    result = run_matrix_script(
        MIRRORED_HIERARCHIES
        + """
      const settle = () => { for (let walk = 0; walk < 40; walk += 1) both(() => {}); };
      settle();
      const frozen = nodes.settled.slice(1).filter((node) => node.matrixAutoUpdate === false).length;
      const errors = [];
      errors.push(both((list) => { list[33].position.x += 2; }));
      settle();
      errors.push(both((list) => { list[34].position.set(1, 2, 3); }));
      settle();
      errors.push(both((list) => { list[35].rotation.y += 0.7; }));
      settle();
      errors.push(both((list) => { list[36].quaternion.set(0, 0.6, 0, 0.8); }));
      settle();
      errors.push(both((list) => { list[37].scale.multiplyScalar(1.5); }));
      settle();
      errors.push(both((list) => { list[38].lookAt(4, 5, 6); }));
      settle();
      errors.push(both((list) => { list[39].translateZ(2); list[39].rotateX(0.3); }));
      settle();
      errors.push(both((list) => { list[1].position.z -= 5; }));
      settle();
      errors.push(both((list) => { list[30].parent.remove(list[30]); list[2].add(list[30]); }));
      settle();
      errors.push(both((list) => {
        const node = list[41];
        const parent = node.parent;
        parent.remove(node);
        parent.parent?.position.set(3, 3, 3);
        parent.add(node);
      }));
      settle();
      errors.push(both((list) => {
        const clone = list[42].clone(false);
        clone.position.x += 4;
        list[3].add(clone);
        list.push(clone);
      }));
      settle();
      errors.push(both((list, root) => {
        const node = new THREE.Group();
        node.position.set(7, 0, 1);
        list[44].add(node);
        list.push(node);
      }));
      settle();
      errors.push(both((list) => { list[45].attach(list[46]); }));
      const spread = { ...nodes.settled[47].position };
      const json = JSON.parse(JSON.stringify(nodes.settled[47].position));
      const copy = nodes.settled[47].position.clone();
      process.stdout.write(JSON.stringify({
        frozen,
        errors,
        spread: Object.keys(spread),
        json: Object.keys(json),
        plainClone: Object.getOwnPropertyNames(copy),
      }));
    """
    )
    assert result["frozen"] > 50, result
    assert all(error < 1e-9 for error in result["errors"]), result
    assert result["spread"] == ["x", "y", "z"]
    assert result["json"] == ["x", "y", "z"]
    assert result["plainClone"] == ["x", "y", "z"]


def test_a_frozen_scene_walk_visits_only_the_branch_that_moved():
    result = run_matrix_script(
        MIRRORED_HIERARCHIES
        + """
      for (let walk = 0; walk < 40; walk += 1) settled.updateMatrixWorld();
      let reads = 0;
      const watch = (node) => {
        Object.defineProperty(node, "children", {
          configurable: true,
          get() { reads += 1; return this._watchedChildren; },
        });
      };
      nodes.settled.forEach((node) => {
        node._watchedChildren = node.children;
        watch(node);
      });
      settled.updateMatrixWorld();
      const idle = reads;
      reads = 0;
      nodes.settled[25].position.y += 1;
      let path = 0;
      for (let node = nodes.settled[25].parent; node; node = node.parent) path += 1;
      let subtree = 0;
      nodes.settled[25].traverse(() => { subtree += 1; });
      reads = 0;
      settled.updateMatrixWorld();
      const moved = reads;
      process.stdout.write(JSON.stringify({ idle, moved, path, subtree }));
    """
    )
    assert result["idle"] == 1, result
    assert result["moved"] == result["path"] + result["subtree"], result


def run_geometry_script(body):
    script = f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      import {{ createGeometryDeduper, indexGeometry }} from {json.dumps(SCENE_MODULE.as_uri())};
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_identical_stable_shapes_share_one_geometry():
    result = run_geometry_script(
        """
      const scene = new THREE.Scene();
      const material = new THREE.MeshBasicMaterial();
      const mesh = (geometry) => {
        const object = new THREE.Mesh(geometry, material);
        scene.add(object);
        return object;
      };
      const a = mesh(new THREE.BoxGeometry(1, 2, 3));
      const b = mesh(new THREE.BoxGeometry(1, 2, 3));
      const moved = mesh(new THREE.BoxGeometry(1, 2, 3).translate(0, 1, 0));
      const wave = mesh(new THREE.PlaneGeometry(2, 2, 4, 4));
      const still = mesh(new THREE.PlaneGeometry(2, 2, 4, 4));
      const deduper = createGeometryDeduper(THREE);
      deduper.sweep(scene);
      const afterFirstSweep = a.geometry !== b.geometry;
      wave.geometry.attributes.position.setY(0, 5);
      wave.geometry.attributes.position.needsUpdate = true;
      deduper.sweep(scene);
      deduper.sweep(scene);
      process.stdout.write(JSON.stringify({
        afterFirstSweep,
        shared: a.geometry === b.geometry,
        sharedFlag: a.geometry.userData.forkmeshSharedResource === true,
        movedKept: moved.geometry !== a.geometry,
        dynamicKept: wave.geometry !== still.geometry,
        stats: deduper.stats,
      }));
    """
    )
    assert result["afterFirstSweep"] is True
    assert result["shared"] is True
    assert result["sharedFlag"] is True
    assert result["movedKept"] is True
    assert result["dynamicKept"] is True
    assert result["stats"] == {"replaced": 1, "dynamic": 1}


def test_index_geometry_merges_shared_vertices_and_keeps_groups():
    result = run_geometry_script(
        """
      const shape = new THREE.Shape([
        new THREE.Vector2(0, 0),
        new THREE.Vector2(1, 0),
        new THREE.Vector2(1, 1),
      ]);
      const source = new THREE.ExtrudeGeometry(shape, {
        depth: 0.3,
        bevelEnabled: true,
        bevelSegments: 2,
      });
      const vertices = source.attributes.position.count;
      const groups = JSON.stringify(source.groups);
      const indexed = indexGeometry(THREE, source);
      process.stdout.write(JSON.stringify({
        vertices,
        kept: indexed.attributes.position.count,
        indices: indexed.index.count,
        groups: groups === JSON.stringify(indexed.groups),
      }));
    """
    )
    assert result["kept"] < result["vertices"]
    assert result["indices"] == result["vertices"]
    assert result["groups"] is True


def test_world_scene_runs_the_geometry_sweep_and_indexes_the_logo_panel():
    assert "scheduleGeometryDedupe();" in SCENE
    assert "cancelGeometryDedupe();" in SCENE
    assert "logoPanelGeometry = indexGeometry(THREE, logoPanelGeometry);" in SCENE


def test_force_keeps_its_full_three_meaning():
    result = run_matrix_script(
        """
      const child = new THREE.Group();
      settled.add(child);
      settled.updateMatrixWorld();
      // An out-of-band write the settled walk would not notice on its own.
      child.matrixWorld.makeTranslation(9, 9, 9);
      settled.updateMatrixWorld(true);
      process.stdout.write(JSON.stringify(child.matrixWorld.elements[12]));
    """
    )
    assert result == 0


def test_scene_installs_the_settled_walk_before_anything_is_added():
    install = "installSettledMatrixUpdate(THREE, scene);"
    assert install in SCENE
    assert SCENE.index("const scene = new THREE.Scene();") < SCENE.index(install)
    # Cameras and skinned meshes keep derived matrices in their own override.
    assert "child.isCamera === true || child.isSkinnedMesh === true" in SCENE


def test_no_world_material_requests_a_transmission_pass():
    assert "transmission:" not in SCENE


def test_garden_shrubs_draw_as_one_instanced_mesh():
    assert "const gardenPlants = new THREE.InstancedMesh(" in SCENE
    assert 'gardenPlants.name = "office-garden-plant";' in SCENE
    assert "gardenPlants.setColorAt(" in SCENE


def test_gpu_warmup_compiles_and_uploads_off_the_critical_path():
    warmup = SCENE[SCENE.index("function scheduleGpuWarmup() {"):]
    warmup = warmup[: warmup.index("function cancelGpuWarmup() {")]
    assert "void programPacer.paced(() =>\n          warmMaterialsAsync(\n            renderer,\n            batchRoot(entries.map((entry) => entry.object))," in warmup
    assert "renderer.initTexture(texture)" in warmup
    assert "const GPU_WARMUP_MATERIALS_PER_SLICE = 4;" in SCENE
    assert "if (shaderCompileGuard.pendingPrograms() === 0) {" in warmup
    assert "fresh < GPU_WARMUP_MATERIALS_PER_SLICE" in warmup
    assert "seenVariants.has(entry.key)" in warmup
    assert "window.requestIdleCallback(" in warmup
    assert "compactDistrictResident === false" in warmup
    assert "if (compactRenderer) return;" in warmup
    assert "scheduleGpuWarmup();\n\n  return {" in SCENE
    assert "function dispose() {\n    cancelGpuWarmup();" in SCENE


def test_gpu_warmup_rescans_the_scene_nearest_first():
    warmup = SCENE[SCENE.index("function scheduleGpuWarmup() {"):]
    warmup = warmup[: warmup.index("function cancelGpuWarmup() {")]
    assert "const GPU_WARMUP_RESCAN_MS = 4_000;" in SCENE
    assert "nextScanAt = performance.now() + GPU_WARMUP_RESCAN_MS;" in warmup
    assert "return [...found.values()].sort((a, b) => a.distance - b.distance);" in warmup
    assert "object.userData?.raycastProxy !== true" in warmup
    assert "object.layers.test(camera.layers)" in warmup
    assert "material.visible === false" in warmup
    assert "shaderCompileGuard.primeReadyPrograms(GPU_WARMUP_PRIME_MS);" in warmup
    assert "compileWithFlippedShadows" not in SCENE


def test_gpu_warmup_compiles_the_lobby_reflection_against_its_target():
    warmup = SCENE[SCENE.index("function scheduleGpuWarmup() {"):]
    warmup = warmup[: warmup.index("function cancelGpuWarmup() {")]
    assert "GPU_WARMUP_REFLECTION_REACH ** 2" in warmup
    assert "logoReflectionRoots()" in warmup
    assert 'if (floorId !== "lobby") otherFloors.add(group);' in warmup
    compile_entries = warmup[warmup.index("const compileEntries = "):]
    compile_entries = compile_entries[: compile_entries.index("const compileNext = ")]
    bound = compile_entries.index("renderer.setRenderTarget(target, 0);")
    compiled = compile_entries.index("warmMaterialsAsync(")
    restored = compile_entries.index("renderer.properties.get(material).currentProgram = program;")
    assert bound < compiled < restored
    assert "compileEntries(reflected, reflectionTarget);" in warmup


def test_point_light_count_stays_steady_while_walking():
    budget = SCENE[SCENE.index("function updateLocalPointLightBudget("):]
    budget = budget[: budget.index("function syncOfficeFloorVisibility()")]
    assert (
        "const enabled = Math.min(budget, localPointLightCandidates.length);"
        in budget
    )
    assert "localPointLightCandidates[index].visible = index < enabled;" in budget
    assert "new Set(" not in budget
    assert (
        "padPointLightSlots(qualityProfile.pointLightBudget - enabled);"
        in budget
    )
    slots = SCENE[SCENE.index("function padPointLightSlots(count) {"):]
    slots = slots[: slots.index("\n  }\n") + 4]
    assert 'new THREE.PointLight("#000000", 0, 0.01, 2)' in slots
    assert "slot.visible = index < wanted;" in slots


def test_render_cadence_accepts_early_vsync_callbacks():
    loop = SCENE[SCENE.index("  function animate(time) {"):]
    loop = loop[: loop.index("const frameWorkStartedAt")]
    assert "Math.min(displayFrameIntervalMs, renderFrameIntervalMs) * 0.5" in loop
    assert "time + earlyToleranceMs < nextRenderFrameAt" in loop
    assert "time - nextRenderFrameAt > renderFrameIntervalMs * 2" in loop


def test_distant_office_is_the_merged_furnished_tower_not_a_stand_in():
    office = SCENE[SCENE.index("function createForkMeshOffice("):]
    office = office[: office.index("\n}\n")]
    assert "#1b6654" not in office
    assert "lowTower" not in office
    assert "lowFloors" not in office
    assert "lowIsland" not in office
    assert 'low.name = "forkmesh-office-low-poly";' in office
    batches = SCENE[SCENE.index("function* mergeObjectBatchesSteps("):]
    batches = batches[: batches.index("\n}\n")]
    assert "batchMaterialKey(THREE, material, object)" in batches
    assert "new THREE.Float32BufferAttribute(batch.colors, 4)" in batches
    assert "mesh.castShadow = false;" in batches


def test_office_stands_on_the_lawn_without_a_green_island_disc():
    assert "forkmesh-office-remote-island" not in SCENE
    assert "forkmesh-office-low-island" not in SCENE
    assert "OFFICE_ISLAND_RADIUS + 5" not in SCENE
    assert "tree.position.set(x, CONTINUOUS_CITY_SURFACE_Y, z);" in SCENE
    assert "flower.position.set(x, CONTINUOUS_CITY_SURFACE_Y, z);" in SCENE


def test_office_island_has_no_glowing_rim():
    # The emissive torus traced the island edge as a green ring around the
    # Office from all of Town.
    assert "islandRim" not in SCENE
    assert "lowIslandRim" not in SCENE
    assert "forkmesh-office-remote-island-rim" not in SCENE


def test_atmosphere_tracks_the_sky():
    assert "scene.fog = new THREE.Fog(" in SCENE
    assert "scene.fog?.color.copy(state.background);" in SCENE
    assert "scene.environment = skyEnvironment?.texture || null;" in SCENE
    assert "scene.environmentIntensity =" in SCENE
    # Phones keep hemisphere-only shading, and a restored context rebuilds
    # the environment render target it lost.
    env = SCENE[SCENE.index("function buildSkyEnvironment() {"):]
    assert env.index("if (compactRenderer) return;") < env.index("bakeSkyEnvironment(THREE, renderer)")
    restored = SCENE[SCENE.index("const handleContextRestored = () => {"):]
    assert restored.index("buildSkyEnvironment();") < restored.index("resize();")


def run_guard_script(body):
    script = f"""
      import {{ installShaderCompileGuard }} from {json.dumps(SCENE_MODULE.as_uri())};
      let clock = 0;
      const renderer = {{
        info: {{ programs: [] }},
        drawn: [],
        // Stands in for three.js: setProgram reads the program's uniforms
        // (the blocking first use), then the object is drawn.
        renderBufferDirect(camera, scene, geometry, material) {{
          material.program.getUniforms();
          this.drawn.push(material.name);
        }},
      }};
      const view = {{ camera: {{}}, scene: {{}} }};
      const guard = installShaderCompileGuard(
        renderer,
        (camera, scene) => camera === view.camera && scene === view.scene,
        () => clock,
      );
      const program = (name, ready = false) => {{
        const created = {{
          name,
          ready,
          program: {{}},
          reads: 0,
          isReady() {{ return this.ready; }},
          getUniforms() {{ this.reads += 1; return {{}}; }},
        }};
        renderer.info.programs.push(created);
        return {{ name, program: created }};
      }};
      const draw = (material, camera = view.camera, scene = view.scene) =>
        renderer.renderBufferDirect(camera, scene, null, material, {{}}, null);
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_compile_guard_skips_main_view_draws_until_the_program_is_ready():
    result = run_guard_script(
        """
      const plate = program("plate");
      draw(plate);
      const held = { drawn: [...renderer.drawn], reads: plate.program.reads };
      // A shadow pass or cube capture is not redrawn every frame: it keeps
      // three.js's blocking behaviour.
      draw(plate, {}, null);
      const shadow = [...renderer.drawn];
      plate.program.ready = true;
      draw(plate);
      process.stdout.write(JSON.stringify({
        held,
        shadow,
        drawn: renderer.drawn,
        deferred: guard.deferredDraws,
        pending: guard.pendingPrograms(),
      }));
    """
    )
    assert result["held"] == {"drawn": [], "reads": 0}
    assert result["shadow"] == ["plate"]
    assert result["drawn"] == ["plate", "plate"]
    assert result["deferred"] == 1
    assert result["pending"] == 0


def test_compile_guard_holds_first_use_behind_other_compiles_but_not_forever():
    result = run_guard_script(
        """
      const ready = program("ready", true);
      const compiling = program("compiling");
      draw(ready);
      const behindQueue = renderer.drawn.length;
      clock += 1_000;
      draw(ready);
      const stillHeld = renderer.drawn.length;
      clock += 600;
      draw(ready);
      const released = [...renderer.drawn];
      // Once used, a program is never held again.
      draw(ready);
      process.stdout.write(JSON.stringify({
        behindQueue,
        stillHeld,
        released,
        drawn: renderer.drawn,
        compilingReads: compiling.program.reads,
        pending: guard.pendingPrograms(),
      }));
    """
    )
    assert result["behindQueue"] == 0
    assert result["stillHeld"] == 0
    assert result["released"] == ["ready"]
    assert result["drawn"] == ["ready", "ready"]
    assert result["compilingReads"] == 0
    assert result["pending"] == 1


def test_compile_guard_forgets_released_programs_and_follows_a_restored_context():
    result = run_guard_script(
        """
      const disposed = program("disposed");
      // three.js clears .program when it deletes a released program; it
      // never reports ready and must not hold every later first draw.
      disposed.program.program = undefined;
      const afterRelease = guard.pendingPrograms();
      // A restored context starts a fresh program list.
      renderer.info.programs = [];
      const fresh = program("fresh");
      draw(fresh);
      const heldAfterRestore = renderer.drawn.length;
      fresh.program.ready = true;
      draw(fresh);
      let failure = "";
      try {
        draw({ name: "broken", program: { getUniforms() { throw new Error("boom"); } } });
      } catch (error) {
        failure = error.message;
      }
      process.stdout.write(JSON.stringify({
        afterRelease,
        heldAfterRestore,
        drawn: renderer.drawn,
        failure,
      }));
    """
    )
    assert result["afterRelease"] == 0
    assert result["heldAfterRestore"] == 0
    assert result["drawn"] == ["fresh"]
    # Anything other than a pending program still surfaces.
    assert result["failure"] == "boom"


def test_scene_guards_the_main_view_and_shadow_passes():
    install = SCENE[SCENE.index("const shaderCompileGuard = installShaderCompileGuard("):]
    install = install[: install.index(");") + 2]
    assert "(drawScene === scene && deferrableCameras.has(drawCamera)) || drawScene === null" in install
    assert "const deferrableCameras = new Set([camera]);" in SCENE
    assert "reflectionCamera.children.forEach((face) => deferrableCameras.add(face));" in SCENE
    assert SCENE.index("const renderer = preparedContext?.renderer || new THREE.WebGLRenderer(") < SCENE.index(
        "const shaderCompileGuard = installShaderCompileGuard("
    )
    assert "shaderCompileDeferrals: shaderCompileGuard.deferredDraws," in SCENE


def test_shadow_switches_keep_the_shadow_programs():
    apply = SCENE[SCENE.index("function applyShadowQuality("):]
    apply = apply[: apply.index("\n  }\n")]
    assert "const programs = on || worldQualityProfile(bootQualityTier).shadows === true;" in apply
    assert "sun.castShadow = programs;" in apply
    assert "renderer.shadowMap.enabled = programs;" in apply
    assert "sun.shadow.intensity = on ? 1 : 0;" in apply
    animate = world_scene_module("runtime/animate.js")
    refresh = animate[animate.index("let refreshedShadowMap = false;"):]
    refresh = refresh[: refresh.index("const deferredShadowDrawsBefore")]
    assert "ctx.sun.shadow.intensity > 0 &&" in refresh


def test_office_logo_reflection_is_filtered_with_the_kept_sky_generator():
    env = SCENE[SCENE.index("function bakeSkyEnvironment(THREE, renderer, prepared = null) {"):]
    env = env[: env.index("\n}\n")]
    # The generator outlives the bake, and the cube-map shader is compiled
    # against a linear render target, which is the variant the capture uses.
    assert "return { environment, pmrem };" in env
    assert "pmrem.dispose();" not in env[env.rindex("} finally {"):]
    target = env.index("renderer.setRenderTarget(environment);")
    assert target < env.index("pmrem.compileCubemapShader();")
    assert env.index("pmrem.compileCubemapShader();") < env.index(
        "renderer.setRenderTarget(outputTarget);"
    )
    build = SCENE[SCENE.index("function buildSkyEnvironment() {"):]
    build = build[: build.index("function applyShadowQuality(")]
    assert "skyEnvironmentPmrem = baked?.pmrem || null;" in build
    assert "preparedSkyEnvironment = null;" in build
    assert "skyEnvironmentPmrem?.dispose();" in SCENE[SCENE.index("function dispose() {"):]
    # The chrome always holds an already filtered map where the sky exists.
    assert "logoReflectionEnvironment?.texture ||" in SCENE
    assert "skyEnvironment?.texture ||" in SCENE
    capture = SCENE[SCENE.index("function updateOfficeLogoReflection(time) {"):]
    capture = capture[: capture.index("\n  function applyOfficeLogoEnvironment() {")]
    published = capture[capture.index("if (logoReflectionStep === LOGO_REFLECTION_FACES) {"):]
    published = published[: published.index("const cubeWasVisible")]
    assert published.index("if (logoReflectionDeferred) {") < published.index("skyEnvironmentPmrem.fromCubemap(")
    assert "applyOfficeLogoEnvironment();" in published
    assert "logoReflectionEligibleAt = time + LOGO_REFLECTION_RETRY_MS;" in published
    restored = SCENE[SCENE.index("const handleContextRestored = () => {"):]
    assert restored.index("buildSkyEnvironment();") < restored.index(
        "resetOfficeLogoEnvironment();"
    )


def test_synchronous_gpu_reads_happen_before_the_warmup_queue_exists():
    renderer_setup = SCENE[SCENE.index("const renderer = preparedContext?.renderer || new THREE.WebGLRenderer("):]
    renderer_setup = renderer_setup[: renderer_setup.index("const shaderCompileGuard")]
    assert "renderer.debug.checkShaderErrors = checkShaderErrors === true;" in renderer_setup
    assert "renderer.capabilities.getMaxAnisotropy();" in renderer_setup
    assert "  checkShaderErrors = false,\n" in SCENE
    assert "let diagnosticsContextInfo = null;" in SCENE
    construction = world_scene_module("core.js")
    assert construction.index("\n  rendererContextInfo();") < construction.index("\n  scheduleGpuWarmup();")
    assert "rendererGpu: () => rendererContextInfo().gpu," in SCENE
    shell = (WORLD / "world.js").read_text(encoding="utf-8")
    assert 'this.shaderChecksRequested = worldQuery.get("shaderchecks") === "1";' in shell
    assert "checkShaderErrors: this.shaderChecksRequested," in shell
    gpu_label = shell[shell.index("  rendererGpuLabel() {"):]
    gpu_label = gpu_label[: gpu_label.index("  deviceProfile() {")]
    assert gpu_label.index("this.world?.rendererGpu?.()") < gpu_label.index("getContext")


def test_warmup_survives_a_material_disposed_mid_compile():
    script = f"""
      import {{ warmMaterialsAsync }} from {json.dumps(SCENE_MODULE.as_uri())};
      const programs = new Map();
      const kept = {{ name: "kept" }};
      const disposed = {{ name: "disposed" }};
      programs.set(kept, {{ program: {{}}, ready: false, isReady() {{ return this.ready; }} }});
      programs.set(disposed, {{ program: {{}}, ready: false, isReady() {{ return this.ready; }} }});
      const renderer = {{
        compile: () => new Set([kept, disposed]),
        properties: {{ get: (material) => programs.has(material)
          ? {{ currentProgram: programs.get(material) }} : {{}} }},
      }};
      const queue = [];
      const root = {{}};
      const done = warmMaterialsAsync(renderer, root, {{}}, {{}}, (poll) => queue.push(poll));
      programs.delete(disposed);
      queue.shift()();
      programs.get(kept).ready = true;
      queue.shift()();
      const resolved = await done;
      console.log(JSON.stringify({{ resolved: resolved === root, left: queue.length }}));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    assert json.loads(result.stdout) == {"resolved": True, "left": 0}


def test_world_warmup_never_calls_three_compile_async():
    assert "renderer.compileAsync(" not in SCENE
    assert "renderer\n              .compileAsync(" not in SCENE
    assert SCENE.count("warmMaterialsAsync(") >= 3
