import {
  geometryCacheKey,
  markSharedResource,
  materialCacheKey,
  sharedResourceCache,
} from "../world-resource-cache.js";

export const SURFACE_MATERIAL = "MeshLambertMaterial";
export const HERO_MATERIAL = "MeshStandardMaterial";

const HERO_ROUGHNESS = 0.35;
const HERO_METALNESS = 0.3;
const STANDARD_ONLY = new Set([
  "roughness",
  "metalness",
  "roughnessMap",
  "metalnessMap",
  "envMapIntensity",
  "envMapRotation",
]);
const HERO_KEYS = ["roughnessMap", "metalnessMap", "envMap", "normalMap"];
const GEOMETRY_LIMIT = 4096;
const MATERIAL_LIMIT = 2048;

const authorities = new WeakMap();

function authority(THREE) {
  let state = authorities.get(THREE);
  if (!state) {
    state = {
      geometries: sharedResourceCache(THREE, "geometry"),
      materials: sharedResourceCache(THREE, "material"),
      geometryRequests: 0,
      materialRequests: 0,
      uniqueMaterials: 0,
    };
    authorities.set(THREE, state);
  }
  return state;
}

export function geometry(THREE, key, factory) {
  const state = authority(THREE);
  state.geometryRequests += 1;
  if (!key) return factory();
  const cached = state.geometries.get(key);
  if (cached) return cached;
  const built = factory();
  if (state.geometries.size >= GEOMETRY_LIMIT) return built;
  state.geometries.set(key, markSharedResource(built));
  return built;
}

export function primitive(THREE, type, ...params) {
  const Geometry = THREE[type];
  if (typeof Geometry !== "function" || !type.endsWith("Geometry")) {
    throw new TypeError(`Unknown geometry type ${type}`);
  }
  return geometry(THREE, geometryCacheKey(type, params), () => new Geometry(...params));
}

export function prism(THREE, outline, height) {
  const points = (Array.isArray(outline) ? outline : []).map(([x, z]) => [
    Math.round(Number(x) * 1000) / 1000,
    Math.round(Number(z) * 1000) / 1000,
  ]);
  const depth = Math.round(Number(height) * 1000) / 1000;
  const key = `prism:${depth}:${points.map((point) => point.join(",")).join(";")}`;
  return geometry(THREE, key, () => {
    const shape = new THREE.Shape(
      points.map(([x, z]) => new THREE.Vector2(x, -z)),
    );
    const built = new THREE.ExtrudeGeometry(shape, {
      depth,
      bevelEnabled: false,
      curveSegments: 1,
    });
    built.rotateX(-Math.PI / 2);
    return built;
  });
}

export function surfaceMaterialType(parameters = {}) {
  if (parameters.hero === true) return HERO_MATERIAL;
  if (HERO_KEYS.some((key) => parameters[key])) return HERO_MATERIAL;
  if ((parameters.metalness ?? 0) >= HERO_METALNESS) return HERO_MATERIAL;
  if ((parameters.roughness ?? 1) <= HERO_ROUGHNESS) return HERO_MATERIAL;
  return SURFACE_MATERIAL;
}

function materialParameters(type, parameters = {}) {
  const out = {};
  for (const [name, value] of Object.entries(parameters)) {
    if (name === "hero" || value === undefined) continue;
    if (type === SURFACE_MATERIAL && STANDARD_ONLY.has(name)) continue;
    out[name] = value;
  }
  return out;
}

function construct(THREE, type, parameters) {
  const Material = THREE[type];
  if (typeof Material !== "function" || !type.endsWith("Material")) {
    throw new TypeError(`Unknown material type ${type}`);
  }
  return new Material(materialParameters(type, parameters));
}

export function material(THREE, parameters = {}, type = surfaceMaterialType(parameters)) {
  const state = authority(THREE);
  state.materialRequests += 1;
  const clean = materialParameters(type, parameters);
  const key = materialCacheKey(THREE, type, clean);
  if (!key) return construct(THREE, type, clean);
  const cached = state.materials.get(key);
  if (cached) return cached;
  const built = construct(THREE, type, clean);
  if (state.materials.size >= MATERIAL_LIMIT) return built;
  state.materials.set(key, markSharedResource(built));
  return built;
}

export function sharedMaterialDrift(THREE) {
  const drifted = [];
  for (const [key, shared] of authority(THREE).materials) {
    const [type, ...parts] = key.split("|");
    const current = {};
    for (const part of parts) {
      const name = part.slice(0, part.indexOf("="));
      current[name] = shared[name];
    }
    if (materialCacheKey(THREE, type, current) !== key) drifted.push(key);
  }
  return drifted;
}

export function uniqueMaterial(THREE, parameters = {}, type = surfaceMaterialType(parameters)) {
  const state = authority(THREE);
  state.uniqueMaterials += 1;
  return construct(THREE, type, parameters);
}

export function resourceStats(THREE) {
  const state = authority(THREE);
  return {
    sharedGeometries: state.geometries.size,
    geometryRequests: state.geometryRequests,
    sharedMaterials: state.materials.size,
    materialRequests: state.materialRequests,
    uniqueMaterials: state.uniqueMaterials,
  };
}
