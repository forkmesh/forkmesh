#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess

from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
BATCHES_MODULE = WORLD / "scene" / "figure-batches.js"
SETTLED_MODULE = WORLD / "scene" / "settled-matrices.js"
THREE_MODULE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
SCENE = world_scene_source()

FIXTURE = """
  const fakeContext = { setTransform() {}, clearRect() {}, drawImage() {} };
  globalThis.document = {
    createElement: () => ({ width: 0, height: 0, getContext: () => fakeContext }),
  };
  const { createFigureBatches } = await import(BATCHES);
  const { installSettledMatrixUpdate } = await import(SETTLED);
  const copies = [];
  const renderer = {
    domElement: { height: 720 },
    initTexture() {},
    copyTextureToTexture(source, target, region, at) {
      copies.push({ target, layer: at.z });
    },
  };
  const scene = new THREE.Scene();
  installSettledMatrixUpdate(THREE, scene);
  const camera = new THREE.PerspectiveCamera(44, 16 / 9, 0.1, 1000);
  scene.add(camera);
  const batchesGroup = new THREE.Group();
  scene.add(batchesGroup);
  const world = new THREE.Group();
  scene.add(world);
  const box = new THREE.BoxGeometry(1, 1, 1);
  const plane = new THREE.PlaneGeometry(1, 1);
  const head = new THREE.SphereGeometry(0.45, 32, 24);
  const texture = () => {
    const map = new THREE.Texture({ width: 64, height: 64 });
    map.needsUpdate = true;
    return map;
  };
  const batches = createFigureBatches(THREE, {
    parent: batchesGroup,
    renderer,
    camera,
  });
  const avatars = [];
  for (let index = 0; index < 6; index += 1) {
    const avatar = new THREE.Group();
    avatar.position.set(index * 12 - 30, 0, -40);
    const highDetail = new THREE.Group();
    avatar.add(highDetail);
    const shirt = new THREE.MeshStandardMaterial({ map: texture() });
    const mesh = (geometry, material, y) => {
      const part = new THREE.Mesh(geometry, material);
      part.position.y = y;
      highDetail.add(part);
      return part;
    };
    const torso = mesh(box, shirt, 2.15);
    const leftArm = mesh(box, shirt, 2.08);
    const rightArm = mesh(box, shirt, 2.08);
    const headMesh = mesh(head, shirt, 3.36);
    const faceMesh = mesh(plane, new THREE.MeshBasicMaterial({ map: texture() }), 3.36);
    const badge = mesh(plane, new THREE.MeshBasicMaterial({ map: texture() }), 2.3);
    const backName = mesh(plane, new THREE.MeshBasicMaterial({ map: texture(), transparent: true }), 2.94);
    avatar.userData = {
      highDetail, shirt, torso, leftArm, rightArm, head: headMesh, faceMesh, badge, backName,
    };
    world.add(avatar);
    batches.register(avatar);
    avatars.push(avatar);
  }
  const meshes = () => Object.fromEntries(
    batchesGroup.children.map((child) => [child.userData.figureBatch, child]),
  );
  let muted = [];
  const frame = () => {
    scene.updateMatrixWorld();
    camera.updateMatrixWorld();
    batches.sync(camera);
    const hidden = avatars.map((avatar) => avatar.visible === false);
    muted = avatars.map((avatar) => avatar.userData.torso.layers.mask === 0);
    batches.restore();
    return hidden;
  };
  const mismatch = () => {
    let worst = 0;
    const matrix = new THREE.Matrix4();
    for (const [kind, mesh] of Object.entries(meshes())) {
      const node = { torso: "torso", leftArm: "leftArm", rightArm: "rightArm", head: "head", face: "faceMesh", badge: "badge", backName: "backName" }[kind];
      for (let index = 0; index < mesh.count; index += 1) {
        const slot = mesh.instanceColor.array[index * 3];
        const avatar = avatars.find((candidate) => batches.stateOf(candidate) && batches.slotOf(candidate) === slot);
        if (!avatar) return Infinity;
        mesh.getMatrixAt(index, matrix);
        const source = avatar.userData[node].matrixWorld.elements;
        for (let k = 0; k < 16; k += 1) worst = Math.max(worst, Math.abs(matrix.elements[k] - source[k]));
      }
    }
    return worst;
  };
  for (let index = 0; index < 4; index += 1) frame();
"""


def run(body):
    script = f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      const BATCHES = {json.dumps(BATCHES_MODULE.as_uri())};
      const SETTLED = {json.dumps(SETTLED_MODULE.as_uri())};
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


def test_distant_figures_draw_from_the_batches_and_hide_during_render():
    result = run(
        """
      const hidden = frame();
      const counts = Object.fromEntries(Object.entries(meshes()).map(([kind, mesh]) => [kind, mesh.visible ? mesh.count : 0]));
      process.stdout.write(JSON.stringify({
        hidden,
        visibleAfter: avatars.map((avatar) => avatar.visible),
        counts,
        stats: batches.stats(),
        error: mismatch(),
        copies: copies.length,
      }));
    """
    )
    assert result["hidden"] == [True] * 6
    assert result["visibleAfter"] == [True] * 6
    assert result["stats"]["batched"] == 6
    assert result["stats"]["full"] == 0
    assert result["counts"]["torso"] == 6
    assert result["counts"]["leftArm"] == 6
    assert result["counts"]["face"] == 6
    assert result["counts"]["backName"] == 6
    assert result["counts"]["head"] == 6
    assert result["error"] < 1e-5
    assert result["copies"] == 24


def test_a_figure_close_to_the_camera_keeps_its_own_card_and_name_board():
    result = run(
        """
      camera.position.set(avatars[2].position.x, 3, avatars[2].position.z + 3);
      camera.lookAt(avatars[2].position.x, 2, avatars[2].position.z);
      const hidden = frame();
      const restored = avatars[2].userData.torso.layers.mask;
      process.stdout.write(JSON.stringify({
        hidden,
        muted,
        restored,
        state: batches.stateOf(avatars[2]),
        torso: meshes().torso.count,
        badge: meshes().badge.count,
        backName: meshes().backName.count,
        error: mismatch(),
      }));
    """
    )
    assert result["state"] == "near"
    assert result["hidden"][2] is False
    assert result["hidden"].count(True) == 5
    assert result["muted"][2] is True
    assert result["restored"] == 1
    assert result["torso"] == 6
    assert result["badge"] == 5
    assert result["backName"] == 5
    assert result["error"] < 1e-5


def test_far_figures_keep_the_full_head():
    result = run(
        """
      camera.position.set(0, 20, 400);
      camera.lookAt(0, 0, -40);
      frame();
      frame();
      const all = meshes();
      process.stdout.write(JSON.stringify({
        kinds: Object.keys(all).sort(),
        head: all.head.count,
        triangles: all.head.geometry.index.count / 3,
        torso: all.torso.count,
      }));
    """
    )
    assert "headLow" not in result["kinds"]
    assert result["head"] == 6
    assert result["triangles"] == 1472
    assert result["torso"] == 6


def test_moved_detached_and_extra_parts_stay_exact():
    result = run(
        """
      avatars[1].position.x += 0.5;
      frame();
      const moved = mismatch();
      world.remove(avatars[4]);
      frame();
      const detached = { torso: meshes().torso.count, error: mismatch() };
      const extra = new THREE.Mesh(box, new THREE.MeshBasicMaterial());
      avatars[0].add(extra);
      const hidden = frame();
      process.stdout.write(JSON.stringify({
        moved,
        detached,
        extraHidden: hidden[0],
        extraState: batches.stateOf(avatars[0]),
        error: mismatch(),
      }));
    """
    )
    assert result["moved"] < 1e-5
    assert result["detached"]["torso"] == 5
    assert result["detached"]["error"] < 1e-5
    assert result["extraHidden"] is False
    assert result["extraState"] == "full"
    assert result["error"] < 1e-5


def test_a_repainted_texture_is_copied_into_its_layer_again():
    result = run(
        """
      const before = copies.length;
      avatars[3].userData.badge.material.map.needsUpdate = true;
      for (let index = 0; index < 60; index += 1) frame();
      process.stdout.write(JSON.stringify({
        added: copies.slice(before).map((copy) => copy.layer),
        slot: batches.slotOf(avatars[3]),
      }));
    """
    )
    assert result["added"] == [result["slot"]]


def test_scene_wires_the_batches_around_every_render():
    before = SCENE.split(
        "scene.onBeforeRender = (_renderer, _scene, renderCamera) => {", 1
    )[1].split("};", 1)[0]
    assert before.index("figureBatches.restore();") < before.index(
        "avatarPartInstances.sync();"
    )
    assert before.index("avatarPartInstances.sync();") < before.index(
        "figureBatches.sync(renderCamera);"
    )
    after = SCENE.split("scene.onAfterRender = () => {", 1)[1].split("};", 1)[0]
    assert "figureBatches.restore();" in after
    assert SCENE.count("figureBatches.register(figure);") == 1
    assert SCENE.count("figureBatches.register(avatar);") == 1
    assert "figureBatches.release(figure);" in SCENE
    assert "figureBatches.reserve(loungeJobs.length + remotePlayers.size);" in SCENE
    assert "figureBatches: figureBatches.stats()," in SCENE


def test_batched_materials_sample_one_layer_per_instance():
    assert "uniform sampler2DArray figureLayers;" in SCENE
    assert "vFigureLayer = int( instanceColor.r + 0.5 );" in SCENE
    assert "vColor = vec4( 1.0 );" in SCENE
    assert '{ toneMapped: false, alphaTest: 0.5 },' in SCENE
    assert "texture.source.dataReady = false;" in SCENE
    assert "renderer.copyTextureToTexture(previous, texture, null, copyAt.set(0, 0, 0));" in SCENE
