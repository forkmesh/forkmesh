import * as resources from "./resources.js";

export function makeMaterial(THREE, color, options = {}) {
  return resources.uniqueMaterial(
    THREE,
    standardMaterialParameters(THREE, color, options),
    "MeshStandardMaterial",
  );
}

export function makeSharedMaterial(THREE, color, options = {}) {
  return resources.material(
    THREE,
    standardMaterialParameters(THREE, color, options),
    "MeshStandardMaterial",
  );
}

export function standardMaterialParameters(THREE, color, options = {}) {
  const parameters = {
    color,
    roughness: options.roughness ?? 0.68,
    metalness: options.metalness ?? 0.08,
    emissive: options.emissive || 0x000000,
    emissiveIntensity: options.emissiveIntensity ?? 0,
    transparent: Boolean(options.transparent),
    opacity: options.opacity ?? 1,
  };
  // Three.js warns for explicitly supplied `undefined` enum values. Omit the
  // option entirely unless a caller intentionally selected a rendering side.
  if (options.side !== undefined) parameters.side = options.side;
  // three r152+ draws every transparent double-sided material in two passes
  // (back faces, then front faces) and sets material.needsUpdate before each
  // pass. That version bump re-resolves the shader program and re-uploads the
  // material's entire uniform block on every draw of every frame — profiled
  // at multiple GB/minute of allocation churn plus a doubled draw call for
  // each such mesh. Every World use is a flat card, open shell, or additive
  // glow that reads identically single-pass, so opt back into the pre-r152
  // single-pass path. (Direct `new THREE.*Material` sites set the same flag.)
  if (parameters.transparent && parameters.side === THREE.DoubleSide) {
    parameters.forceSinglePass = true;
  }
  if (options.depthWrite !== undefined) {
    parameters.depthWrite = options.depthWrite;
  }
  if (options.map) parameters.map = options.map;
  if (options.hero === true) parameters.hero = true;
  return parameters;
}

// Repeated people, placards, vehicles, and props clone one immutable mesh
// prototype per geometry key. Clones keep independent transforms/materials
// while sharing the expensive vertex/index buffers. Shared buffers live for
// the scene lifetime; removing one remote avatar must not dispose a buffer
// still used by every other avatar.
const sharedMeshPrototypeCaches = new WeakMap();

export function cloneSharedMesh(THREE, key, geometryFactory, material) {
  let cache = sharedMeshPrototypeCaches.get(THREE);
  if (!cache) {
    cache = new Map();
    sharedMeshPrototypeCaches.set(THREE, cache);
  }
  let prototype = cache.get(key);
  if (!prototype) {
    const geometry = geometryFactory();
    geometry.userData.forkmeshSharedResource = true;
    prototype = new THREE.Mesh(geometry, null);
    prototype.userData.forkmeshSharedMeshKey = key;
    cache.set(key, prototype);
  }
  const mesh = prototype.clone(false);
  mesh.material = material;
  mesh.userData.forkmeshSharedMeshKey = key;
  return mesh;
}

// One material behind every invisible hit proxy in the World. It is shared, so
// a departing avatar cannot dispose the copy the rest of the crowd is using,
// and it is never drawn — only raycast against.
const sharedHitProxyMaterials = new WeakMap();
export function avatarHitProxyMaterial(THREE) {
  let material = sharedHitProxyMaterials.get(THREE);
  if (!material) {
    material = resources.uniqueMaterial(THREE, { visible: false }, "MeshBasicMaterial");
    material.userData.forkmeshSharedResource = true;
    sharedHitProxyMaterials.set(THREE, material);
  }
  return material;
}

export function cloneSharedPlane(THREE, width, height, material) {
  return cloneSharedMesh(
    THREE,
    `plane:${width}:${height}`,
    () => resources.primitive(THREE, "PlaneGeometry", width, height),
    material,
  );
}
