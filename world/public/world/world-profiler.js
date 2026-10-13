export const WORLD_PROFILE_PHASES = [
  { id: "housekeeping", label: "Housekeeping: environment, LOD, badges" },
  { id: "frame-animations", label: "Per-frame animation callbacks" },
  { id: "simulation", label: "Walking, rides, districts, Office" },
  { id: "avatars", label: "Remote avatars, Forkbot, agent bots" },
  { id: "effects", label: "Portals, beacons, one-shot effects" },
  { id: "camera", label: "Camera, sky, weather, store elements" },
  { id: "proximity", label: "Proximity scans and doors" },
  { id: "mirror", label: "Office logo mirror capture" },
  { id: "ambient", label: "Ambient animation callbacks" },
  { id: "labels", label: "DOM name labels" },
  { id: "matrices", label: "Scene matrix update" },
  { id: "render", label: "renderer.render: cull, sort, shadows, submit" },
];

const PHASE_IDS = WORLD_PROFILE_PHASES.map((phase) => phase.id);
const FRAME_BUDGET_MS = 1000 / 60;
export const WORLD_PROFILE_MIN_WINDOW_MS = 1000;
export const WORLD_PROFILE_MIN_FRAMES = 30;

function zeroPhases() {
  const phases = {};
  for (const id of PHASE_IDS) phases[id] = 0;
  return phases;
}

export function describeWorldCallback(callback) {
  const name = typeof callback?.name === "string" ? callback.name : "";
  let source = "";
  try {
    source = Function.prototype.toString.call(callback);
  } catch {
    source = "";
  }
  const lines = source
    .replace(/^[^{]*\{/, "")
    .replace(/\}\s*$/, "")
    .split("\n")
    .map((line) => line.trim())
    .filter((line) => line && !/^[{}()[\];,]*$/.test(line));
  const informative =
    lines.find(
      (line) =>
        !/^(if|for|while|else|switch|try|return)\b/.test(line) &&
        (/[\w\]]\.[\w.[\]]+\s*[-+*/]?=(?!=)/.test(line) ||
          /\w\.\w+\(/.test(line)),
    ) || lines[0];
  const statement = (informative || source.replace(/\s+/g, " ")).slice(0, 96);
  if (name && !/^(anonymous|callback)$/.test(name)) {
    return `${name}() — ${statement}`;
  }
  return statement || "anonymous callback";
}

export function createGpuTimer(renderer) {
  let gl = null;
  let ext = null;
  try {
    gl = renderer?.getContext?.() || null;
    const webgl2 =
      typeof WebGL2RenderingContext !== "undefined" &&
      gl instanceof WebGL2RenderingContext;
    ext = webgl2 ? gl.getExtension("EXT_disjoint_timer_query_webgl2") : null;
  } catch {
    ext = null;
  }
  const pending = [];
  let active = null;
  let totalMs = 0;
  let peakMs = 0;
  let samples = 0;
  let disjoint = 0;
  const reset = () => {
    for (const query of pending) {
      try {
        gl.deleteQuery(query);
      } catch {
        break;
      }
    }
    pending.length = 0;
    active = null;
  };
  return {
    get available() {
      return Boolean(ext);
    },
    begin() {
      if (!ext || active || pending.length >= 4) return;
      if (gl.isContextLost?.()) return reset();
      const query = gl.createQuery();
      if (!query) return;
      gl.beginQuery(ext.TIME_ELAPSED_EXT, query);
      active = query;
    },
    end() {
      if (!active) return;
      gl.endQuery(ext.TIME_ELAPSED_EXT);
      pending.push(active);
      active = null;
    },
    poll() {
      if (!ext || !pending.length) return;
      if (gl.isContextLost?.()) return reset();
      while (pending.length) {
        const query = pending[0];
        if (!gl.getQueryParameter(query, gl.QUERY_RESULT_AVAILABLE)) break;
        if (gl.getParameter(ext.GPU_DISJOINT_EXT)) {
          disjoint += 1;
        } else {
          const ms = gl.getQueryParameter(query, gl.QUERY_RESULT) / 1e6;
          totalMs += ms;
          peakMs = Math.max(peakMs, ms);
          samples += 1;
        }
        gl.deleteQuery(query);
        pending.shift();
      }
    },
    take() {
      const reading = {
        available: Boolean(ext),
        avgMs: samples ? totalMs / samples : NaN,
        peakMs: samples ? peakMs : NaN,
        samples,
        disjoint,
      };
      totalMs = 0;
      peakMs = 0;
      samples = 0;
      disjoint = 0;
      return reading;
    },
    dispose: reset,
  };
}

export function createWorldProfiler({
  renderer,
  camera,
  ownerOf = () => "",
  now = () => performance.now(),
} = {}) {
  let enabled = false;
  let frames = 0;
  let windowStartedAt = now();
  let markAt = 0;
  let frame = zeroPhases();
  let totals = zeroPhases();
  let peaks = zeroPhases();
  let callbacks = new Map();
  let owners = new Map();
  let ownerCache = new WeakMap();
  let ownerCacheAt = 0;
  const passes = { main: 0, shadow: 0, other: 0, mainTriangles: 0 };
  const labels = new WeakMap();
  const callbackOwners = new WeakMap();
  const ownerOrdinals = new Map();
  let pendingCallbacks = [];
  let lastReading = null;
  const gpu = createGpuTimer(renderer);

  const resetWindow = () => {
    frames = 0;
    totals = zeroPhases();
    peaks = zeroPhases();
    callbacks = new Map();
    owners = new Map();
    passes.main = 0;
    passes.shadow = 0;
    passes.other = 0;
    passes.mainTriangles = 0;
    windowStartedAt = now();
  };

  const callbackLabel = (callback) => {
    let label = labels.get(callback);
    if (label === undefined) {
      const owner = callbackOwners.get(callback);
      label = owner || describeWorldCallback(callback);
      labels.set(callback, label);
    }
    return label;
  };

  const ownerLabel = (object) => {
    let owner = ownerCache.get(object);
    if (owner !== undefined) return owner;
    owner = ownerOf(object) || "";
    if (!owner) {
      let node = object;
      while (node && !node.name) node = node.parent;
      owner = node?.name
        ? `Unregistered · ${String(node.name).slice(0, 48)}`
        : "Unregistered objects";
    }
    ownerCache.set(object, owner);
    return owner;
  };

  return {
    get enabled() {
      return enabled;
    },
    get gpuAvailable() {
      return gpu.available;
    },
    watchCallbacks(list) {
      if (!Array.isArray(list) || list.push !== Array.prototype.push) return list;
      list.push = function watchedCallbackPush(...items) {
        for (const item of items) {
          if (typeof item === "function" && pendingCallbacks.length < 512) pendingCallbacks.push(item);
        }
        return Array.prototype.push.apply(this, items);
      };
      return list;
    },
    claimCallbacks(owner) {
      const label = String(owner || "").trim();
      if (!label || !pendingCallbacks.length) return 0;
      const claimed = pendingCallbacks;
      pendingCallbacks = [];
      for (const callback of claimed) {
        const ordinal = (ownerOrdinals.get(label) || 0) + 1;
        ownerOrdinals.set(label, ordinal);
        callbackOwners.set(callback, claimed.length > 1 || ordinal > 1 ? `${label} #${ordinal}` : label);
        labels.delete(callback);
      }
      return claimed.length;
    },
    setEnabled(value) {
      const next = value === true;
      if (next === enabled) return;
      enabled = next;
      if (!enabled) gpu.dispose();
      lastReading = null;
      resetWindow();
    },
    beginFrame() {
      if (!enabled) return;
      frame = zeroPhases();
      markAt = now();
    },
    mark(phase) {
      if (!enabled) return;
      const at = now();
      frame[phase] += at - markAt;
      markAt = at;
    },
    endFrame() {
      if (!enabled) return;
      frames += 1;
      for (const id of PHASE_IDS) {
        totals[id] += frame[id];
        if (frame[id] > peaks[id]) peaks[id] = frame[id];
      }
    },
    runCallbacks(list, time, delta) {
      if (!enabled) {
        list.forEach((callback) => callback(time, delta));
        return;
      }
      for (let index = 0; index < list.length; index += 1) {
        const callback = list[index];
        const startedAt = now();
        callback(time, delta);
        const ms = now() - startedAt;
        let entry = callbacks.get(callback);
        if (!entry) {
          entry = { ms: 0, calls: 0, peakMs: 0 };
          callbacks.set(callback, entry);
        }
        entry.ms += ms;
        entry.calls += 1;
        if (ms > entry.peakMs) entry.peakMs = ms;
      }
    },
    noteDraw(drawCamera, drawScene, geometry, material, object, group) {
      if (!enabled) return;
      if (drawScene === null) {
        passes.shadow += 1;
        return;
      }
      if (drawCamera !== camera) {
        passes.other += 1;
        return;
      }
      passes.main += 1;
      const owner = ownerLabel(object);
      let entry = owners.get(owner);
      if (!entry) {
        entry = { draws: 0, triangles: 0, transparent: 0 };
        owners.set(owner, entry);
      }
      entry.draws += 1;
      if (material?.transparent) entry.transparent += 1;
      if (object?.isMesh) {
        const vertices = group
          ? group.count
          : geometry?.index
            ? geometry.index.count
            : geometry?.attributes?.position?.count || 0;
        const instances = object.isInstancedMesh
          ? object.count
          : geometry?.isInstancedBufferGeometry
            ? geometry.instanceCount
            : 1;
        const triangles = Math.floor((Number(vertices) || 0) / 3) *
          Math.max(0, Number(instances) || 0);
        entry.triangles += triangles;
        passes.mainTriangles += triangles;
      }
    },
    beginGpu() {
      if (enabled) gpu.begin();
    },
    endGpu() {
      if (enabled) gpu.end();
    },
    sample() {
      gpu.poll();
      const at = now();
      const elapsedMs = Math.max(1, at - windowStartedAt);
      if (
        enabled &&
        lastReading &&
        (elapsedMs < WORLD_PROFILE_MIN_WINDOW_MS || frames < WORLD_PROFILE_MIN_FRAMES)
      ) {
        return lastReading;
      }
      const frameCount = Math.max(1, frames);
      if (at - ownerCacheAt >= 5000) {
        ownerCacheAt = at;
        ownerCache = new WeakMap();
      }
      const phases = WORLD_PROFILE_PHASES.map((phase) => ({
        id: phase.id,
        label: phase.label,
        avgMs: totals[phase.id] / frameCount,
        peakMs: peaks[phase.id],
      }));
      const cpuMs = phases.reduce((sum, phase) => sum + phase.avgMs, 0);
      const topCallbacks = [...callbacks.entries()]
        .map(([callback, entry]) => ({
          label: callbackLabel(callback),
          msPerSecond: (entry.ms * 1000) / elapsedMs,
          avgMs: entry.calls ? entry.ms / entry.calls : 0,
          peakMs: entry.peakMs,
          calls: entry.calls,
        }))
        .sort((a, b) => b.msPerSecond - a.msPerSecond)
        .slice(0, 12);
      const draws = [...owners.entries()]
        .map(([label, entry]) => ({
          label,
          draws: entry.draws / frameCount,
          triangles: entry.triangles / frameCount,
          transparent: entry.transparent / frameCount,
        }))
        .sort((a, b) => b.draws - a.draws)
        .slice(0, 16);
      const reading = {
        enabled,
        frames,
        elapsedMs,
        budgetMs: FRAME_BUDGET_MS,
        cpuMs,
        phases,
        callbacks: topCallbacks,
        callbackCount: callbacks.size,
        draws,
        passes: {
          main: passes.main / frameCount,
          shadow: passes.shadow / frameCount,
          other: passes.other / frameCount,
          mainTriangles: passes.mainTriangles / frameCount,
        },
        gpu: gpu.take(),
      };
      resetWindow();
      if (enabled && reading.frames > 0) lastReading = reading;
      return reading;
    },
    dispose() {
      lastReading = null;
      enabled = false;
      gpu.dispose();
    },
  };
}

const TEXTURE_SLOTS = [
  "map",
  "normalMap",
  "roughnessMap",
  "metalnessMap",
  "aoMap",
  "emissiveMap",
  "bumpMap",
  "alphaMap",
  "envMap",
  "lightMap",
  "displacementMap",
  "specularMap",
  "matcap",
  "gradientMap",
];

const MATERIAL_COST = {
  MeshPhysicalMaterial: 5,
  MeshStandardMaterial: 4,
  MeshPhongMaterial: 3,
  MeshToonMaterial: 3,
  MeshLambertMaterial: 2,
  MeshMatcapMaterial: 1,
  MeshBasicMaterial: 1,
};

const TESSELLATED_TYPES = new Set([
  "SphereGeometry",
  "CylinderGeometry",
  "ConeGeometry",
  "TorusGeometry",
  "TorusKnotGeometry",
  "CircleGeometry",
  "CapsuleGeometry",
  "RingGeometry",
  "LatheGeometry",
]);

const TESSELLATED_SMALL_RADIUS = 6;

function roundParam(value) {
  return typeof value === "number" ? Math.round(value * 1000) / 1000 : value;
}

function geometrySignature(geometry) {
  const position = geometry.attributes?.position;
  if (geometry.parameters && typeof geometry.parameters === "object") {
    const params = Object.values(geometry.parameters)
      .filter((value) => typeof value !== "object" || value === null)
      .map(roundParam);
    return `${geometry.type}(${params.join(", ")})`;
  }
  if (!position?.array || position.count < 3) return "";
  const array = position.array;
  const probe = [0, 1, 2, 3, 4, 5, array.length - 3, array.length - 2, array.length - 1]
    .map((index) => roundParam(array[index]))
    .join(",");
  return `${geometry.type}[${position.count}v/${geometry.index?.count || 0}i:${probe}]`;
}

function hashWords(array, seed) {
  if (!array?.buffer) return seed;
  const words =
    array.byteLength % 4 === 0
      ? new Uint32Array(array.buffer, array.byteOffset, array.byteLength / 4)
      : new Uint8Array(array.buffer, array.byteOffset, array.byteLength);
  let hash = seed;
  for (let index = 0; index < words.length; index += 1) {
    hash = Math.imul(hash ^ words[index], 0x01000193) >>> 0;
  }
  return hash;
}

function geometryContentHash(geometry, cache) {
  const attributes = geometry.attributes || {};
  let version = Number(geometry.index?.version) || 0;
  for (const name in attributes) version += Number(attributes[name]?.version) || 0;
  const cached = cache.get(geometry);
  if (cached && cached.version === version) return cached.hash;
  let hash = hashWords(geometry.index?.array, 0x811c9dc5);
  for (const name of Object.keys(attributes).sort()) {
    hash = hashWords(attributes[name]?.array, Math.imul(hash ^ name.length, 0x01000193) >>> 0);
  }
  const text = hash.toString(36);
  cache.set(geometry, { version, hash: text });
  return text;
}

function neverDrawn(material) {
  if (Array.isArray(material)) {
    return material.length > 0 && material.every((entry) => entry?.visible === false);
  }
  return material?.visible === false;
}

function colorHex(color) {
  return color?.isColor ? color.getHexString() : "";
}

function materialSignature(material) {
  if (material.isShaderMaterial || material.isRawShaderMaterial) return "";
  return [
    material.type,
    colorHex(material.color),
    colorHex(material.emissive),
    roundParam(material.emissiveIntensity),
    roundParam(material.opacity),
    material.transparent,
    material.side,
    material.map?.uuid || "",
    material.alphaMap?.uuid || "",
    material.normalMap?.uuid || "",
    material.emissiveMap?.uuid || "",
    material.envMap?.uuid || "",
    roundParam(material.roughness),
    roundParam(material.metalness),
    material.vertexColors,
    material.wireframe,
    material.depthWrite,
    material.depthTest,
    material.blending,
    material.flatShading,
    material.fog,
    material.toneMapped,
    roundParam(material.alphaTest),
    material.polygonOffset,
    roundParam(material.polygonOffsetFactor),
    roundParam(material.polygonOffsetUnits),
    material.colorWrite,
    material.forceSinglePass,
    roundParam(material.envMapIntensity),
    roundParam(material.clearcoat),
    roundParam(material.clearcoatRoughness),
    roundParam(material.size),
    material.sizeAttenuation,
    material.onBeforeCompile ? String(material.customProgramCacheKey?.() || "custom") : "",
    material.userData?.ringPhase ?? "",
  ].join("|");
}

function matrixSignature(matrix) {
  const e = matrix.elements;
  return (
    e[0] + e[1] * 3 + e[2] * 5 + e[4] * 7 + e[5] * 11 + e[6] * 13 +
    e[8] * 17 + e[9] * 19 + e[10] * 23 + e[12] * 29 + e[13] * 31 + e[14] * 37
  );
}

function isPowerOfTwo(value) {
  return value > 0 && (value & (value - 1)) === 0;
}

function textureSize(texture) {
  const image = texture?.isCubeTexture ? texture.image?.[0] : texture?.image;
  return {
    width: Math.max(0, Number(image?.width) || 0),
    height: Math.max(0, Number(image?.height) || 0),
  };
}

function compact(value) {
  const count = Math.max(0, Number(value) || 0);
  if (count >= 1_000_000) return `${(count / 1_000_000).toFixed(1)}m`;
  if (count >= 10_000) return `${Math.round(count / 1_000)}k`;
  if (count >= 1_000) return `${(count / 1_000).toFixed(1)}k`;
  return `${Math.round(count)}`;
}

function topCounts(map, limit = 6, format = (key, count) => `${key} × ${count}`) {
  return [...map.entries()]
    .sort((a, b) => b[1] - a[1])
    .slice(0, limit)
    .map(([key, count]) => format(key, count));
}

function bump(map, key, amount = 1) {
  map.set(key, (map.get(key) || 0) + amount);
}

export function createWorldAuditState() {
  return {
    at: 0,
    audits: 0,
    matrices: new WeakMap(),
    textureVersions: new WeakMap(),
    attributeVersions: new WeakMap(),
    geometryHashes: new WeakMap(),
    leakHistory: [],
    walk: null,
  };
}

export function auditWorldScene({
  scene,
  renderer,
  ownerOf = () => "",
  state = createWorldAuditState(),
  profile = null,
  quality = null,
  output = null,
  now = () => performance.now(),
} = {}) {
  const startedAt = now();
  const comparable = state.audits > 0;
  const elapsedSeconds = comparable ? Math.max(0.001, (startedAt - state.at) / 1000) : 0;
  const ownerCache = new Map();
  const owner = (object) => {
    let label = ownerCache.get(object);
    if (label === undefined) {
      label = ownerOf(object) || "Unregistered objects";
      ownerCache.set(object, label);
    }
    return label;
  };

  const stats = {
    objects: 0,
    hiddenObjects: 0,
    autoMatrixObjects: 0,
    staticAutoMatrixObjects: 0,
    unsettledObjects: 0,
    walkVisitedPerWalk: 0,
    walkComposedPerWalk: 0,
    walkThawsPerSecond: 0,
    drawables: 0,
    visibleDrawables: 0,
    instancedMeshes: 0,
    sprites: 0,
    shadowCasters: 0,
    smallShadowCasters: 0,
    shadowReceivers: 0,
    cullingOff: 0,
    transparentDrawables: 0,
    needlessTransparent: 0,
    invisibleDrawn: 0,
    activePointLights: 0,
    activeSpotLights: 0,
    shadowPointLights: 0,
    sceneGeometries: 0,
    sceneMaterials: 0,
    sceneTextures: 0,
    textureUploads: 0,
    bufferUploadBytes: 0,
    materialCost: {},
  };

  const geometries = new Map();
  const materials = new Map();
  const textures = new Map();
  const pairs = new Map();
  const cullingOffByOwner = new Map();
  const needlessTransparentByOwner = new Map();
  const invisibleByOwner = new Map();
  const smallCastersByOwner = new Map();
  const bufferUploadsByOwner = new Map();
  const hiddenByOwner = new Map();
  const staticByOwner = new Map();
  const unsettledByOwner = new Map();
  const walk = scene?.userData?.settledMatrixWalk || null;
  const spritesByOwner = new Map();
  const materialCostByType = new Map();

  const visit = (object, parentVisible) => {
    stats.objects += 1;
    const visible = parentVisible && object.visible !== false;
    if (!visible) {
      stats.hiddenObjects += 1;
      if (parentVisible) bump(hiddenByOwner, owner(object));
    }
    if (walk && !walk.isSettled(object) && object.matrixWorldAutoUpdate !== false) {
      stats.unsettledObjects += 1;
      bump(unsettledByOwner, owner(object));
    }
    if (object.matrixAutoUpdate === true && !object.isCamera) {
      stats.autoMatrixObjects += 1;
      const signature = matrixSignature(object.matrixWorld);
      const previous = state.matrices.get(object);
      if (comparable && previous === signature) {
        stats.staticAutoMatrixObjects += 1;
        bump(staticByOwner, owner(object));
      }
      state.matrices.set(object, signature);
    }
    if (object.isLight && visible) {
      if (object.isPointLight) {
        stats.activePointLights += 1;
        if (object.castShadow) stats.shadowPointLights += 1;
      }
      if (object.isSpotLight) stats.activeSpotLights += 1;
    }
    const drawable =
      (object.isMesh || object.isPoints || object.isLine || object.isSprite) &&
      object.userData?.raycastProxy !== true &&
      (object.layers?.mask === undefined || (object.layers.mask & 1) === 1) &&
      !neverDrawn(object.material);
    if (drawable) {
      stats.drawables += 1;
      const geometry = object.geometry;
      const objectMaterials = Array.isArray(object.material)
        ? object.material.filter(Boolean)
        : object.material
          ? [object.material]
          : [];
      if (geometry) {
        let record = geometries.get(geometry);
        if (!record) {
          const vertices =
            geometry.index?.count ?? geometry.attributes?.position?.count ?? 0;
          record = {
            uses: 0,
            visibleUses: 0,
            triangles: object.isMesh ? Math.floor(vertices / 3) : 0,
            owner: owner(object),
            radius: 0,
          };
          geometries.set(geometry, record);
        }
        record.uses += 1;
        if (visible) record.visibleUses += 1;
        if (visible && object.isMesh && !record.radius) {
          if (!geometry.boundingSphere) geometry.computeBoundingSphere?.();
          const scale = object.matrixWorld.getMaxScaleOnAxis?.() || 1;
          record.radius = (geometry.boundingSphere?.radius || 0) * scale;
        }
        let version = 0;
        for (const attribute of Object.values(geometry.attributes || {})) {
          version += Number(attribute?.version) || 0;
        }
        const previousVersion = state.attributeVersions.get(geometry);
        if (comparable && previousVersion !== undefined && version > previousVersion) {
          let bytes = 0;
          for (const attribute of Object.values(geometry.attributes || {})) {
            bytes += attribute?.array?.byteLength || 0;
          }
          stats.bufferUploadBytes += bytes * (version - previousVersion);
          bump(
            bufferUploadsByOwner,
            `${owner(object)} · ${object.name || object.parent?.name || geometry.type || "geometry"}`,
            bytes * (version - previousVersion),
          );
        }
        state.attributeVersions.set(geometry, version);
      }
      for (const material of objectMaterials) {
        let record = materials.get(material);
        if (!record) {
          record = { uses: 0, owner: owner(object) };
          materials.set(material, record);
          for (const slot of TEXTURE_SLOTS) {
            const texture = material[slot];
            if (!texture?.isTexture) continue;
            if (!textures.has(texture)) {
              textures.set(texture, { owner: owner(object), slot });
            }
          }
        }
        record.uses += 1;
      }
      if (visible) {
        stats.visibleDrawables += 1;
        if (object.isInstancedMesh) stats.instancedMeshes += 1;
        if (object.isSprite) {
          stats.sprites += 1;
          bump(spritesByOwner, owner(object));
        }
        if (object.frustumCulled === false) {
          stats.cullingOff += 1;
          bump(cullingOffByOwner, owner(object));
        }
        if (object.isMesh && object.castShadow) {
          stats.shadowCasters += 1;
          const record = geometries.get(geometry);
          if (record && record.radius > 0 && record.radius < 0.35) {
            stats.smallShadowCasters += 1;
            bump(smallCastersByOwner, owner(object));
          }
        }
        if (object.isMesh && object.receiveShadow) stats.shadowReceivers += 1;
        let transparent = false;
        for (const material of objectMaterials) {
          if (material.visible === false) continue;
          if (material.transparent) transparent = true;
          if (
            material.transparent &&
            (Number(material.opacity) || 0) <= 0.01 &&
            !material.isShaderMaterial
          ) {
            stats.invisibleDrawn += 1;
            bump(invisibleByOwner, owner(object));
          } else if (
            material.transparent &&
            (material.opacity === undefined ? 1 : Number(material.opacity)) >= 0.99 &&
            !material.map &&
            !material.alphaMap &&
            !material.isShaderMaterial &&
            !material.vertexColors
          ) {
            stats.needlessTransparent += 1;
            bump(needlessTransparentByOwner, owner(object));
          }
          const cost = MATERIAL_COST[material.type];
          if (cost) bump(materialCostByType, material.type);
        }
        if (transparent) stats.transparentDrawables += 1;
        if (
          object.isMesh &&
          !object.isInstancedMesh &&
          !object.isSkinnedMesh &&
          geometry &&
          objectMaterials.length === 1 &&
          !Array.isArray(object.material)
        ) {
          const key = geometry;
          let byMaterial = pairs.get(key);
          if (!byMaterial) {
            byMaterial = new Map();
            pairs.set(key, byMaterial);
          }
          const material = objectMaterials[0];
          let pair = byMaterial.get(material);
          if (!pair) {
            pair = { count: 0, owner: owner(object), geometry, material };
            byMaterial.set(material, pair);
          }
          pair.count += 1;
        }
      }
    }
    const children = object.children;
    for (let index = 0; index < children.length; index += 1) {
      visit(children[index], visible);
    }
  };
  if (scene) visit(scene, true);

  stats.sceneGeometries = geometries.size;
  stats.sceneMaterials = materials.size;
  stats.materialCost = Object.fromEntries(materialCostByType);

  const textureUploadsByOwner = new Map();
  const oversizedTextures = [];
  const npotTextures = [];
  let textureBytes = 0;
  for (const [texture, record] of textures) {
    const { width, height } = textureSize(texture);
    textureBytes += width * height * 4 * (texture.generateMipmaps ? 1.33 : 1);
    if (Math.max(width, height) > 2048) {
      oversizedTextures.push(`${texture.name || record.owner} ${width}×${height}`);
    }
    if (
      width &&
      height &&
      (!isPowerOfTwo(width) || !isPowerOfTwo(height)) &&
      texture.generateMipmaps &&
      !texture.isCanvasTexture &&
      !texture.isDataTexture
    ) {
      npotTextures.push(`${texture.name || record.owner} ${width}×${height}`);
    }
    const version = Number(texture.version) || 0;
    const previousVersion = state.textureVersions.get(texture);
    if (comparable && previousVersion !== undefined && version > previousVersion) {
      stats.textureUploads += version - previousVersion;
      bump(
        textureUploadsByOwner,
        `${texture.name || record.owner} ${width}×${height}${texture.isCanvasTexture ? " canvas" : ""}`,
        version - previousVersion,
      );
    }
    state.textureVersions.set(texture, version);
  }
  stats.sceneTextures = textures.size;

  const geometryGroups = new Map();
  if (!state.geometryHashes) state.geometryHashes = new WeakMap();
  const geometryHashes = state.geometryHashes;
  const tessellated = [];
  const nonIndexed = [];
  for (const [geometry, record] of geometries) {
    const signature = geometrySignature(geometry);
    if (signature) {
      const key = `${signature}#${geometryContentHash(geometry, geometryHashes)}`;
      let group = geometryGroups.get(key);
      if (!group) {
        group = { copies: 0, owner: record.owner, signature };
        geometryGroups.set(key, group);
      }
      group.copies += 1;
    }
    if (
      record.visibleUses > 0 &&
      record.triangles >= 2000 &&
      record.radius > 0 &&
      record.radius < 1.5
    ) {
      tessellated.push({
        text: `${record.owner} · ${signature || geometry.type} · ${compact(record.triangles)} tris at ${record.radius.toFixed(2)}u radius`,
        weight: record.triangles * record.visibleUses,
      });
    } else if (
      record.visibleUses > 0 &&
      record.radius > 0 &&
      record.radius < TESSELLATED_SMALL_RADIUS &&
      TESSELLATED_TYPES.has(geometry.type) &&
      Object.entries(geometry.parameters || {}).some(
        ([key, value]) => /segments/i.test(key) && Number(value) > 48,
      )
    ) {
      tessellated.push({
        text: `${record.owner} · ${signature}`,
        weight: record.triangles * record.visibleUses,
      });
    }
    if (!geometry.index && (geometry.attributes?.position?.count || 0) >= 3000) {
      nonIndexed.push({
        text: `${record.owner} · ${geometry.type} ${compact(geometry.attributes.position.count)} vertices`,
        weight: geometry.attributes.position.count,
      });
    }
  }
  const duplicateGeometries = [...geometryGroups.entries()].filter(
    ([, group]) => group.copies > 1,
  );
  const duplicateGeometryCount = duplicateGeometries.reduce(
    (sum, [, group]) => sum + group.copies - 1,
    0,
  );

  const materialGroups = new Map();
  for (const [material, record] of materials) {
    const signature = materialSignature(material);
    if (!signature) continue;
    let group = materialGroups.get(signature);
    if (!group) {
      group = {
        copies: 0,
        owner: record.owner,
        type: material.type,
        name: material.name || "",
        color: colorHex(material.color),
      };
      materialGroups.set(signature, group);
    }
    group.copies += 1;
  }
  const duplicateMaterials = [...materialGroups.values()].filter(
    (group) => group.copies > 1,
  );
  const duplicateMaterialCount = duplicateMaterials.reduce(
    (sum, group) => sum + group.copies - 1,
    0,
  );

  const instancingGroups = [];
  for (const byMaterial of pairs.values()) {
    for (const pair of byMaterial.values()) {
      if (pair.count >= 6) instancingGroups.push(pair);
    }
  }
  instancingGroups.sort((a, b) => b.count - a.count);
  const instancingSavings = instancingGroups.reduce(
    (sum, pair) => sum + pair.count - 1,
    0,
  );

  const info = renderer?.info || {};
  const gpuGeometries = Math.max(0, Number(info.memory?.geometries) || 0);
  const gpuTextures = Math.max(0, Number(info.memory?.textures) || 0);
  const programs = Math.max(0, Number(info.programs?.length) || 0);
  const programOwners = new Map();
  const properties = renderer?.properties;
  if (typeof properties?.get === "function") {
    for (const [material, record] of materials) {
      if (typeof properties.has === "function" && !properties.has(material)) continue;
      const variants = properties.get(material)?.programs;
      if (!variants?.size) continue;
      let entry = programOwners.get(record.owner);
      if (!entry) {
        entry = { keys: new Set(), materials: 0, types: new Map() };
        programOwners.set(record.owner, entry);
      }
      variants.forEach((_program, key) => entry.keys.add(key));
      entry.materials += 1;
      bump(entry.types, String(material.type || "Material").replace(/Material$/, ""));
    }
  }
  const programsByElement = [...programOwners.entries()]
    .map(([label, entry]) => ({
      label,
      programs: entry.keys.size,
      materials: entry.materials,
      types: topCounts(entry.types, 3).join(", "),
    }))
    .sort((a, b) => b.programs - a.programs || b.materials - a.materials)
    .slice(0, 8);
  const orphanGeometries = Math.max(0, gpuGeometries - stats.sceneGeometries);
  const orphanTextures = Math.max(0, gpuTextures - stats.sceneTextures);
  state.leakHistory.push({ at: startedAt, geometries: orphanGeometries, textures: orphanTextures });
  while (state.leakHistory.length > 12) state.leakHistory.shift();
  const firstLeak = state.leakHistory[0];
  const leakMinutes = Math.max(0, (startedAt - firstLeak.at) / 60_000);
  const geometryGrowth = orphanGeometries - firstLeak.geometries;
  const textureGrowth = orphanTextures - firstLeak.textures;

  if (walk) {
    const reading = {
      at: startedAt,
      walks: walk.walks,
      visited: walk.visited,
      composed: walk.composed,
      thawed: walk.thawed,
    };
    const previousWalk = state.walk;
    if (previousWalk && reading.walks > previousWalk.walks) {
      const walks = reading.walks - previousWalk.walks;
      stats.walkVisitedPerWalk = (reading.visited - previousWalk.visited) / walks;
      stats.walkComposedPerWalk = (reading.composed - previousWalk.composed) / walks;
      stats.walkThawsPerSecond =
        (reading.thawed - previousWalk.thawed) /
        Math.max(0.001, (startedAt - previousWalk.at) / 1000);
    }
    state.walk = reading;
  }
  const matrixFix = () => {
    const owners = topCounts(unsettledByOwner, 4, (key, count) => `${key} ${compact(count)}`).join(", ");
    const walked = stats.walkVisitedPerWalk > 0
      ? `The walk visits ${compact(Math.round(stats.walkVisitedPerWalk))} objects per frame and recomposes ${compact(Math.round(stats.walkComposedPerWalk))}`
      : `${compact(stats.unsettledObjects)} objects sit outside settled subtrees`;
    const thaws = stats.walkThawsPerSecond >= 1 ? `; ${compact(Math.round(stats.walkThawsPerSecond))} frozen objects thaw per second` : "";
    const statics = stats.staticAutoMatrixObjects > 0
      ? ` ${compact(stats.staticAutoMatrixObjects)} objects never moved but still auto-update: set matrixAutoUpdate = false after placing them.`
      : "";
    return `${walked}${thaws}.${owners ? ` Unsettled objects by element: ${owners}.` : ""} Keep animated props in small groups, hide or detach subtrees that are not in view (matrixWorldAutoUpdate = false on hidden interiors), and add large subtrees in pieces: each added subtree is walked in full once.${statics}`;
  };

  const findings = [];
  const add = (finding) => findings.push(finding);
  const phase = (id) => profile?.phases?.find((entry) => entry.id === id)?.avgMs || 0;
  const gpuMs = Number(profile?.gpu?.avgMs);
  const cpuMs = Number(profile?.cpuMs) || 0;

  if (profile && profile.frames > 0) {
    const bound = Number.isFinite(gpuMs)
      ? gpuMs > cpuMs
        ? "GPU"
        : "CPU"
      : null;
    const overBudget = Math.max(cpuMs, Number.isFinite(gpuMs) ? gpuMs : 0);
    if (overBudget > FRAME_BUDGET_MS * 0.75) {
      const heaviest = [...profile.phases]
        .sort((a, b) => b.avgMs - a.avgMs)
        .slice(0, 3)
        .map((entry) => `${entry.label} ${entry.avgMs.toFixed(2)} ms`);
      add({
        id: "frame-budget",
        level: overBudget > FRAME_BUDGET_MS ? "high" : "caution",
        title: bound
          ? `Frames are ${bound}-bound: ${overBudget.toFixed(1)} ms of a ${FRAME_BUDGET_MS.toFixed(1)} ms budget`
          : `Main-thread work is ${cpuMs.toFixed(1)} ms of a ${FRAME_BUDGET_MS.toFixed(1)} ms budget`,
        evidence: `CPU ${cpuMs.toFixed(2)} ms/frame${Number.isFinite(gpuMs) ? ` · GPU ${gpuMs.toFixed(2)} ms/frame` : " · GPU timer unavailable in this browser"}. Heaviest phases: ${heaviest.join(", ")}.`,
        why: bound === "GPU"
          ? "The GPU finishes after the CPU: fewer pixels, cheaper materials, fewer shadow casters and less overdraw are what speed this up. Cutting JavaScript will not."
          : "The main thread spends the frame before the GPU gets work: cut per-frame JavaScript, scene-graph size and draw calls first.",
        fix: bound === "GPU"
          ? "Lower the pixel ratio, drop transparent overdraw, move decor to MeshLambert/MeshBasic, shrink the shadow frustum and caster count."
          : "Work down the phase list in the Frame budget: the top phase is the first thing to optimize.",
        offenders: heaviest,
        weight: overBudget * 10,
      });
    }
    for (const [id, threshold, fix] of [
      ["ambient", 1.5, "Throttle or cull ambient callbacks that animate objects off screen; skip them when their root is hidden or far away."],
      ["frame-animations", 1, "Per-frame callbacks run every rendered frame. Move anything that is not motion-critical to the ambient cadence."],
      ["labels", 1, "Name labels write DOM styles every 34 ms. Use transform instead of left/top, skip unchanged values, and cull labels outside the viewport."],
      ["avatars", 1.5, "Remote avatars are simulated even off screen. Skip the walk cycle for avatars outside the frustum or beyond label distance."],
      ["proximity", 1, "Proximity scans walk portals and boards. Use a spatial grid instead of scanning every candidate each tick."],
      ["matrices", 1, matrixFix()],
      ["mirror", 1, "The mirror renders six cube faces. Lower the cube target size, cap the refresh rate, or only capture while the player is in the lobby."],
      ["effects", 1, "One-shot effects are updated by per-object loops. Pool effect meshes and animate them in a shader or InstancedMesh."],
      ["render", 6, "renderer.render walks, culls and sorts every visible object before submitting. Fewer objects and draw calls reduce this directly."],
    ]) {
      const ms = phase(id);
      if (ms < threshold) continue;
      const entry = profile.phases.find((item) => item.id === id);
      const callbacks =
        id === "ambient" || id === "frame-animations"
          ? (profile.callbacks || []).slice(0, 5).map(
              (callback) =>
                `${callback.label}: ${callback.msPerSecond.toFixed(1)} ms/s · ${callback.calls} calls · avg ${callback.avgMs.toFixed(2)} ms · peak ${callback.peakMs.toFixed(2)} ms`,
            )
          : [];
      add({
        id: `phase-${id}`,
        level: ms >= threshold * 3 ? "high" : "caution",
        title: `${entry.label} costs ${ms.toFixed(2)} ms per frame`,
        evidence: `Average ${ms.toFixed(2)} ms, worst ${entry.peakMs.toFixed(1)} ms over ${profile.frames} frames (${(Number(profile.elapsedMs) / 1000 || 0).toFixed(1)} s).`,
        why: "Every millisecond here is main-thread time the browser cannot use for input or the next frame.",
        fix,
        offenders: callbacks,
        weight: ms * 8,
      });
    }
    const shadowDraws = Number(profile.passes?.shadow) || 0;
    const mainDraws = Number(profile.passes?.main) || 0;
    if (shadowDraws >= 150) {
      add({
        id: "shadow-draws",
        level: shadowDraws >= 600 ? "high" : "caution",
        title: `Shadow pass submits ${compact(shadowDraws)} draws per frame`,
        evidence: `${compact(shadowDraws)} shadow draws vs ${compact(mainDraws)} main-view draws per frame (averaged across frames with and without a shadow refresh). ${compact(stats.shadowCasters)} visible meshes cast shadows, ${compact(stats.smallShadowCasters)} of them smaller than 0.35 units.`,
        why: "three.js re-renders every shadow caster into the shadow map on each refresh; small decor adds depth draws without a visible shadow.",
        fix: "Set castShadow = false on small props, foliage, labels and interior decor; keep it on buildings, avatars and large landmarks. Tighten the sun's shadow camera to the area around the player.",
        offenders: topCounts(smallCastersByOwner, 6, (key, count) => `${key}: ${count} small casters`),
        weight: shadowDraws / 20,
      });
    }
  }

  if (instancingSavings >= 20) {
    add({
      id: "instancing",
      level: instancingSavings >= 200 ? "high" : "caution",
      title: `${compact(instancingSavings)} draw calls could be removed with InstancedMesh`,
      evidence: `${instancingGroups.length} groups of visible meshes share both geometry and material but are drawn one by one.`,
      why: "Each Mesh is its own draw call; identical geometry + material pairs can be one InstancedMesh (or one merged BufferGeometry for static parts).",
      fix: "Build these groups as InstancedMesh with per-instance matrices (and instanceColor for tints), or mergeGeometries() for static scenery.",
      offenders: instancingGroups
        .slice(0, 8)
        .map(
          (pair) =>
            `${pair.owner} · ${pair.geometry.type} + ${pair.material.type}${pair.material.name ? ` "${pair.material.name}"` : ""}: ${pair.count} meshes → 1 draw`,
        ),
      weight: instancingSavings / 4,
    });
  }
  if (duplicateMaterialCount >= 1) {
    add({
      id: "duplicate-materials",
      level: duplicateMaterialCount >= 200 ? "high" : duplicateMaterialCount >= 20 ? "caution" : "info",
      title: `${compact(duplicateMaterialCount)} materials are identical copies`,
      evidence: `${compact(stats.sceneMaterials)} material objects collapse to ${compact(materialGroups.size)} distinct configurations.`,
      why: "Separate but identical materials cannot be batched, each carries its own uniforms upload and state change, and they defeat instancing.",
      fix: "Create materials through a cache keyed by their parameters and share one instance; clone only when a material is animated independently.",
      offenders: duplicateMaterials
        .sort((a, b) => b.copies - a.copies)
        .slice(0, 6)
        .map((group) => `${group.owner} · ${group.type}${group.name ? ` "${group.name}"` : ""} ${group.color} × ${group.copies}`),
      weight: duplicateMaterialCount / 10,
    });
  }
  if (duplicateGeometryCount >= 20) {
    add({
      id: "duplicate-geometries",
      level: duplicateGeometryCount >= 200 ? "high" : "caution",
      title: `${compact(duplicateGeometryCount)} geometries are identical copies`,
      evidence: `${compact(stats.sceneGeometries)} geometry objects collapse to ${compact(geometryGroups.size)} distinct shapes.`,
      why: "Every BufferGeometry is its own GPU buffer set: copies waste memory, upload time and prevent instancing.",
      fix: "Create each shape once (a module-level cache keyed by parameters) and reuse it across meshes; scale with object.scale instead of new dimensions.",
      offenders: duplicateGeometries
        .sort((a, b) => b[1].copies - a[1].copies)
        .slice(0, 6)
        .map(([, group]) => `${group.owner} · ${group.signature} × ${group.copies}`),
      weight: duplicateGeometryCount / 10,
    });
  }
  if (stats.invisibleDrawn > 0) {
    add({
      id: "invisible-drawn",
      level: stats.invisibleDrawn >= 20 ? "high" : "caution",
      title: `${compact(stats.invisibleDrawn)} fully transparent meshes are still drawn`,
      evidence: "Visible objects whose material opacity is 0 are sorted and submitted every frame.",
      why: "three.js only skips objects with visible = false; opacity 0 still costs a draw call, blending and sorting.",
      fix: "Set object.visible = false when a fade reaches 0 and back to true when it starts again.",
      offenders: topCounts(invisibleByOwner),
      weight: stats.invisibleDrawn * 2,
    });
  }
  if (stats.needlessTransparent >= 10) {
    add({
      id: "needless-transparency",
      level: stats.needlessTransparent >= 100 ? "high" : "caution",
      title: `${compact(stats.needlessTransparent)} opaque meshes are flagged transparent`,
      evidence: `Materials with transparent = true, opacity 1 and no alpha texture. ${compact(stats.transparentDrawables)} visible drawables are transparent in total.`,
      why: "Transparent objects skip early depth rejection, are sorted back-to-front on the CPU every frame and blend every fragment.",
      fix: "Set transparent = false while opacity is 1 (toggle it on only during fades), or use alphaTest for cut-outs.",
      offenders: topCounts(needlessTransparentByOwner),
      weight: stats.needlessTransparent / 4,
    });
  }
  if (stats.cullingOff >= 10) {
    add({
      id: "culling-off",
      level: stats.cullingOff >= 100 ? "high" : "caution",
      title: `${compact(stats.cullingOff)} visible objects have frustum culling disabled`,
      evidence: "frustumCulled = false objects are drawn even when they are behind the camera.",
      why: "Frustum culling is three.js's cheapest optimization; disabling it is only right for objects whose bounds are wrong (skinned or shader-displaced).",
      fix: "Restore frustumCulled and fix the bounding sphere instead (computeBoundingSphere after moving vertices, or a padded boundingSphere for InstancedMesh).",
      offenders: topCounts(cullingOffByOwner),
      weight: stats.cullingOff / 5,
    });
  }
  if (stats.staticAutoMatrixObjects >= 2000) {
    add({
      id: "static-matrices",
      level: stats.staticAutoMatrixObjects >= 10_000 ? "high" : "caution",
      title: `${compact(stats.staticAutoMatrixObjects)} static objects still auto-update their matrices`,
      evidence: `${compact(stats.autoMatrixObjects)} of ${compact(stats.objects)} objects have matrixAutoUpdate = true; ${compact(stats.staticAutoMatrixObjects)} did not move since the previous audit.${profile ? ` Scene matrix update: ${phase("matrices").toFixed(2)} ms/frame.` : ""}`,
      why: "Every frame the matrix walk visits these objects and compares their transform even though nothing changed.",
      fix: "After placing static scenery call object.updateMatrix() and set matrixAutoUpdate = false (and matrixWorldAutoUpdate = false for whole static subtrees).",
      offenders: topCounts(staticByOwner),
      weight: stats.staticAutoMatrixObjects / 500,
    });
  }
  if (stats.hiddenObjects >= 5000) {
    add({
      id: "hidden-objects",
      level: stats.hiddenObjects >= 20_000 ? "high" : "caution",
      title: `${compact(stats.hiddenObjects)} hidden objects stay in the scene graph`,
      evidence: `${compact(stats.hiddenObjects)} of ${compact(stats.objects)} objects are invisible (themselves or through a parent).`,
      why: "Hidden objects are skipped for drawing but are still visited by every matrix update, traverse() and raycast.",
      fix: "Detach long-hidden subtrees (parent.remove) and re-add them when needed, or give them matrixWorldAutoUpdate = false while hidden.",
      offenders: topCounts(hiddenByOwner),
      weight: stats.hiddenObjects / 2000,
    });
  }
  if (tessellated.length) {
    const tris = tessellated.reduce((sum, item) => sum + item.weight, 0);
    add({
      id: "over-tessellated",
      level: tris >= 400_000 ? "high" : "caution",
      title: `${tessellated.length} small shapes use far more triangles than they can show`,
      evidence: `${compact(tris)} triangles across small visible meshes or high segment counts.`,
      why: "Triangles on a shape a few pixels wide are pure vertex and rasterization cost; 16–32 segments is plenty for props.",
      fix: "Lower widthSegments/heightSegments/radialSegments, or add a THREE.LOD with a low-poly level for distance.",
      offenders: tessellated.sort((a, b) => b.weight - a.weight).slice(0, 6).map((item) => item.text),
      weight: tris / 50_000,
    });
  }
  if (nonIndexed.length) {
    add({
      id: "non-indexed",
      level: "info",
      title: `${nonIndexed.length} large geometries are not indexed`,
      evidence: "Non-indexed geometry repeats shared vertices, so the vertex shader runs up to 6× per vertex.",
      why: "Indexed geometry lets the GPU reuse transformed vertices from its post-transform cache.",
      fix: "Run BufferGeometryUtils.mergeVertices() once when building these geometries.",
      offenders: nonIndexed.sort((a, b) => b.weight - a.weight).slice(0, 6).map((item) => item.text),
      weight: 1,
    });
  }
  if (comparable) {
    const uploadsPerSecond = stats.textureUploads / elapsedSeconds;
    if (uploadsPerSecond >= 2) {
      add({
        id: "texture-uploads",
        level: uploadsPerSecond >= 10 ? "high" : "caution",
        title: `${uploadsPerSecond.toFixed(1)} texture uploads per second`,
        evidence: `${stats.textureUploads} texture re-uploads (needsUpdate) in the last ${elapsedSeconds.toFixed(0)} s.`,
        why: "Every needsUpdate on a CanvasTexture re-sends the whole image with texImage2D and can stall the GPU process.",
        fix: "Only set needsUpdate when the text or image actually changed, draw to a smaller canvas, and batch label text into an atlas.",
        offenders: topCounts(textureUploadsByOwner, 6, (key, count) => `${key}: ${count}×`),
        weight: uploadsPerSecond * 2,
      });
    }
    const bytesPerSecond = stats.bufferUploadBytes / elapsedSeconds;
    if (bytesPerSecond >= 256 * 1024) {
      add({
        id: "buffer-uploads",
        level: bytesPerSecond >= 2 * 1024 * 1024 ? "high" : "caution",
        title: `${(bytesPerSecond / (1024 * 1024)).toFixed(1)} MB/s of vertex buffers re-uploaded`,
        evidence: "Geometry attributes flagged needsUpdate are re-sent whole to the GPU.",
        why: "Updating a full attribute each frame costs bandwidth; updateRanges limits the upload to what changed.",
        fix: "Use attribute.addUpdateRange(start, count) and setUsage(THREE.DynamicDrawUsage), or animate in the vertex shader.",
        offenders: topCounts(
          bufferUploadsByOwner,
          6,
          (key, bytes) => `${key}: ${(bytes / elapsedSeconds / 1024).toFixed(0)} KB/s`,
        ),
        weight: bytesPerSecond / (256 * 1024),
      });
    }
  }
  if (leakMinutes >= 0.5 && (geometryGrowth >= 50 || textureGrowth >= 20)) {
    add({
      id: "gpu-leak",
      level: geometryGrowth >= 500 || textureGrowth >= 100 ? "high" : "caution",
      title: "GPU resources are accumulating outside the scene",
      evidence: `renderer.info holds ${compact(gpuGeometries)} geometries and ${compact(gpuTextures)} textures; the scene graph uses ${compact(stats.sceneGeometries)} and ${compact(stats.sceneTextures)}. The gap grew by ${compact(geometryGrowth)} geometries and ${compact(textureGrowth)} textures in ${leakMinutes.toFixed(1)} min.`,
      why: "Removing an object from the scene does not free its GPU buffers; three.js keeps them until dispose() is called.",
      fix: "Call geometry.dispose(), material.dispose() and texture.dispose() when removing objects that will not come back.",
      offenders: [],
      weight: geometryGrowth / 20 + textureGrowth / 5,
    });
  } else if (orphanGeometries >= 1500 || orphanTextures >= 300) {
    add({
      id: "gpu-orphans",
      level: "info",
      title: "Many GPU resources are not in the scene graph",
      evidence: `${compact(orphanGeometries)} geometries and ${compact(orphanTextures)} textures live on the GPU but are not reachable from the scene (paged districts, detached parts or leaks).`,
      why: "Undisposed resources hold GPU memory and make eviction stutter more likely on small GPUs.",
      fix: "Dispose resources for content that is gone for good; this reading turns into a leak warning if the gap keeps growing.",
      offenders: [],
      weight: 1,
    });
  }
  if (programs >= 120) {
    add({
      id: "programs",
      level: programs >= 250 ? "high" : "caution",
      title: `${programs} shader programs are compiled`,
      evidence: `${compact(stats.sceneMaterials)} materials produce ${programs} distinct programs.${programsByElement.length ? ` Most variants: ${programsByElement.slice(0, 4).map((row) => `${row.label} ${row.programs}`).join(" · ")}.` : ""}`,
      why: "Each new material permutation (type, maps, defines, fog, lights, skinning) compiles a program; first use stalls the frame.",
      fix: "Standardize material types and texture slots, avoid per-material onBeforeCompile variants, and warm new materials with renderer.compileAsync().",
      offenders: programsByElement.map((row) => `${row.label}: ${row.programs} programs, ${row.materials} materials (${row.types})`),
      weight: programs / 40,
    });
  }
  const standard =
    (stats.materialCost.MeshStandardMaterial || 0) +
    (stats.materialCost.MeshPhysicalMaterial || 0);
  if (standard >= 500) {
    add({
      id: "material-cost",
      level: Number.isFinite(gpuMs) && gpuMs > FRAME_BUDGET_MS * 0.6 ? "caution" : "info",
      title: `${compact(standard)} visible meshes use physically based materials`,
      evidence: Object.entries(stats.materialCost)
        .sort((a, b) => b[1] - a[1])
        .map(([type, count]) => `${type} ${compact(count)}`)
        .join(" · "),
      why: "MeshStandard/MeshPhysical evaluate a full PBR lighting model per pixel; Lambert and Basic are several times cheaper.",
      fix: "Use MeshLambertMaterial for matte decor and MeshBasicMaterial for unlit signs and labels; keep PBR for hero surfaces.",
      offenders: [],
      weight: standard / 400,
    });
  }
  const lightBudget = Number(quality?.pointLightBudget);
  if (stats.activePointLights + stats.activeSpotLights > Math.max(4, Number.isFinite(lightBudget) ? lightBudget : 4)) {
    add({
      id: "lights",
      level: stats.shadowPointLights ? "high" : "caution",
      title: `${stats.activePointLights + stats.activeSpotLights} local lights are active`,
      evidence: `${stats.activePointLights} point, ${stats.activeSpotLights} spot${stats.shadowPointLights ? `, ${stats.shadowPointLights} point lights cast shadows (6 shadow renders each)` : ""}.`,
      why: "Every active light is evaluated for every lit fragment of every lit material, and changing the light count recompiles programs.",
      fix: "Keep a fixed small pool of lights and move them to the nearest sources; bake the rest into emissive materials or light maps.",
      offenders: [],
      weight: stats.activePointLights,
    });
  }
  if (stats.sprites >= 150) {
    add({
      id: "sprites",
      level: stats.sprites >= 500 ? "high" : "caution",
      title: `${compact(stats.sprites)} sprites are visible`,
      evidence: "Every THREE.Sprite is its own transparent draw call.",
      why: "Sprites cannot be batched and are sorted as transparent objects each frame.",
      fix: "Draw icon and label clouds with one THREE.Points or InstancedMesh over a texture atlas, and hide sprites beyond reading distance.",
      offenders: topCounts(spritesByOwner),
      weight: stats.sprites / 50,
    });
  }
  if (oversizedTextures.length) {
    add({
      id: "oversized-textures",
      level: "caution",
      title: `${oversizedTextures.length} textures are larger than 2048 px`,
      evidence: `Estimated ${Math.round(textureBytes / (1024 * 1024))} MB of textures in the scene.`,
      why: "Large textures cost GPU memory and upload time; 2048 is usually enough for the web.",
      fix: "Resize to ≤ 2048 (≤ 1024 on phones) and ship KTX2/Basis compressed textures.",
      offenders: oversizedTextures.slice(0, 6),
      weight: oversizedTextures.length,
    });
  }
  if (npotTextures.length >= 5) {
    add({
      id: "npot-textures",
      level: "info",
      title: `${npotTextures.length} mipmapped textures are not power-of-two`,
      evidence: "Non-power-of-two images with mipmaps are rescaled or handled on slower paths by some drivers.",
      why: "Power-of-two sizes (256, 512, 1024, 2048) get full mipmap and wrap support everywhere.",
      fix: "Author textures at power-of-two sizes, or disable mipmaps for UI-like textures that are always seen at one scale.",
      offenders: npotTextures.slice(0, 6),
      weight: 0.5,
    });
  }
  const bufferPixels =
    (Number(output?.drawingBufferWidth) || 0) * (Number(output?.drawingBufferHeight) || 0);
  if (bufferPixels >= 3_600_000) {
    add({
      id: "fill-rate",
      level: Number.isFinite(gpuMs) && gpuMs > FRAME_BUDGET_MS * 0.6 ? "high" : "caution",
      title: `Rendering ${compact(bufferPixels)} pixels per frame`,
      evidence: `${output.drawingBufferWidth}×${output.drawingBufferHeight} drawing buffer at DPR ${Number(output.pixelRatio || 0).toFixed(2)}.`,
      why: "Fragment cost scales with pixel count; DPR 2 is 4× the pixels of DPR 1.",
      fix: "Cap renderer.setPixelRatio(Math.min(devicePixelRatio, 1.5)) and let the adaptive tier drop it further under load.",
      offenders: [],
      weight: bufferPixels / 1_000_000,
    });
  }

  const order = { high: 0, caution: 1, info: 2 };
  findings.sort(
    (a, b) => order[a.level] - order[b.level] || (b.weight || 0) - (a.weight || 0),
  );
  state.at = startedAt;
  state.audits += 1;
  return {
    at: Date.now(),
    auditMs: now() - startedAt,
    stats: {
      ...stats,
      textureMB: textureBytes / (1024 * 1024),
      gpuGeometries,
      gpuTextures,
      programs,
      programsByElement,
      orphanGeometries,
      orphanTextures,
      instancingGroups: instancingGroups.length,
      instancingSavings,
      duplicateGeometries: duplicateGeometryCount,
      duplicateMaterials: duplicateMaterialCount,
      distinctGeometries: geometryGroups.size,
      distinctMaterials: materialGroups.size,
    },
    findings,
  };
}
