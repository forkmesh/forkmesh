import { canvasTextureScale } from "./canvas.js";
import { objectIsEffectivelyVisible } from "./object-utils.js";
import { standardMaterialParameters } from "./materials.js";
import { SETTLED_PARENT, SETTLED_SUBTREE } from "./settled-matrices.js";
import * as resources from "./resources.js";

export const FIGURE_NEAR_DETAIL_PIXELS_PER_UNIT = 140;
export const FIGURE_NEAR_DETAIL_EXIT_RATIO = 0.85;
export const FIGURE_NEAR_DETAIL_LIMIT = 24;
export const FIGURE_LAYER_COPIES_PER_FRAME = 32;
export const FIGURE_LAYER_COPIES_MIN = 4;
export const FIGURE_LAYER_COPY_BUDGET_MS = 8;
const FIGURE_LAYER_UNPACK_DEFAULTS = Object.freeze([
  "UNPACK_ROW_LENGTH",
  "UNPACK_IMAGE_HEIGHT",
  "UNPACK_SKIP_PIXELS",
  "UNPACK_SKIP_ROWS",
  "UNPACK_SKIP_IMAGES",
]);
export const FIGURE_ELIGIBILITY_CHECKS_PER_FRAME = 6;
const FIGURE_CAPACITY_STEP = 64;
const FIGURE_BOUNDS_RADIUS = 3.4;
const FIGURE_LAYER_SOURCE_UNREADABLE = new WeakSet();
const FIGURE_STAGING_CANVAS = Object.freeze({ willReadFrequently: true });

export const FIGURE_LAYER_KINDS = Object.freeze({
  shirt: { width: 256, height: 256, mipmaps: false, compact: false },
  face: { width: 128, height: 128, mipmaps: false, compact: false },
  badge: { width: 256, height: 256, mipmaps: true, compact: true },
  backName: { width: 256, height: 128, mipmaps: false, compact: false },
});

const FIGURE_LAYER_SOURCE_PART = { shirt: 0, face: 4, badge: 5, backName: 6 };

export const FIGURE_BATCH_PARTS = Object.freeze([
  { kind: "torso", layer: "shirt", node: (data) => data.torso, near: true },
  { kind: "leftArm", layer: "shirt", node: (data) => data.leftArm, near: true },
  { kind: "rightArm", layer: "shirt", node: (data) => data.rightArm, near: true },
  { kind: "head", layer: "shirt", node: (data) => data.head, near: true },
  { kind: "face", layer: "face", node: (data) => data.faceMesh, near: true },
  { kind: "badge", layer: "badge", node: (data) => data.badge },
  { kind: "backName", layer: "backName", node: (data) => data.backName },
]);
const PART_COUNT = FIGURE_BATCH_PARTS.length;
const NEAR_PARTS = FIGURE_BATCH_PARTS
  .map((part, index) => (part.near ? index : -1))
  .filter((index) => index >= 0);
const NEAR_OWN_PARTS = FIGURE_BATCH_PARTS
  .map((part, index) => (part.near ? -1 : index))
  .filter((index) => index >= 0);
export const FIGURE_NEAR_UPLOAD_BUDGET_MS = 4;

function batchedState(state) {
  return state === "high" || state === "near";
}
const LAYER_NAMES = Object.keys(FIGURE_LAYER_KINDS);

function layerSize(kind) {
  const spec = FIGURE_LAYER_KINDS[kind];
  const scale = spec.compact ? Math.min(1, canvasTextureScale || 1) : 1;
  return {
    width: Math.max(32, Math.round(spec.width * scale)),
    height: Math.max(32, Math.round(spec.height * scale)),
  };
}

function sourceImageReady(texture) {
  const image = texture?.image;
  if (!image || FIGURE_LAYER_SOURCE_UNREADABLE.has(image)) return false;
  if (image.complete === false) return false;
  const width = image.naturalWidth || image.videoWidth || image.width || 0;
  const height = image.naturalHeight || image.videoHeight || image.height || 0;
  return width > 0 && height > 0;
}

function visibleWithin(node, root) {
  let current = node;
  while (current && current !== root) {
    if (current.visible === false) return false;
    current = current.parent;
  }
  return current === root;
}

function drawableKind(object) {
  return object.isMesh === true || object.isSprite === true ||
    object.isLine === true || object.isPoints === true;
}

export function figureBatchable(avatar, owned) {
  const stack = [avatar];
  while (stack.length) {
    const node = stack.pop();
    if (node.visible === false) continue;
    if (
      drawableKind(node) &&
      !owned.has(node) &&
      (node.layers.mask & 1) !== 0 &&
      node.material?.visible !== false
    ) {
      return false;
    }
    const children = node.children;
    for (let index = 0; index < children.length; index += 1) {
      stack.push(children[index]);
    }
  }
  return true;
}

export function layeredFigureMaterial(THREE, type, parameters, uniform, key) {
  const material = resources.uniqueMaterial(THREE, parameters, type);
  material.userData.forkmeshSharedResource = true;
  material.onBeforeCompile = (shader) => {
    shader.uniforms.figureLayers = uniform;
    shader.vertexShader = shader.vertexShader
      .replace(
        "#include <common>",
        "#include <common>\nflat varying int vFigureLayer;\nvarying vec2 vFigureUv;",
      )
      .replace(
        "#include <color_vertex>",
        "#include <color_vertex>\nvColor = vec4( 1.0 );\nvFigureLayer = int( instanceColor.r + 0.5 );\nvFigureUv = uv;",
      );
    shader.fragmentShader = shader.fragmentShader
      .replace(
        "#include <common>",
        "#include <common>\nuniform sampler2DArray figureLayers;\nflat varying int vFigureLayer;\nvarying vec2 vFigureUv;",
      )
      .replace(
        "#include <map_fragment>",
        "diffuseColor *= texture( figureLayers, vec3( vFigureUv, float( vFigureLayer ) ) );",
      );
  };
  material.customProgramCacheKey = () => `forkmesh-figure-layers:${key}`;
  return material;
}

export function createFigureBatches(THREE, {
  parent,
  renderer,
  camera: mainCamera,
  forceFullDetail = () => false,
  gpuBusy = () => false,
}) {
  const records = new Map();
  const thresholds = {
    near: FIGURE_NEAR_DETAIL_PIXELS_PER_UNIT,
  };
  const freeSlots = [];
  let nextSlot = 0;
  let capacity = 0;
  let reserved = 0;
  let syncMs = 0;
  let eligibilityCursor = 0;
  const hidden = [];
  const muted = [];
  const sphere = new THREE.Sphere();
  let boundsDirty = true;
  const position = new THREE.Vector3();
  const copyAt = new THREE.Vector3();
  const layers = {};
  const materials = {};
  const batches = [];
  const counters = {
    full: 0,
    near: 0,
    high: 0,
    hidden: 0,
    waiting: 0,
    copies: 0,
    pendingCopies: 0,
    ineligible: 0,
  };

  for (const kind of LAYER_NAMES) {
    const { width, height } = layerSize(kind);
    const canvas = document.createElement("canvas");
    canvas.width = width;
    canvas.height = height;
    const context = canvas.getContext("2d", FIGURE_STAGING_CANVAS);
    layers[kind] = {
      kind,
      width,
      height,
      mipmaps: FIGURE_LAYER_KINDS[kind].mipmaps,
      uniform: { value: null },
      texture: null,
      canvas,
      context,
      staging: new THREE.Texture(canvas),
    };
  }
  materials.shirt = layeredFigureMaterial(
    THREE,
    "MeshStandardMaterial",
    standardMaterialParameters(THREE, "#ffffff", { roughness: 0.8 }),
    layers.shirt.uniform,
    "shirt",
  );
  materials.face = layeredFigureMaterial(
    THREE,
    "MeshBasicMaterial",
    {},
    layers.face.uniform,
    "face",
  );
  materials.badge = layeredFigureMaterial(
    THREE,
    "MeshBasicMaterial",
    { toneMapped: false },
    layers.badge.uniform,
    "badge",
  );
  materials.backName = layeredFigureMaterial(
    THREE,
    "MeshBasicMaterial",
    { toneMapped: false, alphaTest: 0.5 },
    layers.backName.uniform,
    "backName",
  );
  FIGURE_BATCH_PARTS.forEach((part) => {
    batches.push({
      part,
      source: null,
      material: materials[part.layer],
      mesh: null,
      members: [],
      from: Infinity,
      to: -1,
    });
  });

  function allocateLayers(size) {
    for (const kind of LAYER_NAMES) {
      const layer = layers[kind];
      const previous = layer.texture;
      const texture = new THREE.DataArrayTexture(null, layer.width, layer.height, size);
      texture.format = THREE.RGBAFormat;
      texture.type = THREE.UnsignedByteType;
      texture.colorSpace = THREE.SRGBColorSpace;
      texture.magFilter = THREE.LinearFilter;
      texture.minFilter = layer.mipmaps
        ? THREE.LinearMipmapLinearFilter
        : THREE.LinearFilter;
      texture.generateMipmaps = layer.mipmaps;
      texture.anisotropy = layer.mipmaps ? 8 : 1;
      texture.source.dataReady = false;
      texture.userData.forkmeshSharedResource = true;
      texture.needsUpdate = true;
      renderer.initTexture(texture);
      if (previous) {
        renderer.copyTextureToTexture(previous, texture, null, copyAt.set(0, 0, 0));
        previous.dispose();
      }
      layer.texture = texture;
      layer.uniform.value = texture;
    }
  }

  function allocateMeshes(size) {
    const template = records.values().next().value;
    for (const batch of batches) {
      if (!batch.source && template) {
        batch.source = batch.part.node(template.data)?.geometry || null;
      }
      if (!batch.source) continue;
      if (batch.mesh) {
        parent.remove(batch.mesh);
        batch.mesh.dispose();
      }
      const mesh = new THREE.InstancedMesh(batch.source, batch.material, size);
      const layerIds = new THREE.InstancedBufferAttribute(new Float32Array(size * 3), 3);
      layerIds.setUsage(THREE.DynamicDrawUsage);
      mesh.instanceColor = layerIds;
      mesh.name = `forkmesh-figure-${batch.part.kind}-batch`;
      mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
      mesh.count = 0;
      mesh.visible = false;
      mesh.castShadow = batch.part.near === true;
      mesh.receiveShadow = true;
      mesh.raycast = () => {};
      mesh.userData.figureBatch = batch.part.kind;
      mesh.boundingSphere = sphere.clone();
      parent.add(mesh);
      batch.mesh = mesh;
      batch.layerIds = layerIds;
      batch.members.length = 0;
      batch.from = Infinity;
      batch.to = -1;
    }
  }

  function grow(size) {
    capacity = size;
    allocateLayers(size);
    allocateMeshes(size);
    for (const record of records.values()) resetInstances(record);
    boundsDirty = true;
  }

  function resetInstances(record) {
    record.state = "full";
    record.refresh = true;
    record.visibleMask = -1;
    record.indices.fill(-1);
  }

  function resetRecord(record) {
    resetInstances(record);
    record.copied = false;
    for (const kind of LAYER_NAMES) {
      record.layerTextures[kind] = null;
      record.layerVersions[kind] = -1;
    }
  }

  function register(avatar) {
    const data = avatar?.userData;
    if (!data || records.has(avatar)) return false;
    const nodes = FIGURE_BATCH_PARTS.map((part) => part.node(data));
    if (nodes.some((node) => !node?.isMesh || Array.isArray(node.material))) {
      return false;
    }
    const slot = freeSlots.length ? freeSlots.pop() : nextSlot++;
    const record = {
      avatar,
      data,
      slot,
      nodes,
      owned: new Set(nodes),
      state: "full",
      desired: "full",
      nextState: "full",
      eligible: false,
      checked: false,
      copied: false,
      visibleMask: -1,
      matrix: new Float64Array(16),
      indices: new Int32Array(PART_COUNT),
      layerTextures: {},
      layerVersions: {},
      pixels: 0,
      shape: -1,
      refresh: true,
      highDetail: data.highDetail || null,
    };
    resetRecord(record);
    records.set(avatar, record);
    return true;
  }

  function release(avatar) {
    const record = records.get(avatar);
    if (!record) return;
    for (let index = 0; index < PART_COUNT; index += 1) {
      removeInstance(batches[index], record, index);
    }
    records.delete(avatar);
    freeSlots.push(record.slot);
    boundsDirty = true;
  }

  function markRange(batch, index) {
    if (index < batch.from) batch.from = index;
    if (index > batch.to) batch.to = index;
  }

  function writeMatrix(batch, index, elements, force) {
    const array = batch.mesh.instanceMatrix.array;
    const offset = index * 16;
    let changed = force;
    for (let component = 0; component < 16; component += 1) {
      const value = Math.fround(elements[component]);
      if (array[offset + component] !== value) {
        array[offset + component] = value;
        changed = true;
      }
    }
    if (changed) markRange(batch, index);
  }

  function addInstance(batch, record, part) {
    const index = batch.members.length;
    batch.members.push(record);
    record.indices[part] = index;
    batch.layerIds.array[index * 3] = record.slot;
    writeMatrix(batch, index, record.nodes[part].matrixWorld.elements, true);
  }

  function removeInstance(batch, record, part) {
    const index = record.indices[part];
    if (index < 0 || !batch.mesh) return;
    record.indices[part] = -1;
    const last = batch.members.length - 1;
    const moved = batch.members[last];
    batch.members.length = last;
    if (index === last) return;
    batch.members[index] = moved;
    moved.indices[part] = index;
    batch.layerIds.array[index * 3] = moved.slot;
    const array = batch.mesh.instanceMatrix.array;
    array.copyWithin(index * 16, last * 16, last * 16 + 16);
    markRange(batch, index);
  }

  let unpackStateKnown = false;
  function rememberUnpackState() {
    if (unpackStateKnown) return;
    unpackStateKnown = true;
    const gl = renderer.getContext?.();
    const state = renderer.state;
    if (!gl || typeof state?.pixelStorei !== "function") return;
    for (const name of FIGURE_LAYER_UNPACK_DEFAULTS) {
      if (gl[name] !== undefined) state.pixelStorei(gl[name], 0);
    }
  }

  function copyLayer(layer, slot, texture) {
    rememberUnpackState();
    const { context, width, height } = layer;
    context.setTransform(1, 0, 0, 1, 0, 0);
    context.clearRect(0, 0, width, height);
    context.setTransform(1, 0, 0, -1, 0, height);
    try {
      context.drawImage(texture.image, 0, 0, width, height);
      renderer.copyTextureToTexture(
        layer.staging,
        layer.texture,
        null,
        copyAt.set(0, 0, slot),
      );
      return true;
    } catch (_) {
      FIGURE_LAYER_SOURCE_UNREADABLE.add(texture.image);
      const canvas = document.createElement("canvas");
      canvas.width = width;
      canvas.height = height;
      layer.canvas = canvas;
      layer.context = canvas.getContext("2d", FIGURE_STAGING_CANVAS);
      layer.staging = new THREE.Texture(canvas);
      return false;
    }
  }

  function staleLayers(record, stale) {
    stale.length = 0;
    for (const kind of LAYER_NAMES) {
      const texture = record.nodes[FIGURE_LAYER_SOURCE_PART[kind]].material?.map;
      if (
        texture &&
        record.layerTextures[kind] === texture &&
        record.layerVersions[kind] === texture.version
      ) {
        continue;
      }
      if (!sourceImageReady(texture)) return false;
      stale.push(kind, texture);
    }
    return true;
  }

  const stale = [];
  const nearUploads = new WeakMap();
  const copies = { badge: [], shirt: [], face: [], backName: [] };
  let copyBudget = 0;

  function queueCopies(record) {
    if (record.copied && !record.refresh && record.state === record.desired) {
      return true;
    }
    if (!staleLayers(record, stale)) return false;
    if (!stale.length) return true;
    if (copyBudget < stale.length / 2) return record.copied;
    for (let index = 0; index < stale.length; index += 2) {
      const kind = stale[index];
      const texture = stale[index + 1];
      copies[kind].push(record, texture);
      record.layerTextures[kind] = texture;
      record.layerVersions[kind] = texture.version;
      copyBudget -= 1;
    }
    return true;
  }

  let copyCostMs = 0;
  function frameCopyBudget() {
    if (!(copyCostMs > 0)) return FIGURE_LAYER_COPIES_MIN;
    return Math.max(
      FIGURE_LAYER_COPIES_MIN,
      Math.min(
        FIGURE_LAYER_COPIES_PER_FRAME,
        Math.floor(FIGURE_LAYER_COPY_BUDGET_MS / copyCostMs),
      ),
    );
  }

  function flushCopies() {
    const startedAt = performance.now();
    let total = 0;
    for (const kind of LAYER_NAMES) {
      const list = copies[kind];
      if (!list.length) continue;
      const layer = layers[kind];
      const texture = layer.texture;
      for (let index = 0; index < list.length; index += 2) {
        const record = list[index];
        const source = list[index + 1];
        texture.generateMipmaps = layer.mipmaps && index === list.length - 2;
        if (!copyLayer(layer, record.slot, source)) {
          record.layerTextures[kind] = null;
          record.eligible = false;
        } else {
          record.copied = true;
        }
        total += 1;
      }
      texture.generateMipmaps = layer.mipmaps;
      list.length = 0;
    }
    if (total > 0) {
      const perCopy = (performance.now() - startedAt) / total;
      copyCostMs = copyCostMs > 0 ? copyCostMs * 0.7 + perCopy * 0.3 : perCopy;
    }
    counters.copies += total;
  }

  function nearTexturesReady(record, startedAt) {
    let ready = true;
    for (const part of NEAR_OWN_PARTS) {
      const texture = record.nodes[part]?.material?.map;
      if (!texture?.isTexture || texture.version === 0) continue;
      if (nearUploads.get(texture) === texture.version) continue;
      if (performance.now() - startedAt >= FIGURE_NEAR_UPLOAD_BUDGET_MS) {
        ready = false;
        continue;
      }
      renderer.initTexture(texture);
      nearUploads.set(texture, texture.version);
    }
    return ready;
  }

  function checkEligibility(record) {
    record.eligible = figureBatchable(record.avatar, record.owned) &&
      record.nodes.every((node) =>
        node.onBeforeRender === THREE.Object3D.prototype.onBeforeRender);
    record.checked = true;
  }

  function avatarMoved(record) {
    const elements = record.avatar.matrixWorld.elements;
    const last = record.matrix;
    let moved = false;
    for (let index = 0; index < 16; index += 1) {
      if (last[index] !== elements[index]) {
        last[index] = elements[index];
        moved = true;
      }
    }
    return moved;
  }

  function visibilityMask(record) {
    let mask = 0;
    for (let index = 0; index < PART_COUNT; index += 1) {
      if (visibleWithin(record.nodes[index], record.avatar)) mask |= 1 << index;
    }
    return mask;
  }

  function writeRecord(record, changed, moved) {
    const avatar = record.avatar;
    const state = record.state;
    const drawn = batchedState(state);
    if (
      !changed &&
      !moved &&
      !record.refresh &&
      (!drawn ||
        (avatar[SETTLED_SUBTREE] === true &&
          avatar[SETTLED_PARENT] === avatar.parent))
    ) {
      return;
    }
    record.refresh = false;
    const mask = drawn ? visibilityMask(record) : 0;
    record.visibleMask = mask;
    for (let index = 0; index < PART_COUNT; index += 1) {
      const batch = batches[index];
      if (!batch.mesh) continue;
      const shown = partShown(index, state, mask);
      const at = record.indices[index];
      if (shown && at < 0) {
        addInstance(batch, record, index);
      } else if (!shown && at >= 0) {
        removeInstance(batch, record, index);
      } else if (shown) {
        writeMatrix(batch, at, record.nodes[index].matrixWorld.elements, false);
      }
    }
  }

  function partShown(index, state, mask) {
    if (!batchedState(state)) return false;
    if ((mask & (1 << index)) === 0) return false;
    const part = FIGURE_BATCH_PARTS[index];
    return state !== "near" || part.near === true;
  }

  function updateBounds() {
    let minX = Infinity;
    let minY = Infinity;
    let minZ = Infinity;
    let maxX = -Infinity;
    let maxY = -Infinity;
    let maxZ = -Infinity;
    let reach = 0;
    let drawn = 0;
    for (const record of records.values()) {
      if (!batchedState(record.state)) continue;
      const elements = record.avatar.matrixWorld.elements;
      const x = elements[12];
      const y = elements[13];
      const z = elements[14];
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (z < minZ) minZ = z;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
      if (z > maxZ) maxZ = z;
      reach = Math.max(reach, FIGURE_BOUNDS_RADIUS * record.avatar.scale.y);
      drawn += 1;
    }
    if (!drawn) {
      sphere.center.set(0, -1e6, 0);
      sphere.radius = 0;
    } else {
      sphere.center.set((minX + maxX) / 2, (minY + maxY) / 2, (minZ + maxZ) / 2);
      sphere.radius =
        Math.hypot(maxX - minX, maxY - minY, maxZ - minZ) / 2 + reach;
    }
    for (const batch of batches) batch.mesh?.boundingSphere?.copy(sphere);
  }

  function pixelsPerUnit(camera) {
    const height = Math.max(1, Number(renderer.domElement?.height) || 1);
    const fov = ((Number(camera.fov) || 50) * Math.PI) / 180;
    return (height * (Number(camera.zoom) || 1)) / (2 * Math.tan(fov / 2));
  }

  function desiredState(record, lodScale, cameraPosition) {
    const avatar = record.avatar;
    if (!record.eligible) return "full";
    if (forceFullDetail(avatar)) return "forced";
    const elements = avatar.matrixWorld.elements;
    position.set(elements[12], elements[13], elements[14]);
    const distance = Math.max(0.01, position.distanceTo(cameraPosition));
    const pixels = (lodScale * avatar.scale.y) / distance;
    record.pixels = pixels;
    const nearAt = record.state === "near"
      ? thresholds.near * FIGURE_NEAR_DETAIL_EXIT_RATIO
      : thresholds.near;
    if (pixels > nearAt) return "near";
    return "high";
  }

  const near = [];
  const cameraPosition = new THREE.Vector3();
  const byPixels = (a, b) => b.pixels - a.pixels;

  function sync(camera) {
    restore();
    if (!records.size) return;
    if (camera !== mainCamera) {
      hideBatched();
      return;
    }
    const startedAt = performance.now();
    const needed = Math.max(nextSlot, reserved);
    if (needed > capacity) {
      let size = Math.max(FIGURE_CAPACITY_STEP, capacity);
      while (size < needed) size *= 2;
      grow(size);
    }
    const lodScale = pixelsPerUnit(camera);
    camera.getWorldPosition(cameraPosition);
    if (eligibilityCursor >= records.size) eligibilityCursor = 0;
    const windowEnd = eligibilityCursor + FIGURE_ELIGIBILITY_CHECKS_PER_FRAME;
    near.length = 0;
    let index = -1;
    for (const record of records.values()) {
      index += 1;
      const avatar = record.avatar;
      if (!avatar.parent) {
        release(avatar);
        continue;
      }
      if (!objectIsEffectivelyVisible(avatar)) {
        record.desired = "hidden";
        continue;
      }
      const shape = avatar.children.length * 1024 +
        (record.highDetail?.children.length || 0);
      if (
        !record.checked ||
        record.shape !== shape ||
        (index >= eligibilityCursor && index < windowEnd)
      ) {
        record.shape = shape;
        record.refresh = true;
        checkEligibility(record);
      }
      if (record.slot >= capacity) {
        record.desired = "full";
        continue;
      }
      const desired = desiredState(record, lodScale, cameraPosition);
      if (desired === "near") near.push(record);
      record.desired = desired === "forced" ? "near" : desired;
    }
    eligibilityCursor += FIGURE_ELIGIBILITY_CHECKS_PER_FRAME;
    if (near.length > FIGURE_NEAR_DETAIL_LIMIT) {
      near.sort(byPixels);
      for (let index = FIGURE_NEAR_DETAIL_LIMIT; index < near.length; index += 1) {
        near[index].desired = "high";
      }
    }
    const uploadStartedAt = performance.now();
    for (let index = 0; index < near.length && index < FIGURE_NEAR_DETAIL_LIMIT; index += 1) {
      const record = near[index];
      if (record.state === "near" || record.desired !== "near") continue;
      if (nearTexturesReady(record, uploadStartedAt)) continue;
      record.desired = "high";
    }
    copyBudget = gpuBusy() ? 0 : frameCopyBudget();
    counters.full = 0;
    counters.near = 0;
    counters.high = 0;
    counters.hidden = 0;
    counters.waiting = 0;
    counters.pendingCopies = 0;
    counters.ineligible = 0;
    for (const record of records.values()) {
      if (!record.eligible) counters.ineligible += 1;
      let desired = record.desired;
      if (batchedState(desired) && !queueCopies(record)) {
        desired = desired === "high" ? "waiting" : "full";
        counters.pendingCopies += 1;
      }
      record.nextState = desired;
    }
    if (capacity) flushCopies();
    for (const record of records.values()) {
      if (record.slot >= capacity) continue;
      const next = record.nextState;
      const changed = next !== record.state;
      if (changed) boundsDirty = true;
      record.state = next;
      const moved = avatarMoved(record);
      if (moved && batchedState(next)) boundsDirty = true;
      writeRecord(record, changed, moved);
      counters[next] += 1;
    }
    hideBatched();
    if (boundsDirty) {
      updateBounds();
      boundsDirty = false;
    }
    for (const batch of batches) {
      const mesh = batch.mesh;
      if (!mesh) continue;
      mesh.count = batch.members.length;
      mesh.visible = mesh.count > 0;
      if (batch.to >= batch.from) {
        const count = batch.to - batch.from + 1;
        const matrices = mesh.instanceMatrix;
        matrices.clearUpdateRanges();
        matrices.addUpdateRange(batch.from * 16, count * 16);
        matrices.needsUpdate = true;
        batch.layerIds.clearUpdateRanges();
        batch.layerIds.addUpdateRange(batch.from * 3, count * 3);
        batch.layerIds.needsUpdate = true;
        batch.from = Infinity;
        batch.to = -1;
      }
    }
    syncMs += (performance.now() - startedAt - syncMs) * 0.05;
  }

  function reserve(count) {
    reserved = Math.max(0, Math.floor(Number(count) || 0));
  }

  function hideBatched() {
    for (const record of records.values()) {
      if (record.avatar.visible !== true) continue;
      if (record.state === "near") {
        for (const index of NEAR_PARTS) {
          const node = record.nodes[index];
          if ((record.indices[index] < 0) || node.layers.mask === 0) continue;
          muted.push(node, node.layers.mask);
          node.layers.mask = 0;
        }
        continue;
      }
      if (record.state !== "high" && record.state !== "waiting") continue;
      record.avatar.visible = false;
      hidden.push(record.avatar);
    }
  }

  function restore() {
    for (let index = 0; index < hidden.length; index += 1) {
      hidden[index].visible = true;
    }
    hidden.length = 0;
    for (let index = 0; index < muted.length; index += 2) {
      muted[index].layers.mask = muted[index + 1];
    }
    muted.length = 0;
  }

  function stats() {
    let draws = 0;
    for (const batch of batches) {
      if (batch.mesh?.visible) draws += 1;
    }
    return {
      figures: records.size,
      capacity,
      full: counters.full,
      near: counters.near,
      batched: counters.high,
      hidden: counters.hidden,
      waiting: counters.waiting,
      ineligible: counters.ineligible,
      pendingCopies: counters.pendingCopies,
      layerCopies: counters.copies,
      syncMs: Math.round(syncMs * 1000) / 1000,
      draws,
    };
  }

  function setDetailThresholds({ near } = {}) {
    thresholds.near = Number.isFinite(near) ? near : FIGURE_NEAR_DETAIL_PIXELS_PER_UNIT;
  }

  function stateOf(avatar) {
    return records.get(avatar)?.state || "";
  }

  function slotOf(avatar) {
    return records.get(avatar)?.slot ?? -1;
  }

  function dispose() {
    restore();
    for (const batch of batches) {
      if (!batch.mesh) continue;
      parent.remove(batch.mesh);
      batch.mesh.dispose();
      batch.mesh = null;
    }
    for (const kind of LAYER_NAMES) layers[kind].texture?.dispose();
    Object.values(materials).forEach((material) => material.dispose());
    records.clear();
  }

  return {
    register,
    release,
    reserve,
    sync,
    restore,
    stats,
    stateOf,
    slotOf,
    setDetailThresholds,
    dispose,
  };
}
