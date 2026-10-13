const SHARED_FLAG = "forkmeshSharedResource";
const GEOMETRY_CACHE_LIMIT = 2048;
const MATERIAL_CACHE_LIMIT = 1024;
const COLOR_PARAMETERS = new Set([
  "color",
  "emissive",
  "specular",
  "specularColor",
  "sheenColor",
  "attenuationColor",
]);

const caches = new WeakMap();

export function sharedResourceCache(THREE, kind) {
  let byKind = caches.get(THREE);
  if (!byKind) {
    byKind = new Map();
    caches.set(THREE, byKind);
  }
  let cache = byKind.get(kind);
  if (!cache) {
    cache = new Map();
    byKind.set(kind, cache);
  }
  return cache;
}

function keepSharedResource() {}

export function markSharedResource(resource) {
  if (!resource || typeof resource !== "object") return resource;
  if (!resource.userData || typeof resource.userData !== "object") {
    resource.userData = {};
  }
  resource.userData[SHARED_FLAG] = true;
  resource.dispose = keepSharedResource;
  return resource;
}

export function isSharedResource(resource) {
  return resource?.userData?.[SHARED_FLAG] === true;
}

function numberKey(value) {
  if (!Number.isFinite(value)) return String(value);
  return String(Math.round(value * 1e6) / 1e6);
}

export function geometryCacheKey(type, params = []) {
  const parts = [];
  for (const value of params) {
    if (typeof value === "number") parts.push(numberKey(value));
    else if (typeof value === "boolean" || value === undefined || value === null) {
      parts.push(String(value));
    } else return "";
  }
  return `${type}(${parts.join(",")})`;
}

export function sharedGeometry(THREE, type, ...params) {
  const Geometry = THREE[type];
  if (typeof Geometry !== "function") {
    throw new TypeError(`Unknown geometry type ${type}`);
  }
  const key = geometryCacheKey(type, params);
  if (!key) return new Geometry(...params);
  const cache = sharedResourceCache(THREE, "geometry");
  const cached = cache.get(key);
  if (cached) return cached;
  const geometry = new Geometry(...params);
  if (cache.size >= GEOMETRY_CACHE_LIMIT) return geometry;
  cache.set(key, markSharedResource(geometry));
  return geometry;
}

export function materialCacheKey(THREE, type, parameters = {}) {
  const parts = [type];
  for (const name of Object.keys(parameters).sort()) {
    const value = parameters[name];
    if (value === undefined) continue;
    let text;
    if (COLOR_PARAMETERS.has(name) && value !== null) {
      text = new THREE.Color(value).getHexString();
    } else if (typeof value === "number") {
      text = numberKey(value);
    } else if (
      typeof value === "boolean" ||
      typeof value === "string" ||
      value === null
    ) {
      text = String(value);
    } else if (value?.isTexture) {
      text = `texture:${value.uuid}`;
    } else {
      return "";
    }
    parts.push(`${name}=${text}`);
  }
  return parts.join("|");
}

export function sharedMaterial(THREE, type, parameters = {}) {
  const Material = THREE[type];
  if (typeof Material !== "function") {
    throw new TypeError(`Unknown material type ${type}`);
  }
  const key = materialCacheKey(THREE, type, parameters);
  if (!key) return new Material(parameters);
  const cache = sharedResourceCache(THREE, "material");
  const cached = cache.get(key);
  if (cached) return cached;
  const material = new Material(parameters);
  if (cache.size >= MATERIAL_CACHE_LIMIT) return material;
  cache.set(key, markSharedResource(material));
  return material;
}

export function sharedResourceCacheSizes(THREE) {
  return {
    geometries: sharedResourceCache(THREE, "geometry").size,
    materials: sharedResourceCache(THREE, "material").size,
  };
}

const mirrorSources = new WeakMap();
const instanceTargetLists = new WeakMap();

export const INSTANCED_SOURCE_LAYER = 30;
let IDENTITY = null;

function sceneRootOf(object) {
  let current = object;
  while (current?.parent) current = current.parent;
  return current;
}

function effectivelyVisibleWithin(object, root, rootScene) {
  let current = object;
  let last = null;
  while (current) {
    if (current === root) return true;
    if (current.visible === false) return false;
    last = current;
    current = current.parent;
  }
  return last === rootScene;
}

function defaultOnBeforeRender(THREE, object) {
  return object.onBeforeRender === THREE.Object3D.prototype.onBeforeRender;
}

export function mirrorableMesh(THREE, object) {
  if (!object?.isMesh || object.isInstancedMesh || object.isSkinnedMesh) return false;
  if (Array.isArray(object.material) || !object.material) return false;
  if (object.material.visible === false) return false;
  if (object.userData?.raycastProxy === true) return false;
  if (object.userData?.instancedMirror === true) return false;
  if (mirrorSources.has(object)) return false;
  if (!object.geometry?.attributes?.position) return false;
  for (const name in object.geometry.morphAttributes || {}) {
    if (object.geometry.morphAttributes[name]?.length) return false;
  }
  if ((object.layers?.mask & 1) !== 1) return false;
  return defaultOnBeforeRender(THREE, object);
}

export function isInstancedMirrorSource(object) {
  return mirrorSources.has(object);
}

export function createInstancedMirrorSet(THREE, parent, {
  name = "forkmesh-instanced-mirror",
  cull = true,
  interval = 1,
  phase = 0,
} = {}) {
  let frame = interval > 1 ? Math.abs(Math.trunc(phase)) % interval : 0;
  let dirty = true;
  const sources = new Set();
  const batches = new Map();
  const inverse = new THREE.Matrix4();
  const relative = new THREE.Matrix4();
  IDENTITY ||= new THREE.Matrix4();
  let batchCount = 0;

  const hide = (source) => {
    if (mirrorSources.has(source)) return;
    mirrorSources.set(source, { mask: source.layers.mask });
    source.layers.set(INSTANCED_SOURCE_LAYER);
    source.userData.instancedMirrorSource = true;
  };

  const reveal = (source) => {
    const record = mirrorSources.get(source);
    if (!record) return;
    source.layers.mask = record.mask;
    mirrorSources.delete(source);
    delete source.userData.instancedMirrorSource;
  };

  const batchFor = (geometry, material) => {
    let byMaterial = batches.get(geometry);
    if (!byMaterial) {
      byMaterial = new Map();
      batches.set(geometry, byMaterial);
    }
    let batch = byMaterial.get(material);
    if (!batch) {
      batch = { geometry, material, mesh: null, capacity: 0, pending: [] };
      byMaterial.set(material, batch);
    }
    return batch;
  };

  const grow = (batch, needed) => {
    if (batch.mesh && batch.capacity >= needed) return;
    const capacity = Math.max(needed, batch.capacity * 2, 4);
    const mesh = new THREE.InstancedMesh(batch.geometry, batch.material, capacity);
    batchCount += 1;
    mesh.name = `${name}-${batchCount}`;
    mesh.count = 0;
    mesh.frustumCulled = cull;
    mesh.raycast = () => {};
    mesh.userData.instancedMirror = true;
    if (batch.mesh) {
      parent.remove(batch.mesh);
      batch.mesh.dispose();
    }
    parent.add(mesh);
    batch.mesh = mesh;
    batch.capacity = capacity;
  };

  function add(source) {
    if (!mirrorableMesh(THREE, source)) return mirrorSources.has(source);
    sources.add(source);
    hide(source);
    dirty = true;
    return true;
  }

  function remove(source) {
    if (!sources.delete(source)) return false;
    reveal(source);
    dirty = true;
    return true;
  }

  const parentSnapshot = new Float64Array(16);
  const visibleMemo = new Map();

  const visibleWithin = (object, rootScene) => {
    const trail = [];
    let current = object;
    let verdict;
    while (true) {
      if (current === parent) { verdict = true; break; }
      if (!current) { verdict = trail[trail.length - 1] === rootScene; break; }
      const known = visibleMemo.get(current);
      if (known !== undefined) { verdict = known; break; }
      if (current.visible === false) { verdict = false; trail.push(current); break; }
      trail.push(current);
      current = current.parent;
    }
    for (let index = 0; index < trail.length; index += 1) {
      const node = trail[index];
      if (node.visible === false) {
        visibleMemo.set(node, false);
      } else {
        visibleMemo.set(node, verdict);
      }
    }
    return verdict;
  };

  let spentMs = 0;
  let syncs = 0;
  function sync(extra = null, options = {}) {
    const startedAt = performance.now();
    const written = syncNow(extra, options);
    spentMs += performance.now() - startedAt;
    syncs += 1;
    return written;
  }

  function syncNow(extra = null, { checked = false, force = false } = {}) {
    frame += 1;
    if (!force && !dirty && interval > 1 && frame % interval !== 0) return 0;
    dirty = false;
    const rootScene = sceneRootOf(parent);
    const atRoot = parent === rootScene;
    if (!atRoot && !effectivelyVisibleWithin(parent, rootScene, rootScene)) return 0;
    const identity = atRoot && parent.matrixWorld.equals(IDENTITY);
    let parentChanged = false;
    if (!identity) {
      const elements = parent.matrixWorld.elements;
      for (let index = 0; index < 16; index += 1) {
        if (parentSnapshot[index] !== elements[index]) {
          parentChanged = true;
          parentSnapshot[index] = elements[index];
        }
      }
      if (parentChanged) inverse.copy(parent.matrixWorld).invert();
    }
    visibleMemo.clear();
    const place = (source, verified) => {
      if (!verified && !visibleWithin(source, rootScene)) return;
      const material = source.material;
      if (!material || Array.isArray(material) || material.visible === false) return;
      batchFor(source.geometry, material).pending.push(source);
    };
    for (const source of sources) place(source, false);
    if (extra) {
      for (const source of extra) {
        if (!source?.isMesh) continue;
        if (!mirrorSources.has(source)) {
          if (!mirrorableMesh(THREE, source)) continue;
          hide(source);
        }
        place(source, checked);
      }
    }
    let written = 0;
    for (const byMaterial of batches.values()) {
      for (const batch of byMaterial.values()) {
        const pending = batch.pending;
        if (!pending.length && !batch.mesh) continue;
        if (pending.length) {
          grow(batch, pending.length);
          batch.mesh.userData.mirrorOwnerSource = pending[0];
        }
        const mesh = batch.mesh;
        const array = mesh.instanceMatrix.array;
        let changed = mesh.count !== pending.length;
        let castShadow = false;
        let receiveShadow = false;
        for (let index = 0; index < pending.length; index += 1) {
          const source = pending[index];
          castShadow ||= source.castShadow === true;
          receiveShadow ||= source.receiveShadow === true;
          let elements;
          if (identity) {
            elements = source.matrixWorld.elements;
          } else {
            const record = mirrorSources.get(source);
            const world = source.matrixWorld.elements;
            let moved = parentChanged || !record.snapshot;
            if (!moved) {
              for (let component = 0; component < 16; component += 1) {
                if (record.snapshot[component] !== world[component]) {
                  moved = true;
                  break;
                }
              }
            }
            if (moved) {
              record.snapshot ||= new Float64Array(16);
              record.relative ||= new Float32Array(16);
              record.snapshot.set(world);
              record.relative.set(relative.multiplyMatrices(inverse, source.matrixWorld).elements);
            }
            elements = record.relative;
          }
          const offset = index * 16;
          for (let component = 0; component < 16; component += 1) {
            const value = Math.fround(elements[component]);
            if (array[offset + component] !== value) {
              array[offset + component] = value;
              changed = true;
            }
          }
        }
        if (pending.length) mesh.renderOrder = pending[0].renderOrder || 0;
        mesh.castShadow = castShadow;
        mesh.receiveShadow = receiveShadow;
        mesh.count = pending.length;
        mesh.visible = pending.length > 0;
        pending.length = 0;
        if (changed) {
          mesh.instanceMatrix.needsUpdate = true;
          if (cull && mesh.count) mesh.computeBoundingSphere();
          written += 1;
        }
      }
    }
    return written;
  }

  function dispose() {
    for (const source of sources) reveal(source);
    sources.clear();
    for (const byMaterial of batches.values()) {
      for (const batch of byMaterial.values()) {
        if (!batch.mesh) continue;
        parent.remove(batch.mesh);
        batch.mesh.dispose();
      }
    }
    batches.clear();
  }

  function stats() {
    let draws = 0;
    let instances = 0;
    for (const byMaterial of batches.values()) {
      for (const batch of byMaterial.values()) {
        if (!batch.mesh?.visible) continue;
        draws += 1;
        instances += batch.mesh.count;
      }
    }
    return {
      draws,
      instances,
      sources: sources.size,
      syncMs: syncs ? Math.round((spentMs / syncs) * 1000) / 1000 : 0,
    };
  }

  const invalidate = () => {
    dirty = true;
  };

  return { add, remove, reveal, sync, dispose, stats, invalidate, parent };
}

export function collectRepeatedMeshes(THREE, root, { minimum = 4, skip = null } = {}) {
  const pairs = new Map();
  const visit = (object) => {
    if (skip && object !== root && skip(object)) return;
    const children = object.children;
    for (let index = 0; index < children.length; index += 1) visit(children[index]);
    if (!mirrorableMesh(THREE, object)) return;
    if (object.material.transparent) return;
    let byMaterial = pairs.get(object.geometry);
    if (!byMaterial) {
      byMaterial = new Map();
      pairs.set(object.geometry, byMaterial);
    }
    let list = byMaterial.get(object.material);
    if (!list) {
      list = [];
      byMaterial.set(object.material, list);
    }
    list.push(object);
  };
  if (root) visit(root);
  const groups = [];
  for (const byMaterial of pairs.values()) {
    for (const list of byMaterial.values()) {
      if (list.length >= minimum) groups.push(list);
    }
  }
  return groups;
}

export function commonAncestor(objects, fallback) {
  if (!objects.length) return fallback;
  const others = objects.slice(1).map((object) => {
    const ancestors = new Set();
    for (let node = object.parent; node; node = node.parent) ancestors.add(node);
    return ancestors;
  });
  for (let node = objects[0].parent; node; node = node.parent) {
    if (others.every((ancestors) => ancestors.has(node))) return node;
  }
  return fallback;
}

export function createInstancedItems(THREE, {
  geometry,
  material,
  items = [],
  name = "forkmesh-instanced-items",
} = {}) {
  const mesh = new THREE.InstancedMesh(geometry, material, Math.max(1, items.length));
  mesh.name = name;
  mesh.count = items.length;
  const targets = [];
  const color = new THREE.Color();
  items.forEach((item, index) => {
    const target = new THREE.Object3D();
    target.name = item.name || `${name}:${index}`;
    if (item.position) target.position.copy(item.position);
    if (item.quaternion) target.quaternion.copy(item.quaternion);
    if (item.rotation) target.rotation.copy(item.rotation);
    if (item.scale) target.scale.copy(item.scale);
    Object.assign(target.userData, item.userData || {});
    target.userData.instanceId = index;
    target.updateMatrix();
    target.parent = mesh;
    mesh.setMatrixAt(index, target.matrix);
    if (item.color !== undefined && item.color !== null) {
      mesh.setColorAt(index, color.set(item.color));
    }
    targets.push(target);
  });
  mesh.instanceMatrix.needsUpdate = true;
  if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
  mesh.computeBoundingSphere();
  mesh.computeBoundingBox?.();
  instanceTargetLists.set(mesh, targets);
  mesh.userData.instancedItems = targets.length;
  return mesh;
}

export function instancedItemTargets(mesh) {
  return instanceTargetLists.get(mesh) || [];
}

export function resolveInstancedHit(hit) {
  const object = hit?.object;
  const targets = object ? instanceTargetLists.get(object) : null;
  if (!object?.isInstancedMesh || !Array.isArray(targets)) return hit;
  const target = targets[hit.instanceId];
  if (!target) return hit;
  return { ...hit, object: target, instancedObject: object };
}

export function resolveInstancedHits(hits) {
  if (!Array.isArray(hits)) return hits;
  for (let index = 0; index < hits.length; index += 1) {
    if (hits[index]?.object?.isInstancedMesh) {
      return hits.map(resolveInstancedHit);
    }
  }
  return hits;
}
