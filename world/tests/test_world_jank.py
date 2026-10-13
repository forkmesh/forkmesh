#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
CACHE_MODULE = WORLD / "world-resource-cache.js"
THREE_MODULE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
SCENE = world_scene_source()
SHELL = (WORLD / "world.js").read_text(encoding="utf-8")


def node(script):
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_shadow_pass_draws_wait_for_their_program_and_are_counted():
    result = node(
        f"""
      import {{ installShaderCompileGuard }} from {json.dumps(SCENE_MODULE.as_uri())};
      const renderer = {{
        info: {{ programs: [] }},
        drawn: [],
        renderBufferDirect(camera, scene, geometry, material) {{
          material.program.getUniforms();
          this.drawn.push(material.name);
        }},
      }};
      const view = {{ camera: {{}}, scene: {{}} }};
      const guard = installShaderCompileGuard(
        renderer,
        (camera, scene) => (camera === view.camera && scene === view.scene) || scene === null,
        () => 0,
      );
      const depth = {{ name: "depth", program: {{ ready: false, program: {{}}, reads: 0,
        isReady() {{ return this.ready; }}, getUniforms() {{ this.reads += 1; return {{}}; }} }} }};
      renderer.info.programs.push(depth.program);
      renderer.renderBufferDirect({{}}, null, null, depth, {{}}, null);
      const held = {{ drawn: [...renderer.drawn], reads: depth.program.reads,
        shadow: guard.deferredShadowDraws, all: guard.deferredDraws }};
      depth.program.ready = true;
      renderer.renderBufferDirect({{}}, null, null, depth, {{}}, null);
      process.stdout.write(JSON.stringify({{ held, drawn: renderer.drawn, shadow: guard.deferredShadowDraws }}));
    """
    )
    assert result["held"] == {"drawn": [], "reads": 0, "shadow": 1, "all": 1}
    assert result["drawn"] == ["depth"]
    assert result["shadow"] == 1


def test_a_shadow_refresh_that_deferred_draws_is_retried_soon():
    loop = SCENE[SCENE.index("    const deferredShadowDrawsBefore"):]
    loop = loop[: loop.index("scheduleRenderStallWarning")]
    assert "shaderCompileGuard.deferredShadowDraws !== deferredShadowDrawsBefore" in loop
    assert "nextShadowMapUpdateAt = time + SHADOW_MAP_DEFERRED_RETRY_MS;" in loop
    assert "if (refreshedShadowMap && !shadowDrawsDeferred) {" in loop


def test_static_mirror_sets_sync_on_staggered_frames():
    result = node(
        f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      import {{ createInstancedMirrorSet }} from {json.dumps(CACHE_MODULE.as_uri())};
      const scene = new THREE.Scene();
      const sets = [0, 1, 2, 3].map((phase) => {{
        const parent = new THREE.Group();
        scene.add(parent);
        const geometry = new THREE.BoxGeometry();
        const material = new THREE.MeshBasicMaterial();
        for (let index = 0; index < 3; index += 1) {{
          const mesh = new THREE.Mesh(geometry, material);
          mesh.position.x = index;
          parent.add(mesh);
        }}
        const set = createInstancedMirrorSet(THREE, parent, {{ interval: 4, phase }});
        parent.children.slice().forEach((mesh) => set.add(mesh));
        return {{ parent, set }};
      }});
      scene.updateMatrixWorld(true);
      sets.forEach(({{ set }}) => set.sync());
      const busy = [];
      for (let frame = 0; frame < 8; frame += 1) {{
        sets.forEach(({{ parent }}) => {{ parent.children[0].position.y += 1; }});
        scene.updateMatrixWorld(true);
        busy.push(sets.map(({{ set }}) => set.sync()).filter((written) => written > 0).length);
      }}
      process.stdout.write(JSON.stringify({{ busy }}));
    """
    )
    assert max(result["busy"]) == 1
    assert sum(result["busy"]) == 8


def test_scene_complexity_walk_runs_in_idle_slices():
    diagnostics = SCENE[SCENE.index("  function getDiagnostics("):]
    diagnostics = diagnostics[: diagnostics.index("    const contextInfo")]
    assert "const steps = sceneComplexitySteps();" in diagnostics
    assert "deadline.timeRemaining() > 2" in diagnostics
    assert "window.requestIdleCallback(callback, { timeout: 2000 })" in diagnostics
    assert "function* sceneComplexitySteps()" in SCENE
    assert "if (visited % SCENE_COMPLEXITY_SLICE_OBJECTS === 0) yield visited;" in SCENE
    assert "at - diagnosticsTopElementsAt >= DIAGNOSTICS_TOP_ELEMENTS_MS" in SCENE
    assert "phase: staticMirrorSets.size," in SCENE


def test_the_five_second_presence_tick_renders_peers_only_after_a_prune():
    assert "this.pruneLocalPeers({ onlyIfChanged: true });" in SHELL
    prune = SHELL[SHELL.index("  pruneLocalPeers({ onlyIfChanged = false } = {}) {"):]
    prune = prune[: prune.index("\n  }\n")]
    assert "if (onlyIfChanged && pruned === 0) return;" in prune
    assert "accountBadgeTimeFormat ||= new Intl.DateTimeFormat(" in SHELL
