import { markSharedResource } from "../world-resource-cache.js";

function hashTypedArray(array, seed) {
  const words =
    array.byteLength % 4 === 0
      ? new Uint32Array(array.buffer, array.byteOffset, array.byteLength / 4)
      : new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
  let hash = seed >>> 0;
  for (let index = 0; index < words.length; index += 1) {
    hash = Math.imul(hash ^ words[index], 0x01000193) >>> 0;
  }
  return hash;
}

function sameTypedArray(left, right) {
  if (left === right) return true;
  if (
    left.constructor !== right.constructor ||
    left.length !== right.length
  ) {
    return false;
  }
  for (let index = 0; index < left.length; index += 1) {
    if (
      left[index] !== right[index] &&
      !(Number.isNaN(left[index]) && Number.isNaN(right[index]))
    ) {
      return false;
    }
  }
  return true;
}

function geometryVersion(geometry) {
  let version = geometry.index ? geometry.index.version : 0;
  for (const name in geometry.attributes) {
    version += geometry.attributes[name].version;
  }
  return version;
}

function dedupableGeometry(THREE, geometry) {
  if (
    !geometry?.isBufferGeometry ||
    !geometry.parameters ||
    typeof geometry.parameters !== "object" ||
    !geometry.attributes?.position
  ) {
    return false;
  }
  for (const name in geometry.morphAttributes) {
    if (geometry.morphAttributes[name]?.length) return false;
  }
  const buffers = Object.values(geometry.attributes);
  if (geometry.index) buffers.push(geometry.index);
  return buffers.every(
    (attribute) =>
      attribute.isBufferAttribute === true &&
      attribute.isInstancedBufferAttribute !== true &&
      attribute.usage === THREE.StaticDrawUsage,
  );
}

function geometryContentKey(geometry) {
  const parts = [geometry.type];
  let hash = 0x811c9dc5;
  if (geometry.index) {
    hash = hashTypedArray(geometry.index.array, hash);
    parts.push(`i${geometry.index.array.length}`);
  }
  for (const name of Object.keys(geometry.attributes).sort()) {
    const attribute = geometry.attributes[name];
    hash = hashTypedArray(attribute.array, hash);
    parts.push(
      `${name}:${attribute.itemSize}:${attribute.normalized ? 1 : 0}:${attribute.array.length}`,
    );
  }
  parts.push(
    geometry.groups
      .map((group) => `${group.start}/${group.count}/${group.materialIndex}`)
      .join(","),
    `${geometry.drawRange.start}/${geometry.drawRange.count}`,
    hash.toString(36),
  );
  return parts.join("|");
}

function sameGeometryContent(left, right) {
  if (Boolean(left.index) !== Boolean(right.index)) return false;
  if (left.index && !sameTypedArray(left.index.array, right.index.array)) {
    return false;
  }
  const names = Object.keys(left.attributes);
  if (names.length !== Object.keys(right.attributes).length) return false;
  return names.every((name) => {
    const a = left.attributes[name];
    const b = right.attributes[name];
    return (
      Boolean(b) &&
      a.itemSize === b.itemSize &&
      a.normalized === b.normalized &&
      sameTypedArray(a.array, b.array)
    );
  });
}

export function createGeometryDeduper(THREE, now = () => performance.now()) {
  const records = new WeakMap();
  const settledMeshes = new WeakMap();
  const canonical = new Map();
  let orphans = new Set();
  let sweeps = 0;
  const stats = { replaced: 0, dynamic: 0 };

  const recordOf = (geometry) => {
    let record = records.get(geometry);
    if (!record) {
      record = dedupableGeometry(THREE, geometry)
        ? {
            key: geometryContentKey(geometry),
            version: geometryVersion(geometry),
            firstSweep: sweeps,
            dynamic: false,
          }
        : { key: "", version: 0, firstSweep: sweeps, dynamic: true };
      records.set(geometry, record);
      return record;
    }
    if (!record.dynamic && geometryVersion(geometry) !== record.version) {
      record.dynamic = true;
      stats.dynamic += 1;
      if (canonical.get(record.key) === geometry) canonical.delete(record.key);
    }
    return record;
  };

  const settle = (mesh) => {
    const geometry = mesh.geometry;
    const record = recordOf(geometry);
    if (record.dynamic) {
      settledMeshes.set(mesh, geometry);
      return;
    }
    if (record.firstSweep === sweeps) return;
    const shared = canonical.get(record.key);
    if (!shared) {
      canonical.set(record.key, geometry);
      settledMeshes.set(mesh, geometry);
      return;
    }
    if (shared === geometry) {
      settledMeshes.set(mesh, geometry);
      return;
    }
    const sharedRecord = records.get(shared);
    if (
      !sharedRecord ||
      sharedRecord.dynamic ||
      geometryVersion(shared) !== sharedRecord.version
    ) {
      recordOf(shared);
      return;
    }
    if (!sameGeometryContent(shared, geometry)) {
      settledMeshes.set(mesh, geometry);
      return;
    }
    markSharedResource(shared);
    if (!shared.boundingSphere && geometry.boundingSphere) {
      shared.boundingSphere = geometry.boundingSphere.clone();
    }
    mesh.geometry = shared;
    orphans.add(geometry);
    settledMeshes.set(mesh, shared);
    stats.replaced += 1;
  };

  return {
    stats,
    sweep(root, budgetMs = 4) {
      const startedAt = now();
      sweeps += 1;
      const inUse = new Set();
      const pending = [];
      root.traverse((object) => {
        if (object.isMesh !== true || object.isSkinnedMesh === true) return;
        const geometry = object.geometry;
        if (!geometry) return;
        inUse.add(geometry);
        if (settledMeshes.get(object) !== geometry) pending.push(object);
      });
      let complete = true;
      for (const mesh of pending) {
        if (now() - startedAt > budgetMs) {
          complete = false;
          break;
        }
        settle(mesh);
      }
      const remaining = new Set();
      orphans.forEach((geometry) => {
        if (inUse.has(geometry)) {
          remaining.add(geometry);
        } else if (geometry.userData?.forkmeshSharedResource !== true) {
          geometry.dispose();
        }
      });
      orphans = remaining;
      return complete;
    },
  };
}

export function indexGeometry(THREE, geometry) {
  if (geometry.index) return geometry;
  const names = Object.keys(geometry.attributes);
  const attributes = names.map((name) => geometry.attributes[name]);
  const count = geometry.attributes.position.count;
  const lookup = new Map();
  const remap = new Uint32Array(count);
  const kept = [];
  for (let vertex = 0; vertex < count; vertex += 1) {
    let key = "";
    for (const attribute of attributes) {
      const offset = vertex * attribute.itemSize;
      for (let component = 0; component < attribute.itemSize; component += 1) {
        key += `${Math.round(attribute.array[offset + component] * 1e5)},`;
      }
    }
    let target = lookup.get(key);
    if (target === undefined) {
      target = kept.length;
      kept.push(vertex);
      lookup.set(key, target);
    }
    remap[vertex] = target;
  }
  const indexed = new THREE.BufferGeometry();
  attributes.forEach((attribute, slot) => {
    const size = attribute.itemSize;
    const array = new attribute.array.constructor(kept.length * size);
    kept.forEach((vertex, target) => {
      for (let component = 0; component < size; component += 1) {
        array[target * size + component] = attribute.array[vertex * size + component];
      }
    });
    indexed.setAttribute(
      names[slot],
      new THREE.BufferAttribute(array, size, attribute.normalized),
    );
  });
  indexed.setIndex(
    new THREE.BufferAttribute(
      kept.length > 65535 ? remap : Uint16Array.from(remap),
      1,
    ),
  );
  geometry.groups.forEach((group) => {
    indexed.addGroup(group.start, group.count, group.materialIndex);
  });
  indexed.computeBoundingBox();
  indexed.computeBoundingSphere();
  geometry.dispose();
  return indexed;
}
