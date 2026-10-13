import { createInstancedMirrorSet } from "../world-resource-cache.js";
import {
  ANTENNA_STALK_COLOR,
  AVATAR_LEG_SEGMENT,
  AVATAR_SHOE_HEIGHT,
} from "./avatar-shared.js";
import { makeMaterial } from "./materials.js";
import { objectIsEffectivelyVisible } from "./object-utils.js";
import { SETTLED_PARENT, SETTLED_SUBTREE } from "./settled-matrices.js";
import * as resources from "./resources.js";

// Everyone in the World wears the same untextured legs, shoes and shoulder
// antenna in the same materials — only their transforms differ — yet drawing
// them per person cost eight of the fifteen submissions each avatar spent. A
// full Members Circle plus live peers is dozens of people, so the crowd's
// lower halves alone were hundreds of draw calls. These parts are drawn as
// four instanced batches for the whole crowd instead.
//
// The rigs stay real Object3D nodes inside each avatar, so walk cycles, seated
// poses, the antenna blink and per-person scale still animate exactly as
// before; one sync pass per render copies their world matrices into the
// batches. The batches sit at the Scene root with no transform of their own,
// because the matrices copied into them are already world-space.
const AVATAR_INSTANCED_PART_KINDS = [
  "leg",
  "shoe",
  "antennaStalk",
  "antennaBulb",
];
// Every avatar contributes at most this many copies of each part: two thighs
// and two shins, one shoe per foot, one antenna stalk and one bulb.
const AVATAR_INSTANCED_PART_SLOTS = {
  leg: 4,
  shoe: 2,
  antennaStalk: 1,
  antennaBulb: 1,
};
export const avatarInstancedPartGeometry = {
  leg: (THREE) => new THREE.BoxGeometry(0.42, AVATAR_LEG_SEGMENT, 0.45),
  shoe: (THREE) => new THREE.BoxGeometry(0.46, AVATAR_SHOE_HEIGHT, 0.7),
  antennaStalk: (THREE) => new THREE.CylinderGeometry(0.014, 0.018, 0.4, 10),
  antennaBulb: (THREE) => new THREE.SphereGeometry(0.045, 14, 12),
};

export function createAvatarPartInstances(THREE, scene) {
  const avatars = new Set();
  const details = createInstancedMirrorSet(THREE, scene, {
    name: "forkmesh-avatar-detail-instances",
    cull: true,
  });
  const detailSources = [];
  let lastDetailSources = [];
  let lastDetailState = [];
  const detailsUnchanged = () => {
    if (lastDetailSources.length !== detailSources.length) return false;
    for (let index = 0; index < detailSources.length; index += 1) {
      const source = detailSources[index];
      const state = lastDetailState[index];
      if (
        lastDetailSources[index] !== source ||
        state.material !== source.material ||
        state.geometry !== source.geometry ||
        state.materialVisible !== source.material?.visible ||
        state.castShadow !== source.castShadow ||
        state.receiveShadow !== source.receiveShadow
      ) {
        return false;
      }
    }
    return true;
  };
  const rememberDetails = () => {
    lastDetailSources = detailSources.slice();
    lastDetailState = detailSources.map((source) => ({
      material: source.material,
      geometry: source.geometry,
      materialVisible: source.material?.visible,
      castShadow: source.castShadow,
      receiveShadow: source.receiveShadow,
    }));
  };
  const partColor = new THREE.Color();
  const batches = new Map();
  const define = (kind, material, colored = false) => {
    const geometry = avatarInstancedPartGeometry[kind](THREE);
    // One buffer and one material for the whole crowd: a departing avatar must
    // never dispose either, so both are marked shared.
    geometry.userData.forkmeshSharedResource = true;
    material.userData.forkmeshSharedResource = true;
    batches.set(kind, {
      kind,
      geometry,
      material,
      colored,
      mesh: null,
      capacity: 0,
      count: 0,
      nodes: [],
      colors: [],
      rewrite: true,
      matrixFrom: Infinity,
      matrixTo: -1,
      colorFrom: Infinity,
      colorTo: -1,
    });
  };
  const avatarMatrices = new WeakMap();
  const avatarStill = (avatar) => {
    const elements = avatar.matrixWorld.elements;
    let last = avatarMatrices.get(avatar);
    let same = last !== undefined;
    if (last === undefined) {
      last = new Float64Array(16);
      avatarMatrices.set(avatar, last);
    }
    for (let index = 0; index < 16; index += 1) {
      if (last[index] !== elements[index]) {
        same = false;
        last[index] = elements[index];
      }
    }
    return (
      same &&
      avatar[SETTLED_SUBTREE] === true &&
      avatar[SETTLED_PARENT] === avatar.parent
    );
  };
  define("leg", makeMaterial(THREE, "#101d19", { roughness: 0.85 }));
  define("shoe", makeMaterial(THREE, "#07100e", { roughness: 0.82 }));
  define(
    "antennaStalk",
    resources.uniqueMaterial(THREE, { color: ANTENNA_STALK_COLOR }, "MeshBasicMaterial"),
  );
  // White, so the blink colour written per instance survives the multiply.
  define("antennaBulb", resources.material(THREE, { color: "#ffffff" }, "MeshBasicMaterial"), true);

  function grow(batch, slots) {
    if (batch.capacity >= slots) return;
    const capacity = Math.max(slots, batch.capacity * 2, 64);
    const mesh = new THREE.InstancedMesh(
      batch.geometry,
      batch.material,
      capacity,
    );
    mesh.name = `forkmesh-avatar-${batch.kind}-instances`;
    mesh.castShadow = true;
    mesh.receiveShadow = true;
    mesh.count = 0;
    mesh.instanceMatrix.setUsage?.(THREE.DynamicDrawUsage);
    // A shared batch belongs to no one avatar and carries no behaviour of its
    // own, so it is not a pick target: clicks on someone's legs land on that
    // avatar's own invisible leg proxy instead.
    mesh.raycast = () => {};
    if (batch.mesh) {
      scene.remove(batch.mesh);
      // Releases the instance buffers only; the geometry and material above
      // are shared and outlive every batch built from them.
      batch.mesh.dispose();
    }
    scene.add(mesh);
    batch.mesh = mesh;
    batch.capacity = capacity;
    batch.nodes.length = 0;
    batch.colors.length = 0;
    batch.rewrite = true;
  }

  // A part is drawn only when nothing between it and its own avatar is hidden —
  // the antenna is switched off by hiding its group, and a seated avatar's legs
  // ride rigs that stay visible. The avatar's own path to the Scene is checked
  // once per avatar instead of once per part.
  function branchVisible(node, root) {
    let current = node;
    while (current && current !== root) {
      if (current.visible === false) return false;
      current = current.parent;
    }
    return current === root;
  }

  function sync() {
    // Capacity is claimed up front: growing a batch part-way through the crowd
    // would abandon the matrices already written into the smaller buffer.
    for (const kind of AVATAR_INSTANCED_PART_KINDS) {
      const batch = batches.get(kind);
      batch.count = 0;
      grow(batch, avatars.size * AVATAR_INSTANCED_PART_SLOTS[kind]);
    }
    let crowdStill = true;
    for (const avatar of avatars) {
      if (!avatar.parent) {
        // Avatars are only ever detached for good — a peer who left, a pruned
        // directory figure — so a lost parent is the release signal. Districts
        // and the Office interior hide their contents instead of removing them.
        avatars.delete(avatar);
        continue;
      }
      const parts = avatar.userData.instancedParts;
      if (!parts || !objectIsEffectivelyVisible(avatar)) {
        avatarMatrices.delete(avatar);
        continue;
      }
      const still = avatarStill(avatar);
      if (!still) crowdStill = false;
      const pin = avatar.userData.verifiedPin;
      const ring = avatar.userData.activityRing;
      const patches = avatar.userData.achievementBadges?.children;
      if (pin && branchVisible(pin, avatar)) detailSources.push(pin);
      if (ring && branchVisible(ring, avatar)) detailSources.push(ring);
      if (patches) {
        for (const patch of patches) {
          if (branchVisible(patch, avatar)) detailSources.push(patch);
        }
      }
      const belt = avatar.userData.operatorBeltLights?.[0]?.parent;
      if (belt && branchVisible(belt, avatar)) {
        for (const part of belt.children) detailSources.push(part);
      }
      for (const kind of AVATAR_INSTANCED_PART_KINDS) {
        const batch = batches.get(kind);
        for (const node of parts[kind]) {
          if (batch.count >= batch.capacity) break;
          if (!branchVisible(node, avatar)) continue;
          const slot = batch.count;
          if (batch.rewrite || !still || batch.nodes[slot] !== node) {
            batch.mesh.setMatrixAt(slot, node.matrixWorld);
            batch.nodes[slot] = node;
            if (slot < batch.matrixFrom) batch.matrixFrom = slot;
            if (slot > batch.matrixTo) batch.matrixTo = slot;
          }
          if (batch.colored) {
            const color = node.userData.partColor || "#ffffff";
            if (batch.rewrite || batch.colors[slot] !== color) {
              batch.mesh.setColorAt(slot, partColor.set(color));
              batch.colors[slot] = color;
              if (slot < batch.colorFrom) batch.colorFrom = slot;
              if (slot > batch.colorTo) batch.colorTo = slot;
            }
          }
          batch.count += 1;
        }
      }
    }
    if (!crowdStill || !detailsUnchanged()) {
      details.sync(detailSources, { checked: true });
      rememberDetails();
    }
    detailSources.length = 0;
    for (const batch of batches.values()) {
      if (!batch.mesh) continue;
      const recount = batch.mesh.count !== batch.count;
      batch.mesh.count = batch.count;
      batch.nodes.length = batch.count;
      batch.colors.length = Math.min(batch.colors.length, batch.count);
      if (batch.matrixTo >= batch.matrixFrom) {
        const matrices = batch.mesh.instanceMatrix;
        matrices.clearUpdateRanges();
        matrices.addUpdateRange(
          batch.matrixFrom * 16,
          (batch.matrixTo - batch.matrixFrom + 1) * 16,
        );
        matrices.needsUpdate = true;
      }
      if (recount || batch.matrixTo >= batch.matrixFrom) {
        batch.mesh.computeBoundingSphere();
      }
      const colors = batch.mesh.instanceColor;
      if (colors && batch.colorTo >= batch.colorFrom) {
        colors.clearUpdateRanges();
        colors.addUpdateRange(
          batch.colorFrom * 3,
          (batch.colorTo - batch.colorFrom + 1) * 3,
        );
        colors.needsUpdate = true;
      }
      batch.rewrite = false;
      batch.matrixFrom = Infinity;
      batch.matrixTo = -1;
      batch.colorFrom = Infinity;
      batch.colorTo = -1;
    }
  }

  return {
    register(avatar) {
      if (avatar?.userData?.instancedParts) avatars.add(avatar);
    },
    release(avatar) {
      avatars.delete(avatar);
    },
    sync,
    // Read by the diagnostics panel and the browser contract: how many people
    // and how many copies of each part the crowd is drawing in four calls.
    stats() {
      const drawn = {};
      for (const kind of AVATAR_INSTANCED_PART_KINDS) {
        drawn[kind] = batches.get(kind).count;
      }
      return { avatars: avatars.size, drawn, details: details.stats() };
    },
  };
}

export function avatarDetailParent(avatar) {
  return avatar?.userData?.highDetail || avatar;
}
