// Three.js recomposes every object's local matrix and multiplies it into a
// world matrix on every frame, moved or not. Almost all of the World is
// static, and under a 4x CPU throttle that walk was the largest JavaScript
// cost of an idle town frame. The scene root instead remembers each object's
// last position/quaternion/scale and parent, and only redoes the arithmetic
// for objects whose transform (or an ancestor's) actually changed. Hidden
// branches are still walked: picking proxies and ground probes read them.
//
// `matrixWorldNeedsUpdate` is only honoured for objects with manual matrices.
// On auto-updating objects it carries no information the transform does not:
// every getWorldPosition() raises it on each ancestor, so trusting it made the
// whole `world` group look dirty on every frame.
const SETTLED_TRANSFORM = Symbol("forkmesh.settledTransform");
export const SETTLED_PARENT = Symbol("forkmesh.settledParent");
const SETTLED_QUIET = Symbol("forkmesh.settledQuiet");
const SETTLED_FROZEN = Symbol("forkmesh.settledFrozen");
const SETTLED_OBSERVED = Symbol("forkmesh.settledObserved");
export const SETTLED_SUBTREE = Symbol("forkmesh.settledSubtree");
const SETTLED_HOOKED = Symbol("forkmesh.settledHooked");
const OBSERVED_OWNER = Symbol("forkmesh.observedOwner");
const OBSERVED_X = Symbol("forkmesh.observedX");
const OBSERVED_Y = Symbol("forkmesh.observedY");
const OBSERVED_Z = Symbol("forkmesh.observedZ");
const SETTLED_EPOCH = Symbol("forkmesh.settledEpoch");
const SETTLED_REVISION = Symbol("forkmesh.settledRevision");
const SETTLED_QUIET_WALKS = 30;
const SETTLED_REFREEZE_WALKS = 2;
const settledCounters = { epoch: 0, revision: 0 };
export const settledMatrixWalk = {
  walks: 0,
  visited: 0,
  composed: 0,
  frozen: 0,
  thawed: 0,
  isSettled: (object) => object?.[SETTLED_SUBTREE] === true,
  isFrozen: (object) => object?.[SETTLED_FROZEN] === true,
};
function invalidateSettledBranch(object) {
  for (
    let node = object;
    node !== null && node[SETTLED_SUBTREE] === true;
    node = node.parent
  ) {
    node[SETTLED_SUBTREE] = false;
    settledCounters.revision += 1;
    node[SETTLED_REVISION] = settledCounters.revision;
  }
}
function thawSettledObject(object) {
  settledMatrixWalk.thawed += 1;
  object[SETTLED_FROZEN] = false;
  object.matrixAutoUpdate = true;
  invalidateSettledBranch(object);
}
function thawIfRotated(object) {
  if (object[SETTLED_FROZEN] !== true) return;
  const q = object.quaternion;
  const last = object[SETTLED_TRANSFORM];
  if (
    last[3] !== q.x || last[4] !== q.y || last[5] !== q.z || last[6] !== q.w
  ) {
    thawSettledObject(object);
  }
}
function observedAxis(slot) {
  return {
    get() {
      return this[slot];
    },
    set(value) {
      if (this[slot] === value) return;
      this[slot] = value;
      const owner = this[OBSERVED_OWNER];
      if (owner[SETTLED_FROZEN] === true) thawSettledObject(owner);
    },
    enumerable: true,
    configurable: true,
  };
}
const OBSERVED_AXIS_X = observedAxis(OBSERVED_X);
const OBSERVED_AXIS_Y = observedAxis(OBSERVED_Y);
const OBSERVED_AXIS_Z = observedAxis(OBSERVED_Z);
function plainVector(vector) {
  if (!vector || vector.isVector3 !== true) return false;
  const keys = Object.keys(vector);
  return (
    keys.length === 3 && keys[0] === "x" && keys[1] === "y" && keys[2] === "z" &&
    Object.getOwnPropertySymbols(vector).length === 0
  );
}
function observeVector(vector, owner) {
  const x = vector.x;
  const y = vector.y;
  const z = vector.z;
  delete vector.z;
  delete vector.y;
  delete vector.x;
  Object.defineProperty(vector, OBSERVED_OWNER, { value: owner });
  Object.defineProperty(vector, OBSERVED_X, { value: x, writable: true });
  Object.defineProperty(vector, OBSERVED_Y, { value: y, writable: true });
  Object.defineProperty(vector, OBSERVED_Z, { value: z, writable: true });
  Object.defineProperty(vector, "x", OBSERVED_AXIS_X);
  Object.defineProperty(vector, "y", OBSERVED_AXIS_Y);
  Object.defineProperty(vector, "z", OBSERVED_AXIS_Z);
}
function observeSettledObject(object) {
  const quaternion = object.quaternion;
  const rotation = object.rotation;
  if (
    !plainVector(object.position) ||
    !plainVector(object.scale) ||
    typeof quaternion?._onChangeCallback !== "function" ||
    typeof rotation?._onChangeCallback !== "function"
  ) {
    return false;
  }
  observeVector(object.position, object);
  observeVector(object.scale, object);
  const onQuaternionChange = quaternion._onChangeCallback;
  quaternion._onChange(() => {
    onQuaternionChange();
    thawIfRotated(object);
  });
  const onRotationChange = rotation._onChangeCallback;
  rotation._onChange(() => {
    onRotationChange();
    thawIfRotated(object);
  });
  return true;
}
function freezeSettledObject(object) {
  let observed = object[SETTLED_OBSERVED];
  if (observed === undefined) {
    observed = observeSettledObject(object);
    object[SETTLED_OBSERVED] = observed;
  }
  if (observed !== true) return false;
  settledMatrixWalk.frozen += 1;
  object[SETTLED_FROZEN] = true;
  object.matrixAutoUpdate = false;
  settledCounters.epoch += 1;
  object[SETTLED_EPOCH] = settledCounters.epoch;
  return true;
}
function settleMatrixWorld(object, parentChanged) {
  settledMatrixWalk.visited += 1;
  let changed = parentChanged || object[SETTLED_PARENT] !== object.parent;
  let frozen = object[SETTLED_FROZEN] === true;
  if (!frozen && object.matrixAutoUpdate === true) {
    const p = object.position;
    const q = object.quaternion;
    const s = object.scale;
    let last = object[SETTLED_TRANSFORM];
    if (last === undefined) {
      last = new Float64Array(10);
      last[0] = Number.NaN;
      object[SETTLED_TRANSFORM] = last;
      object[SETTLED_QUIET] = 0;
    }
    if (
      last[0] !== p.x || last[1] !== p.y || last[2] !== p.z ||
      last[3] !== q.x || last[4] !== q.y || last[5] !== q.z ||
      last[6] !== q.w || last[7] !== s.x || last[8] !== s.y ||
      last[9] !== s.z
    ) {
      last[0] = p.x; last[1] = p.y; last[2] = p.z;
      last[3] = q.x; last[4] = q.y; last[5] = q.z; last[6] = q.w;
      last[7] = s.x; last[8] = s.y; last[9] = s.z;
      object.matrix.compose(p, q, s);
      settledMatrixWalk.composed += 1;
      object[SETTLED_QUIET] = 0;
      changed = true;
    } else {
      const quiet = object[SETTLED_QUIET] + 1;
      object[SETTLED_QUIET] = quiet;
      if (
        quiet >= (object[SETTLED_OBSERVED] === true
          ? SETTLED_REFREEZE_WALKS
          : SETTLED_QUIET_WALKS) &&
        object.isCamera !== true &&
        object.isSkinnedMesh !== true
      ) {
        frozen = freezeSettledObject(object);
      }
    }
  } else if (!frozen && object.matrixWorldNeedsUpdate === true) {
    changed = true;
  }
  if (changed) {
    if (object.matrixWorldAutoUpdate === true) {
      const parent = object.parent;
      if (parent === null) object.matrixWorld.copy(object.matrix);
      else object.matrixWorld.multiplyMatrices(parent.matrixWorld, object.matrix);
    }
    object.matrixWorldNeedsUpdate = false;
    object[SETTLED_PARENT] = object.parent;
  }
  let settled = frozen;
  const children = object.children;
  for (let index = 0; index < children.length; index += 1) {
    const child = children[index];
    if (child.isCamera === true || child.isSkinnedMesh === true) {
      // These keep derived matrices (a camera's inverse, a skin's bind
      // inverse) in their own override; give them the plain three.js update.
      if (child.matrixWorldAutoUpdate === true || changed) {
        child.updateMatrixWorld(true);
      }
      settled = false;
    } else if (changed) {
      if (!settleMatrixWorld(child, true)) settled = false;
    } else if (
      child.matrixWorldAutoUpdate === true &&
      (child[SETTLED_SUBTREE] !== true || child[SETTLED_PARENT] !== object)
    ) {
      if (!settleMatrixWorld(child, false)) settled = false;
    }
  }
  if (settled !== true && object[SETTLED_SUBTREE] === true) {
    settledCounters.revision += 1;
    object[SETTLED_REVISION] = settledCounters.revision;
  }
  object[SETTLED_SUBTREE] = settled;
  return settled;
}
function installSettledHooks(THREE) {
  const prototype = THREE.Object3D.prototype;
  if (prototype[SETTLED_HOOKED] === true) return;
  prototype[SETTLED_HOOKED] = true;
  const add = prototype.add;
  prototype.add = function addSettled(...objects) {
    const result = add.apply(this, objects);
    invalidateSettledBranch(this);
    return result;
  };
  const remove = prototype.remove;
  prototype.remove = function removeSettled(...objects) {
    for (const object of objects) {
      if (object?.parent === this) object[SETTLED_PARENT] = undefined;
    }
    return remove.apply(this, objects);
  };
  const copy = prototype.copy;
  prototype.copy = function copySettled(source, recursive) {
    const result = copy.call(this, source, recursive);
    if (source?.[SETTLED_FROZEN] === true) this.matrixAutoUpdate = true;
    return result;
  };
}
export const SETTLED_CULL_REFRESH_MS = 1000;
export const SETTLED_CULL_MIN_DRAWABLES = 6;
function settledAncestry(root) {
  const ancestors = [];
  const epochs = [];
  for (let node = root.parent; node !== null; node = node.parent) {
    if (node[SETTLED_FROZEN] !== true) return null;
    ancestors.push(node);
    epochs.push(node[SETTLED_EPOCH]);
  }
  return { ancestors, epochs };
}
function buildSettledCullGroup(THREE, scene, root) {
  const ancestry = settledAncestry(root);
  if (
    ancestry === null ||
    ancestry.ancestors[ancestry.ancestors.length - 1] !== scene
  ) {
    return null;
  }
  const members = [];
  let eligible = true;
  root.traverse((child) => {
    if (!eligible) return;
    if (
      child.isLight === true ||
      child.isCamera === true ||
      child.isSprite === true ||
      child.isSkinnedMesh === true ||
      child.isInstancedMesh === true ||
      child.isBatchedMesh === true ||
      child.boundingSphere !== undefined ||
      child[SETTLED_FROZEN] !== true
    ) {
      eligible = false;
      return;
    }
    if (child.isMesh === true || child.isLine === true || child.isPoints === true) {
      if (child.frustumCulled !== true || !child.geometry) {
        eligible = false;
        return;
      }
      members.push(child);
    }
  });
  if (!eligible || members.length < SETTLED_CULL_MIN_DRAWABLES) return null;
  const sphere = new THREE.Sphere().makeEmpty();
  const scratch = new THREE.Sphere();
  const geometries = [];
  const bounds = [];
  const values = new Float64Array(members.length * 4);
  for (let index = 0; index < members.length; index += 1) {
    const member = members[index];
    const geometry = member.geometry;
    if (geometry.boundingSphere === null) geometry.computeBoundingSphere();
    const local = geometry.boundingSphere;
    if (!local || !Number.isFinite(local.radius)) return null;
    sphere.union(scratch.copy(local).applyMatrix4(member.matrixWorld));
    geometries.push(geometry);
    bounds.push(local);
    values[index * 4] = local.center.x;
    values[index * 4 + 1] = local.center.y;
    values[index * 4 + 2] = local.center.z;
    values[index * 4 + 3] = local.radius;
  }
  if (sphere.isEmpty() || !Number.isFinite(sphere.radius)) return null;
  return {
    root,
    revision: root[SETTLED_REVISION],
    ancestors: ancestry.ancestors,
    epochs: ancestry.epochs,
    members,
    geometries,
    bounds,
    values,
    sphere,
    valid: true,
  };
}
function settledCullGroupIntact(group) {
  const root = group.root;
  if (
    root[SETTLED_SUBTREE] !== true ||
    root[SETTLED_REVISION] !== group.revision ||
    root[SETTLED_PARENT] !== root.parent ||
    root.parent !== group.ancestors[0]
  ) {
    return false;
  }
  const ancestors = group.ancestors;
  const epochs = group.epochs;
  for (let index = 0; index < ancestors.length; index += 1) {
    const ancestor = ancestors[index];
    if (
      ancestor[SETTLED_FROZEN] !== true ||
      ancestor[SETTLED_EPOCH] !== epochs[index]
    ) {
      return false;
    }
  }
  const members = group.members;
  const geometries = group.geometries;
  const bounds = group.bounds;
  const values = group.values;
  for (let index = 0; index < members.length; index += 1) {
    const member = members[index];
    const geometry = geometries[index];
    if (member.geometry !== geometry || member.frustumCulled !== true) return false;
    const local = geometry.boundingSphere;
    const offset = index * 4;
    if (
      local !== bounds[index] ||
      values[offset] !== local.center.x ||
      values[offset + 1] !== local.center.y ||
      values[offset + 2] !== local.center.z ||
      values[offset + 3] !== local.radius
    ) {
      return false;
    }
  }
  return true;
}
function oneSidedPlate(THREE, child) {
  if (child.isMesh !== true || child.isInstancedMesh === true || child.isSkinnedMesh === true) {
    return false;
  }
  const material = child.material;
  const geometry = child.geometry;
  if (
    !material || Array.isArray(material) || !geometry ||
    material.side !== THREE.FrontSide ||
    material.isShaderMaterial === true ||
    material.onBeforeCompile !== THREE.Material.prototype.onBeforeCompile ||
    geometry.type !== "PlaneGeometry" ||
    geometry.morphAttributes?.position
  ) {
    return false;
  }
  const normal = geometry.attributes.normal;
  const position = geometry.attributes.position;
  if (!normal || !position || position.count < 3) return false;
  const z = position.getZ(0);
  for (let index = 0; index < position.count; index += 1) {
    if (
      position.getZ(index) !== z ||
      normal.getX(index) !== 0 || normal.getY(index) !== 0 || normal.getZ(index) !== 1
    ) {
      return false;
    }
  }
  child.userData.settledPlateZ = z;
  return true;
}
function collectSettledPlates(THREE, root, plates) {
  root.traverse((child) => {
    if (child[SETTLED_FROZEN] === true && oneSidedPlate(THREE, child)) plates.push(child);
  });
}
function plateFacesAway(plate, cx, cy, cz) {
  const e = plate.matrixWorld.elements;
  const ax = e[0], ay = e[1], az = e[2];
  const bx = e[4], by = e[5], bz = e[6];
  const nx = ay * bz - az * by;
  const ny = az * bx - ax * bz;
  const nz = ax * by - ay * bx;
  const det = nx * e[8] + ny * e[9] + nz * e[10];
  const z = plate.userData.settledPlateZ || 0;
  const px = e[8] * z + e[12];
  const py = e[9] * z + e[13];
  const pz = e[10] * z + e[14];
  const facing = (cx - px) * nx + (cy - py) * ny + (cz - pz) * nz;
  return det < 0 ? facing > 0 : facing < 0;
}
function collectSettledRoots(object, roots) {
  const children = object.children;
  for (let index = 0; index < children.length; index += 1) {
    const child = children[index];
    if (child[SETTLED_SUBTREE] === true && child[SETTLED_PARENT] === object) {
      roots.push(child);
    } else {
      collectSettledRoots(child, roots);
    }
  }
}
export function createSettledGroupCuller(THREE, scene) {
  const frustum = new THREE.Frustum();
  const projection = new THREE.Matrix4();
  const hidden = [];
  const hiddenPlates = [];
  let plates = [];
  let groups = [];
  let cache = new WeakMap();
  let refreshedAt = -Infinity;
  const stats = {
    refreshes: 0,
    settledRoots: 0,
    groups: 0,
    hiddenGroups: 0,
    hiddenDrawables: 0,
    plates: 0,
    hiddenPlates: 0,
  };
  function refresh() {
    const roots = [];
    collectSettledRoots(scene, roots);
    const next = new WeakMap();
    const built = [];
    const platesBuilt = [];
    for (let index = 0; index < roots.length; index += 1) {
      const root = roots[index];
      let entry = cache.get(root);
      if (
        entry === undefined ||
        entry.revision !== root[SETTLED_REVISION] ||
        (entry.group !== null &&
          (entry.group.valid !== true || !settledCullGroupIntact(entry.group)))
      ) {
        const rootPlates = [];
        collectSettledPlates(THREE, root, rootPlates);
        entry = {
          revision: root[SETTLED_REVISION],
          group: buildSettledCullGroup(THREE, scene, root),
          plates: rootPlates,
        };
      }
      next.set(root, entry);
      if (entry.group !== null) built.push(entry.group);
      for (let plate = 0; plate < entry.plates.length; plate += 1) {
        platesBuilt.push(entry.plates[plate]);
      }
    }
    cache = next;
    groups = built;
    plates = platesBuilt;
    stats.plates = plates.length;
    stats.refreshes += 1;
    stats.settledRoots = roots.length;
    stats.groups = built.length;
    return stats;
  }
  function refreshIfDue(at) {
    if (at - refreshedAt < SETTLED_CULL_REFRESH_MS) return false;
    refreshedAt = at;
    refresh();
    return true;
  }
  function hideOutsideView(camera) {
    stats.hiddenGroups = 0;
    stats.hiddenDrawables = 0;
    if (groups.length === 0) return 0;
    if (camera.parent === null && camera.matrixWorldAutoUpdate === true) {
      camera.updateMatrixWorld();
    }
    projection.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
    frustum.setFromProjectionMatrix(
      projection,
      camera.coordinateSystem,
      camera.reversedDepth,
    );
    for (let index = 0; index < groups.length; index += 1) {
      const group = groups[index];
      if (group.valid !== true || group.root.visible !== true) continue;
      if (frustum.intersectsSphere(group.sphere)) continue;
      if (!settledCullGroupIntact(group)) {
        group.valid = false;
        continue;
      }
      group.root.visible = false;
      hidden.push(group);
      stats.hiddenDrawables += group.members.length;
    }
    stats.hiddenGroups = hidden.length;
    return hidden.length;
  }
  function hideBackFacingPlates(camera) {
    stats.hiddenPlates = 0;
    if (plates.length === 0 || camera.isPerspectiveCamera !== true) return 0;
    const e = camera.matrixWorld.elements;
    const cx = e[12];
    const cy = e[13];
    const cz = e[14];
    for (let index = 0; index < plates.length; index += 1) {
      const plate = plates[index];
      if (plate.visible !== true || plate[SETTLED_FROZEN] !== true) continue;
      if (!plateFacesAway(plate, cx, cy, cz)) continue;
      plate.visible = false;
      hiddenPlates.push(plate);
    }
    stats.hiddenPlates = hiddenPlates.length;
    return hiddenPlates.length;
  }
  function restore() {
    for (let index = 0; index < hidden.length; index += 1) {
      hidden[index].root.visible = true;
    }
    hidden.length = 0;
    for (let index = 0; index < hiddenPlates.length; index += 1) {
      hiddenPlates[index].visible = true;
    }
    hiddenPlates.length = 0;
  }
  function render(renderer, camera) {
    const shadowPass =
      renderer.shadowMap?.enabled === true &&
      (renderer.shadowMap.autoUpdate === true ||
        renderer.shadowMap.needsUpdate === true);
    if (!shadowPass) {
      hideOutsideView(camera);
      hideBackFacingPlates(camera);
    }
    try {
      renderer.render(scene, camera);
    } finally {
      restore();
    }
  }
  return {
    refresh,
    refreshIfDue,
    hideOutsideView,
    hideBackFacingPlates,
    restore,
    render,
    stats: () => ({ ...stats }),
  };
}
export function installSettledMatrixUpdate(THREE, scene) {
  installSettledHooks(THREE);
  scene.userData.settledMatrixWalk = settledMatrixWalk;
  const baseUpdate = THREE.Object3D.prototype.updateMatrixWorld;
  scene.updateMatrixWorld = function updateSceneMatrixWorld(force) {
    // An explicit force keeps its full three.js meaning.
    if (force === true) {
      baseUpdate.call(this, true);
      return;
    }
    settledMatrixWalk.walks += 1;
    settleMatrixWorld(this, false);
  };
}
