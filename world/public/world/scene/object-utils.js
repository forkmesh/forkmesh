// Triangle weight of a subtree, memoized because the level-of-detail callers
// run inside the animation loop. Instanced draws count every copy.
const objectTriangleWeights = new WeakMap();
export function triangleWeight(object) {
  if (!object) return 0;
  const cached = objectTriangleWeights.get(object);
  if (cached !== undefined) return cached;
  const weight = countTriangles(object);
  objectTriangleWeights.set(object, weight);
  return weight;
}

export function countTriangles(object) {
  if (!object) return 0;
  let total = 0;
  object.traverse((child) => {
    const geometry = child.isMesh ? child.geometry : null;
    const position = geometry?.attributes?.position;
    if (!position) return;
    const vertices = geometry.index ? geometry.index.count : position.count;
    total += (vertices / 3) * (child.isInstancedMesh ? child.count : 1);
  });
  return Math.round(total);
}

export function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

export function disposeOwnedGeometry(geometry) {
  if (!geometry || geometry.userData?.forkmeshSharedResource === true) {
    return false;
  }
  geometry?.dispose?.();
  return true;
}

export function disposeOwnedMaterial(material, disposeMap = false) {
  if (material?.userData?.forkmeshSharedResource === true) return;
  if (disposeMap) material?.map?.dispose?.();
  material?.dispose?.();
}

export function castsOpaqueShadow(material) {
  const list = Array.isArray(material) ? material : [material];
  return list.some((entry) =>
    entry && entry.visible !== false && entry.transparent !== true);
}

export function setShadows(object, cast = true, receive = true) {
  object.traverse((child) => {
    if (!child.isMesh) return;
    child.castShadow = cast && castsOpaqueShadow(child.material);
    child.receiveShadow = receive;
  });
}

export function disposeObject3D(object) {
  const geometries = new Set();
  const materials = new Set();
  const textures = new Set();
  object?.traverse?.((child) => {
    if (child.geometry) geometries.add(child.geometry);
    const childMaterials = Array.isArray(child.material)
      ? child.material
      : [child.material];
    childMaterials.filter(Boolean).forEach((material) => {
      if (material.userData?.forkmeshSharedResource === true) return;
      materials.add(material);
      if (material.map) textures.add(material.map);
    });
  });
  textures.forEach((texture) => texture.dispose?.());
  materials.forEach((material) => disposeOwnedMaterial(material));
  geometries.forEach(disposeOwnedGeometry);
}

export function removeInteractiveObject(interactive, object) {
  if (!object) return;
  object.traverse?.((child) => {
    let index = interactive.indexOf(child);
    while (index >= 0) {
      interactive.splice(index, 1);
      index = interactive.indexOf(child);
    }
  });
}

export function removeGeneratedLayer(parent, layer, interactive) {
  if (!parent || !layer) return;
  removeInteractiveObject(interactive, layer);
  parent.remove(layer);
  disposeObject3D(layer);
}

export function objectIsEffectivelyVisible(object) {
  const invisibleRaycastProxy = object?.userData?.raycastProxy === true;
  let current = object;
  while (current) {
    if (
      current.visible === false &&
      !(current === object && invisibleRaycastProxy)
    ) {
      return false;
    }
    // A subtree an administrator pulled out of the game via the Elements
    // panel has no path up to the Scene; treat it exactly like a hidden one
    // so its meshes cannot be clicked while they are not being drawn.
    if (!current.parent && !current.isScene) return false;
    current = current.parent;
  }
  return true;
}
