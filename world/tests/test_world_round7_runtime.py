import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
THREE_MODULE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
SETTLED_MODULE = WORLD / "scene" / "settled-matrices.js"
OBJECT_UTILS = WORLD / "scene" / "object-utils.js"
SCENE = world_scene_source()
NODE = shutil.which("node")


def run_node(script):
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_back_facing_one_sided_plates_skip_the_main_pass():
    result = run_node(f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      import {{ createSettledGroupCuller, installSettledMatrixUpdate }} from {json.dumps(SETTLED_MODULE.as_uri())};
      const scene = new THREE.Scene();
      installSettledMatrixUpdate(THREE, scene);
      const culler = createSettledGroupCuller(THREE, scene);
      const holder = new THREE.Group();
      scene.add(holder);
      const plate = (name, setup, options = {{}}) => {{
        const mesh = new THREE.Mesh(
          new THREE.PlaneGeometry(1, 1),
          new THREE.MeshBasicMaterial(options),
        );
        mesh.name = name;
        setup(mesh);
        holder.add(mesh);
        return mesh;
      }};
      plate("front", () => {{}});
      plate("back", (mesh) => {{ mesh.rotation.y = Math.PI; }});
      plate("mirrored-back", (mesh) => {{ mesh.scale.z = -1; mesh.rotation.y = Math.PI; }});
      plate("mirrored-x", (mesh) => {{ mesh.scale.x = -1; }});
      plate("double", (mesh) => {{ mesh.rotation.y = Math.PI; }}, {{ side: THREE.DoubleSide }});
      plate("floor", (mesh) => {{ mesh.rotation.x = -Math.PI / 2; mesh.position.y = -1; }});
      plate("raised-floor", (mesh) => {{ mesh.rotation.x = -Math.PI / 2; mesh.position.y = 1; }});
      const camera = new THREE.PerspectiveCamera(50, 1, 0.1, 100);
      camera.position.set(0, 0, 10);
      camera.updateMatrixWorld();
      for (let walk = 0; walk < 40; walk += 1) scene.updateMatrixWorld();
      culler.refresh();
      const hidden = culler.hideBackFacingPlates(camera);
      const names = holder.children.filter((mesh) => !mesh.visible).map((mesh) => mesh.name).sort();
      culler.restore();
      console.log(JSON.stringify({{
        hidden,
        names,
        plates: culler.stats().plates,
        restored: holder.children.every((mesh) => mesh.visible),
      }}));
    """)
    assert result["names"] == ["back", "raised-floor"]
    assert result["hidden"] == 2
    assert result["plates"] == 6
    assert result["restored"] is True


def test_shadow_frames_keep_every_plate():
    source = SETTLED_MODULE.read_text(encoding="utf-8")
    render = source.split("function render(renderer, camera) {", 1)[1]
    assert "if (!shadowPass) {" in render
    assert "hideBackFacingPlates(camera);" in render


def test_transparent_and_hidden_meshes_cast_no_shadow():
    result = run_node(f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      import {{ setShadows }} from {json.dumps(OBJECT_UTILS.as_uri())};
      const group = new THREE.Group();
      const add = (options) => {{
        const mesh = new THREE.Mesh(new THREE.BoxGeometry(), new THREE.MeshStandardMaterial(options));
        group.add(mesh);
        return mesh;
      }};
      const solid = add({{}});
      const glass = add({{ transparent: true, opacity: 0.4 }});
      const proxy = add({{ visible: false }});
      setShadows(group);
      console.log(JSON.stringify([solid.castShadow, glass.castShadow, proxy.castShadow, glass.receiveShadow]));
    """)
    assert result == [True, False, False, True]


def test_campfire_is_four_merged_draws_driven_by_uniforms():
    assert '"campfire-flame-layers"' in SCENE
    assert '"campfire-smoke-puffs"' in SCENE
    assert '"campfire-glowing-coals"' in SCENE
    assert '"campfire-charred-logs"' in SCENE
    assert "proceduralFire.userData.smokeMaterial.uniforms.uTime.value = seconds;" in SCENE
    assert "burningLogs.userData.coalUniforms.uCoalClock.value" in SCENE
    assert "campfire-smoke-puff-${" not in SCENE


def test_office_build_stamp_repaints_in_place_only_when_shown():
    assert "repaintCanvasTexture(buildStamp.material" in SCENE
    assert "if (!objectShown(buildStamp)) return false;" in SCENE
    assert "OFFICE_BUILD_STAMP_HIDDEN_RETRY_MS" in SCENE


def test_resumed_first_view_passes_skip_the_shadow_map():
    assert "warmFirstView({ budgetMs: FIRST_VIEW_WARM_SLICE_MS, resume: true, shadows: false });" in SCENE
