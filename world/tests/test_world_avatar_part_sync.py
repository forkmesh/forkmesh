#!/usr/bin/env python3

import json
from pathlib import Path
import subprocess


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
THREE_MODULE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"

FIXTURE = """
  const scene = new THREE.Scene();
  installSettledMatrixUpdate(THREE, scene);
  const parts = createAvatarPartInstances(THREE, scene);
  const world = new THREE.Group();
  scene.add(world);
  const avatars = [];
  const pinGeometry = new THREE.PlaneGeometry(0.2, 0.2);
  const pinMaterial = new THREE.MeshBasicMaterial();
  for (let index = 0; index < 5; index += 1) {
    const avatar = new THREE.Group();
    avatar.position.set(index * 3, 0, 0);
    const kinds = { leg: 4, shoe: 2, antennaStalk: 1, antennaBulb: 1 };
    const instancedParts = {};
    for (const [kind, count] of Object.entries(kinds)) {
      instancedParts[kind] = [];
      for (let slot = 0; slot < count; slot += 1) {
        const rig = new THREE.Group();
        rig.position.set(slot * 0.2, kind === "leg" ? 0.5 : 1, 0);
        avatar.add(rig);
        instancedParts[kind].push(rig);
      }
    }
    instancedParts.antennaBulb[0].userData.partColor = "#ff0000";
    const pin = new THREE.Mesh(pinGeometry, pinMaterial);
    pin.position.set(0, 1.6, 0.3);
    avatar.add(pin);
    avatar.userData.verifiedPin = pin;
    avatar.userData.instancedParts = instancedParts;
    world.add(avatar);
    parts.register(avatar);
    avatars.push(avatar);
  }
  const batchMeshes = () => {
    const meshes = {};
    scene.children.forEach((child) => {
      const match = /^forkmesh-avatar-(.+)-instances$/.exec(child.name || "");
      if (match) meshes[match[1]] = child;
    });
    return meshes;
  };
  const frame = () => {
    scene.updateMatrixWorld();
    parts.sync();
  };
  let writes = 0;
  const setMatrixAt = THREE.InstancedMesh.prototype.setMatrixAt;
  THREE.InstancedMesh.prototype.setMatrixAt = function (...args) {
    writes += 1;
    return setMatrixAt.apply(this, args);
  };
  let colorWrites = 0;
  const setColorAt = THREE.InstancedMesh.prototype.setColorAt;
  THREE.InstancedMesh.prototype.setColorAt = function (...args) {
    colorWrites += 1;
    return setColorAt.apply(this, args);
  };
  const mismatch = () => {
    let worst = 0;
    const meshes = batchMeshes();
    for (const [kind, mesh] of Object.entries(meshes)) {
      let slot = 0;
      const matrix = new THREE.Matrix4();
      for (const avatar of avatars) {
        let shown = true;
        for (let node = avatar; node; node = node.parent) if (node.visible === false) shown = false;
        if (!shown) continue;
        for (const node of avatar.userData.instancedParts[kind]) {
          if (node.visible === false) continue;
          mesh.getMatrixAt(slot, matrix);
          for (let k = 0; k < 16; k += 1) {
            worst = Math.max(worst, Math.abs(matrix.elements[k] - node.matrixWorld.elements[k]));
          }
          slot += 1;
        }
      }
      if (slot !== mesh.count) worst = Infinity;
    }
    return worst;
  };
  for (let index = 0; index < 40; index += 1) frame();
"""


def run(body):
    script = f"""
      import * as THREE from {json.dumps(THREE_MODULE.as_uri())};
      import {{ installSettledMatrixUpdate, createAvatarPartInstances }} from {json.dumps(SCENE_MODULE.as_uri())};
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


def test_a_settled_crowd_writes_and_uploads_nothing():
    result = run(
        """
      const versions = Object.values(batchMeshes()).map((mesh) => mesh.instanceMatrix.version);
      writes = 0;
      colorWrites = 0;
      frame();
      frame();
      const after = Object.values(batchMeshes()).map((mesh) => mesh.instanceMatrix.version);
      process.stdout.write(JSON.stringify({ writes, colorWrites, versions, after, error: mismatch() }));
    """
    )
    assert result["writes"] == 0
    assert result["colorWrites"] == 0
    assert result["versions"] == result["after"]
    assert result["error"] < 1e-5


def test_only_a_moved_avatar_is_rewritten_and_uploaded():
    result = run(
        """
      writes = 0;
      avatars[2].position.x += 5;
      frame();
      const moved = writes;
      const legs = batchMeshes().leg.instanceMatrix;
      const ranges = legs.updateRanges.map((range) => [range.start, range.count]);
      const error = mismatch();
      for (let index = 0; index < 5; index += 1) frame();
      writes = 0;
      avatars[3].userData.instancedParts.leg[1].rotation.x = 0.4;
      frame();
      const limb = writes;
      const limbError = mismatch();
      process.stdout.write(JSON.stringify({ moved, ranges, error, limb, limbError }));
    """
    )
    assert result["moved"] == 8
    assert result["ranges"] == [[8 * 16, 4 * 16]]
    assert result["error"] < 1e-5
    assert result["limb"] == 8
    assert result["limbError"] < 1e-5


def test_hidden_parts_and_colour_changes_stay_exact():
    result = run(
        """
      avatars[1].userData.instancedParts.antennaStalk[0].visible = false;
      avatars[0].visible = false;
      frame();
      const hiddenError = mismatch();
      colorWrites = 0;
      avatars[4].userData.instancedParts.antennaBulb[0].userData.partColor = "#00ff00";
      frame();
      const colours = colorWrites;
      const bulbs = batchMeshes().antennaBulb;
      const colour = new THREE.Color();
      bulbs.getColorAt(bulbs.count - 1, colour);
      avatars[0].visible = true;
      avatars[1].userData.instancedParts.antennaStalk[0].visible = true;
      frame();
      process.stdout.write(JSON.stringify({ hiddenError, colours, green: colour.getHexString(), restored: mismatch() }));
    """
    )
    assert result["hiddenError"] < 1e-5
    assert result["colours"] == 1
    assert result["green"] == "00ff00"
    assert result["restored"] < 1e-5


def test_avatar_details_follow_moves_and_rest_when_the_crowd_is_still():
    result = run(
        """
      const detailMesh = () => scene.children.find((child) => /^forkmesh-avatar-detail-instances/.test(child.name || ""));
      const settledVersion = detailMesh().instanceMatrix.version;
      frame();
      frame();
      const stillVersion = detailMesh().instanceMatrix.version;
      avatars[1].position.z += 4;
      frame();
      const pin = avatars[1].userData.verifiedPin;
      const matrix = new THREE.Matrix4();
      let error = Infinity;
      for (let slot = 0; slot < detailMesh().count; slot += 1) {
        detailMesh().getMatrixAt(slot, matrix);
        let worst = 0;
        for (let k = 0; k < 16; k += 1) worst = Math.max(worst, Math.abs(matrix.elements[k] - pin.matrixWorld.elements[k]));
        error = Math.min(error, worst);
      }
      process.stdout.write(JSON.stringify({ settledVersion, stillVersion, count: detailMesh().count, error }));
    """
    )
    assert result["settledVersion"] == result["stillVersion"]
    assert result["count"] == 5
    assert result["error"] < 1e-5
