#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
THREE_MODULE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
SCENE = world_scene_source()

FIXTURE = """
  const scene = new THREE.Scene();
  installSettledMatrixUpdate(THREE, scene);
  const culler = createSettledGroupCuller(THREE, scene);
  const world = new THREE.Group();
  scene.add(world);
  const geometry = new THREE.BoxGeometry(1, 1, 1);
  const material = new THREE.MeshBasicMaterial();
  const clusters = [];
  for (let index = 0; index < 12; index += 1) {
    const cluster = new THREE.Group();
    const angle = (index / 12) * Math.PI * 2;
    cluster.position.set(Math.cos(angle) * 40, 0, Math.sin(angle) * 40);
    for (let part = 0; part < 8; part += 1) {
      const holder = new THREE.Group();
      holder.position.set(part % 3, 0.5, Math.floor(part / 3));
      const mesh = new THREE.Mesh(geometry, material);
      mesh.rotation.y = part * 0.3;
      holder.add(mesh);
      cluster.add(holder);
    }
    world.add(cluster);
    clusters.push(cluster);
  }
  const mover = new THREE.Mesh(geometry, material);
  world.add(mover);
  const camera = new THREE.PerspectiveCamera(50, 1, 0.1, 500);
  camera.position.set(0, 2, 0);
  camera.lookAt(0, 2, -10);
  camera.updateMatrixWorld();
  const walk = (count) => {
    for (let index = 0; index < count; index += 1) {
      mover.position.x = index * 0.01;
      scene.updateMatrixWorld();
    }
  };
  const meshes = [];
  scene.traverse((node) => { if (node.isMesh) meshes.push(node); });
  const frustum = new THREE.Frustum();
  const renderer = {
    shadowMap: { enabled: false, autoUpdate: false, needsUpdate: false },
    frames: [],
    render(drawScene, drawCamera) {
      frustum.setFromProjectionMatrix(
        new THREE.Matrix4().multiplyMatrices(drawCamera.projectionMatrix, drawCamera.matrixWorldInverse),
      );
      let hidden = 0;
      let wronglyHidden = 0;
      let drawn = 0;
      for (const mesh of meshes) {
        let shown = true;
        for (let node = mesh; node; node = node.parent) if (node.visible === false) shown = false;
        const inView = frustum.intersectsObject(mesh);
        if (!shown) {
          hidden += 1;
          if (inView) wronglyHidden += 1;
        } else if (inView) {
          drawn += 1;
        }
      }
      this.frames.push({ hidden, wronglyHidden, drawn });
    },
  };
  const frame = () => {
    scene.updateMatrixWorld();
    culler.render(renderer, camera);
    const last = renderer.frames[renderer.frames.length - 1];
    let leftHidden = 0;
    scene.traverse((node) => { if (node.visible === false) leftHidden += 1; });
    return { ...last, leftHidden };
  };
  walk(40);
  culler.refresh();
"""


def run(body):
    script = f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      import {{ installSettledMatrixUpdate, createSettledGroupCuller }} from {json.dumps(SCENE_MODULE.as_uri())};
      {FIXTURE}
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_out_of_view_settled_groups_are_skipped_and_restored():
    result = run(
        """
      const settled = frame();
      process.stdout.write(JSON.stringify({ settled, stats: culler.stats() }));
    """
    )
    assert result["stats"]["groups"] == 12
    assert result["settled"]["hidden"] > 0
    assert result["settled"]["wronglyHidden"] == 0
    assert result["settled"]["drawn"] > 0
    assert result["settled"]["leftHidden"] == 0


def test_a_member_moved_into_view_is_never_culled_with_its_stale_group():
    result = run(
        """
      const hiddenBefore = frame().hidden;
      const behind = clusters.find((cluster) => cluster.position.z > 30);
      const mesh = behind.children[0].children[0];
      mesh.position.set(-behind.position.x, 2, -20 - behind.position.z);
      const moved = frame();
      mesh.updateWorldMatrix(true, false);
      process.stdout.write(JSON.stringify({ hiddenBefore, moved, auto: mesh.matrixAutoUpdate }));
    """
    )
    assert result["hiddenBefore"] > 0
    assert result["moved"]["wronglyHidden"] == 0
    assert result["auto"] is True


def test_a_moved_ancestor_swapped_geometry_or_added_child_disables_the_group():
    result = run(
        """
      const outcomes = {};
      frame();
      world.position.z = -80;
      outcomes.ancestor = frame();
      world.position.z = 0;
      walk(40);
      culler.refresh();
      frame();
      const behind = clusters.find((cluster) => cluster.position.z > 30);
      const mesh = behind.children[1].children[0];
      mesh.geometry = new THREE.BoxGeometry(400, 400, 400);
      outcomes.geometry = frame();
      mesh.geometry = geometry;
      walk(40);
      culler.refresh();
      frame();
      const added = new THREE.Mesh(new THREE.BoxGeometry(400, 400, 400), material);
      behind.add(added);
      meshes.push(added);
      outcomes.added = frame();
      process.stdout.write(JSON.stringify(outcomes));
    """
    )
    for outcome in result.values():
        assert outcome["wronglyHidden"] == 0, result
        assert outcome["leftHidden"] == 0, result


def test_shadow_refresh_frames_render_every_group():
    result = run(
        """
      renderer.shadowMap.enabled = true;
      renderer.shadowMap.needsUpdate = true;
      const shadowFrame = frame();
      renderer.shadowMap.needsUpdate = false;
      const plainFrame = frame();
      process.stdout.write(JSON.stringify({ shadowFrame, plainFrame }));
    """
    )
    assert result["shadowFrame"]["hidden"] == 0
    assert result["plainFrame"]["hidden"] > 0


def test_groups_with_lights_sprites_or_instances_are_never_culled():
    result = run(
        """
      clusters[0].add(new THREE.PointLight());
      clusters[1].add(new THREE.Sprite(new THREE.SpriteMaterial()));
      clusters[2].add(new THREE.InstancedMesh(geometry, material, 4));
      walk(40);
      culler.refresh();
      const eligible = culler.stats().groups;
      process.stdout.write(JSON.stringify({ eligible }));
    """
    )
    assert result["eligible"] == 9


def test_the_main_view_renders_through_the_culler_after_the_matrix_walk():
    assert "const settledCuller = createSettledGroupCuller(THREE, scene);" in SCENE
    walk = SCENE.index("    scene.updateMatrixWorld();\n    settledCuller.refreshIfDue(time);")
    render = SCENE.index("      settledCuller.render(renderer, camera);")
    assert walk < render
    assert "scene.matrixWorldAutoUpdate = false;\n    worldProfiler.beginGpu();" in SCENE


def test_member_circle_figures_are_a_registered_world_element():
    assert '"member-figures",\n            "Member circle figures",' in SCENE
    assert "() => loungeMembers.get(id) === figure," in SCENE
