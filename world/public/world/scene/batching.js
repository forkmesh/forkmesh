import { BATCH_TEXTURE_SLOTS } from "./avatar-controls.js";
import * as resources from "./resources.js";
import { markSharedResource } from "../world-resource-cache.js";

const sharedBatchMaterials = new WeakMap();

function sharedBatchMaterial(THREE, key, source) {
  let cache = sharedBatchMaterials.get(THREE);
  if (!cache) {
    cache = new Map();
    sharedBatchMaterials.set(THREE, cache);
  }
  let material = cache.get(key);
  if (!material) {
    material = markSharedResource(createBatchMaterial(THREE, source));
    cache.set(key, material);
  }
  return material;
}

function batchRound(value, step) {
  return Math.round((Number(value) || 0) / step) * step;
}

function batchBakeable(THREE, material) {
  if (
    !material?.isMeshStandardMaterial &&
    !material?.isMeshLambertMaterial &&
    !material?.isMeshBasicMaterial
  ) {
    return false;
  }
  if (material.vertexColors || material.wireframe) return false;
  if (material.onBeforeCompile !== THREE.Material.prototype.onBeforeCompile) {
    return false;
  }
  if (BATCH_TEXTURE_SLOTS.some((slot) => material[slot])) return false;
  if (
    material.isMeshPhysicalMaterial &&
    (material.transmission > 0 ||
      material.sheen > 0 ||
      material.iridescence > 0)
  ) {
    return false;
  }
  return true;
}

function batchShading(material) {
  if (material.isMeshStandardMaterial) return "lit";
  if (material.isMeshLambertMaterial) return "lambert";
  return "basic";
}

function batchMaterialKey(THREE, material, object) {
  if (!batchBakeable(THREE, material)) return `own:${material.uuid}`;
  return [
    batchShading(material),
    material.transparent,
    material.side,
    material.depthWrite,
    material.depthTest,
    material.colorWrite,
    material.blending,
    batchRound(material.alphaTest, 0.01),
    material.fog,
    material.toneMapped,
    material.flatShading,
    material.polygonOffset,
    material.polygonOffsetFactor,
    material.polygonOffsetUnits,
    material.forceSinglePass,
    batchRound(material.envMapIntensity ?? 1, 0.25),
    batchRound(material.clearcoat, 0.25),
    batchRound(material.clearcoatRoughness, 0.25),
    material.transparent ? object.renderOrder : 0,
  ].join("|");
}

const BATCH_ATLAS_PADDING = 4;
const BATCH_ATLAS_MAX_SIZE = 4096;
const BATCH_ATLAS_MAX_VERTICES = 256;
const BATCH_ATLAS_MAX_SOURCES = 64;

function batchAtlasDrawable(image) {
  if (!image || !(image.width > 0) || !(image.height > 0)) return false;
  if (typeof image.getContext === "function") return true;
  return typeof ImageBitmap !== "undefined" && image instanceof ImageBitmap;
}

function batchAtlasKey(THREE, material, object) {
  if (typeof document === "undefined") return "";
  if (!material?.isMeshBasicMaterial || object.isInstancedMesh) return "";
  if (material.vertexColors || material.wireframe) return "";
  if (material.onBeforeCompile !== THREE.Material.prototype.onBeforeCompile) {
    return "";
  }
  const map = material.map;
  if (!map?.isTexture || !batchAtlasDrawable(map.image) || map.flipY !== true) {
    return "";
  }
  if (
    map.repeat.x !== 1 ||
    map.repeat.y !== 1 ||
    map.offset.x !== 0 ||
    map.offset.y !== 0 ||
    map.rotation !== 0
  ) {
    return "";
  }
  if (BATCH_TEXTURE_SLOTS.some((slot) => slot !== "map" && material[slot])) {
    return "";
  }
  const geometry = object.geometry;
  const uv = geometry?.attributes?.uv;
  if (!uv || geometry.attributes.position.count > BATCH_ATLAS_MAX_VERTICES) {
    return "";
  }
  for (let index = 0; index < uv.count; index += 1) {
    const u = uv.getX(index);
    const v = uv.getY(index);
    if (u < -1e-4 || u > 1 + 1e-4 || v < -1e-4 || v > 1 + 1e-4) return "";
  }
  return [
    "atlas",
    material.transparent,
    material.side,
    material.depthWrite,
    material.depthTest,
    material.colorWrite,
    material.blending,
    batchRound(material.alphaTest, 0.01),
    material.fog,
    material.toneMapped,
    material.polygonOffset,
    material.polygonOffsetFactor,
    material.polygonOffsetUnits,
    material.forceSinglePass,
    map.colorSpace,
    map.premultiplyAlpha,
    material.transparent ? object.renderOrder : 0,
  ].join("|");
}

function packBatchAtlas(sources, scale) {
  for (let fit = scale; ; fit /= 2) {
    const cells = [...sources].map((texture) => ({
      texture,
      width: Math.max(1, Math.ceil(texture.image.width * fit)),
      height: Math.max(1, Math.ceil(texture.image.height * fit)),
      x: 0,
      y: 0,
    }));
    const widest = Math.max(...cells.map((cell) => cell.width)) + BATCH_ATLAS_PADDING * 2;
    let width = 256;
    while (width < widest) width *= 2;
    const area = cells.reduce(
      (sum, cell) =>
        sum +
        (cell.width + BATCH_ATLAS_PADDING * 2) *
          (cell.height + BATCH_ATLAS_PADDING * 2),
      0,
    );
    while (width * width < area && width < BATCH_ATLAS_MAX_SIZE) width *= 2;
    if (width > BATCH_ATLAS_MAX_SIZE) continue;
    const ordered = cells.slice().sort((a, b) => b.height - a.height);
    let x = 0;
    let y = 0;
    let row = 0;
    for (const cell of ordered) {
      const cellWidth = cell.width + BATCH_ATLAS_PADDING * 2;
      if (x + cellWidth > width) {
        x = 0;
        y += row;
        row = 0;
      }
      cell.x = x + BATCH_ATLAS_PADDING;
      cell.y = y + BATCH_ATLAS_PADDING;
      x += cellWidth;
      row = Math.max(row, cell.height + BATCH_ATLAS_PADDING * 2);
    }
    let height = 1;
    while (height < y + row) height *= 2;
    if (height > BATCH_ATLAS_MAX_SIZE) continue;
    return { width, height, cells };
  }
}

function drawBatchAtlasCell(context, cell) {
  const image = cell.texture.image;
  const { x, y, width, height } = cell;
  const pad = BATCH_ATLAS_PADDING;
  context.clearRect(x - pad, y - pad, width + pad * 2, height + pad * 2);
  context.drawImage(image, x, y, width, height);
  context.drawImage(image, 0, 0, 1, image.height, x - pad, y, pad, height);
  context.drawImage(image, image.width - 1, 0, 1, image.height, x + width, y, pad, height);
  context.drawImage(image, 0, 0, image.width, 1, x, y - pad, width, pad);
  context.drawImage(image, 0, image.height - 1, image.width, 1, x, y + height, width, pad);
  cell.version = cell.texture.version;
}

function createBatchAtlas(THREE, sources, scale) {
  const layout = packBatchAtlas(sources, scale);
  const canvas = document.createElement("canvas");
  canvas.width = layout.width;
  canvas.height = layout.height;
  const context = canvas.getContext("2d");
  layout.cells.forEach((cell) => drawBatchAtlasCell(context, cell));
  const texture = new THREE.CanvasTexture(canvas);
  const first = layout.cells[0].texture;
  texture.colorSpace = first.colorSpace;
  texture.premultiplyAlpha = first.premultiplyAlpha;
  texture.anisotropy = Math.max(...layout.cells.map((cell) => cell.texture.anisotropy || 1));
  texture.name = "forkmesh-merged-batch-atlas";
  return {
    texture,
    context,
    width: layout.width,
    height: layout.height,
    cells: new Map(layout.cells.map((cell) => [cell.texture, cell])),
  };
}

export function refreshBatchAtlas(atlas) {
  if (!atlas) return 0;
  let redrawn = 0;
  for (const cell of atlas.cells.values()) {
    if (cell.version === cell.texture.version) continue;
    drawBatchAtlasCell(atlas.context, cell);
    redrawn += 1;
  }
  if (redrawn) atlas.texture.needsUpdate = true;
  return redrawn;
}

export function disposeBatchAtlas(atlas) {
  if (!atlas) return;
  atlas.texture.dispose();
  atlas.material?.dispose();
  atlas.cells.clear();
}

function createBatchMaterial(THREE, source) {
  const parameters = {
    color: "#ffffff",
    vertexColors: true,
    transparent: source.transparent,
    side: source.side,
    depthWrite: source.depthWrite,
    depthTest: source.depthTest,
    colorWrite: source.colorWrite,
    blending: source.blending,
    alphaTest: source.alphaTest,
    fog: source.fog,
    toneMapped: source.toneMapped,
    polygonOffset: source.polygonOffset,
    polygonOffsetFactor: source.polygonOffsetFactor,
    polygonOffsetUnits: source.polygonOffsetUnits,
    forceSinglePass:
      source.forceSinglePass ||
      (source.transparent && source.side === THREE.DoubleSide),
  };
  if (source.isMeshLambertMaterial) {
    const material = resources.uniqueMaterial(
      THREE,
      { ...parameters, flatShading: source.flatShading },
      "MeshLambertMaterial",
    );
    material.onBeforeCompile = (shader) => {
      shader.vertexShader = shader.vertexShader
        .replace(
          "#include <common>",
          "#include <common>\nattribute vec3 batchEmissive;\nvarying vec3 vBatchEmissive;",
        )
        .replace(
          "#include <begin_vertex>",
          "#include <begin_vertex>\nvBatchEmissive = batchEmissive;",
        );
      shader.fragmentShader = shader.fragmentShader
        .replace(
          "#include <common>",
          "#include <common>\nvarying vec3 vBatchEmissive;",
        )
        .replace(
          "vec3 totalEmissiveRadiance = emissive;",
          "vec3 totalEmissiveRadiance = vBatchEmissive;",
        );
    };
    material.customProgramCacheKey = () => "forkmesh-merged-lambert-batch";
    return material;
  }
  if (!source.isMeshStandardMaterial) {
    return resources.uniqueMaterial(THREE, parameters, "MeshBasicMaterial");
  }
  const clearcoat = batchRound(source.clearcoat, 0.25);
  const lit = {
    ...parameters,
    flatShading: source.flatShading,
    envMapIntensity: batchRound(source.envMapIntensity ?? 1, 0.25),
  };
  const material = clearcoat > 0
    ? resources.uniqueMaterial(THREE, {
        ...lit,
        clearcoat,
        clearcoatRoughness: batchRound(source.clearcoatRoughness, 0.25),
      }, "MeshPhysicalMaterial")
    : resources.uniqueMaterial(THREE, lit, "MeshStandardMaterial");
  material.onBeforeCompile = (shader) => {
    shader.vertexShader = shader.vertexShader
      .replace(
        "#include <common>",
        "#include <common>\nattribute vec2 batchSurface;\nattribute vec3 batchEmissive;\nvarying vec2 vBatchSurface;\nvarying vec3 vBatchEmissive;",
      )
      .replace(
        "#include <begin_vertex>",
        "#include <begin_vertex>\nvBatchSurface = batchSurface;\nvBatchEmissive = batchEmissive;",
      );
    shader.fragmentShader = shader.fragmentShader
      .replace(
        "#include <common>",
        "#include <common>\nvarying vec2 vBatchSurface;\nvarying vec3 vBatchEmissive;",
      )
      .replace(
        "#include <roughnessmap_fragment>",
        "float roughnessFactor = vBatchSurface.x;",
      )
      .replace(
        "#include <metalnessmap_fragment>",
        "float metalnessFactor = vBatchSurface.y;",
      )
      .replace(
        "vec3 totalEmissiveRadiance = emissive;",
        "vec3 totalEmissiveRadiance = vBatchEmissive;",
      );
  };
  material.customProgramCacheKey = () => "forkmesh-merged-batch";
  return material;
}

export function mergeObjectBatches(THREE, root, options = {}) {
  const steps = mergeObjectBatchesSteps(THREE, root, options);
  let step = steps.next();
  while (!step.done) step = steps.next();
  return step.value;
}

export function* mergeObjectBatchesSteps(THREE, root, {
  name = "forkmesh-merged-batches",
  visibility = new Map(),
  materials = new Map(),
  atlasScale = 0,
} = {}) {
  const group = new THREE.Group();
  group.name = name;
  const batches = new Map();
  const textures = new Set();
  const rootInverse = new THREE.Matrix4();
  const relative = new THREE.Matrix4();
  const instanceMatrix = new THREE.Matrix4();
  const normalMatrix = new THREE.Matrix3();
  const vertex = new THREE.Vector3();
  const instanceColor = new THREE.Color();
  root.updateWorldMatrix(true, true);
  rootInverse.copy(root.matrixWorld).invert();

  const visible = (object) =>
    visibility.has(object) ? visibility.get(object) : object.visible;

  const batchFor = (material, object) => {
    let atlasKey = atlasScale > 0 ? batchAtlasKey(THREE, material, object) : "";
    const atlasBatch = atlasKey ? batches.get(atlasKey) : null;
    if (
      atlasBatch &&
      atlasBatch.sources.size >= BATCH_ATLAS_MAX_SOURCES &&
      !atlasBatch.sources.has(material.map)
    ) {
      atlasKey = "";
    }
    const key = atlasKey || batchMaterialKey(THREE, material, object);
    let batch = batches.get(key);
    if (!batch && atlasKey) {
      batch = {
        atlas: true,
        sources: new Set(),
        baked: false,
        material,
        renderOrder: object.renderOrder,
        positions: [],
        normals: [],
        uvs: [],
        colors: [],
        indices: [],
        parts: [],
        vertexCount: 0,
      };
      batches.set(key, batch);
    }
    if (!batch) {
      const baked = !key.startsWith("own:");
      let batchMaterial = material;
      if (baked) {
        batchMaterial = materials.get(key);
        if (!batchMaterial) {
          batchMaterial = sharedBatchMaterial(
            THREE,
            key.slice(0, key.lastIndexOf("|")),
            material,
          );
          materials.set(key, batchMaterial);
        }
      } else {
        BATCH_TEXTURE_SLOTS.forEach((slot) => {
          if (material[slot]?.isTexture) textures.add(material[slot]);
        });
      }
      batch = {
        baked,
        lit: baked && material.isMeshStandardMaterial,
        emissive: baked && !material.isMeshBasicMaterial,
        material: batchMaterial,
        renderOrder: object.renderOrder,
        positions: [],
        normals: [],
        uvs: [],
        colors: [],
        surfaces: [],
        emissives: [],
        indices: [],
        vertexCount: 0,
      };
      batches.set(key, batch);
    }
    return batch;
  };

  const appendPart = (batch, geometry, start, count, matrix, material, tint) => {
    const position = geometry.attributes.position;
    const normal = geometry.attributes.normal;
    const uv = geometry.attributes.uv;
    const index = geometry.index;
    const end = Math.min(start + count, index ? index.count : position.count);
    const remap = new Int32Array(position.count).fill(-1);
    normalMatrix.getNormalMatrix(matrix);
    const flip = matrix.determinant() < 0;
    const color = material.color;
    const emissiveScale = material.emissiveIntensity ?? 0;
    if (batch.atlas) {
      batch.sources.add(material.map);
      batch.parts.push({ texture: material.map, from: batch.uvs.length, to: 0 });
    }
    const place = (source) => {
      if (remap[source] >= 0) return remap[source];
      const target = batch.vertexCount;
      batch.vertexCount += 1;
      remap[source] = target;
      vertex.fromBufferAttribute(position, source).applyMatrix4(matrix);
      batch.positions.push(vertex.x, vertex.y, vertex.z);
      vertex.fromBufferAttribute(normal, source).applyMatrix3(normalMatrix).normalize();
      batch.normals.push(vertex.x, vertex.y, vertex.z);
      if (!batch.baked) {
        batch.uvs.push(uv ? uv.getX(source) : 0, uv ? uv.getY(source) : 0);
        if (batch.atlas) {
          batch.colors.push(
            color.r * tint.r,
            color.g * tint.g,
            color.b * tint.b,
            material.opacity,
          );
        }
        return target;
      }
      batch.colors.push(
        color.r * tint.r,
        color.g * tint.g,
        color.b * tint.b,
        material.opacity,
      );
      if (batch.lit) batch.surfaces.push(material.roughness, material.metalness);
      if (batch.emissive) {
        batch.emissives.push(
          material.emissive.r * emissiveScale,
          material.emissive.g * emissiveScale,
          material.emissive.b * emissiveScale,
        );
      }
      return target;
    };
    for (let corner = start; corner + 2 < end; corner += 3) {
      const a = place(index ? index.getX(corner) : corner);
      const b = place(index ? index.getX(corner + 1) : corner + 1);
      const c = place(index ? index.getX(corner + 2) : corner + 2);
      if (flip) batch.indices.push(a, c, b);
      else batch.indices.push(a, b, c);
    }
    if (batch.atlas) batch.parts[batch.parts.length - 1].to = batch.uvs.length;
  };

  const clones = [];
  const visit = (object) => {
    if (!visible(object)) return false;
    if (object.userData?.instancedMirror === true) return false;
    if (object.isSprite) {
      const sprite = object.clone(false);
      relative.multiplyMatrices(rootInverse, object.matrixWorld);
      relative.decompose(sprite.position, sprite.quaternion, sprite.scale);
      if (sprite.material?.map?.isTexture) textures.add(sprite.material.map);
      clones.push(sprite);
    } else if (object.isMesh && object.geometry?.attributes?.position) {
      const geometry = object.geometry;
      const groups = Array.isArray(object.material)
        ? geometry.groups.map((part) => ({
            start: part.start,
            count: part.count,
            material: object.material[part.materialIndex],
          }))
        : [{ start: 0, count: Infinity, material: object.material }];
      const mergeable =
        !object.isSkinnedMesh &&
        geometry.attributes.normal &&
        !Object.keys(geometry.morphAttributes || {}).length &&
        groups.every(
          ({ material }) =>
            !material ||
            batchBakeable(THREE, material) ||
            (!material.vertexColors &&
              !(object.isInstancedMesh && object.instanceColor)),
        );
      relative.multiplyMatrices(rootInverse, object.matrixWorld);
      if (!mergeable) {
        const copy = object.clone(false);
        relative.decompose(copy.position, copy.quaternion, copy.scale);
        copy.castShadow = false;
        copy.receiveShadow = false;
        groups.forEach(({ material }) => {
          BATCH_TEXTURE_SLOTS.forEach((slot) => {
            if (material?.[slot]?.isTexture) textures.add(material[slot]);
          });
        });
        clones.push(copy);
      } else {
        const instances = object.isInstancedMesh ? object.count : 1;
        for (let instance = 0; instance < instances; instance += 1) {
          const matrix = relative.clone();
          instanceColor.setRGB(1, 1, 1);
          if (object.isInstancedMesh) {
            object.getMatrixAt(instance, instanceMatrix);
            matrix.multiply(instanceMatrix);
            if (object.instanceColor) object.getColorAt(instance, instanceColor);
          }
          groups.forEach(({ start, count, material }) => {
            if (!material || material.visible === false) return;
            if (material.transparent && !(material.opacity > 0)) return;
            appendPart(
              batchFor(material, object),
              geometry,
              start,
              count,
              matrix,
              material,
              instanceColor,
            );
          });
        }
      }
    }
    return true;
  };
  const stack = root.children.slice().reverse();
  while (stack.length) {
    const object = stack.pop();
    if (!visit(object)) continue;
    for (let index = object.children.length - 1; index >= 0; index -= 1) {
      stack.push(object.children[index]);
    }
    yield;
  }

  yield;
  let draws = 0;
  const atlases = [];
  for (const batch of batches.values()) {
    if (!batch.atlas || !batch.indices.length) continue;
    const atlas = createBatchAtlas(THREE, batch.sources, atlasScale);
    for (const part of batch.parts) {
      const cell = atlas.cells.get(part.texture);
      for (let index = part.from; index < part.to; index += 2) {
        batch.uvs[index] = (cell.x + batch.uvs[index] * cell.width) / atlas.width;
        batch.uvs[index + 1] =
          1 - (cell.y + (1 - batch.uvs[index + 1]) * cell.height) / atlas.height;
      }
    }
    const source = batch.material;
    atlas.material = resources.uniqueMaterial(THREE, {
      color: "#ffffff",
      map: atlas.texture,
      vertexColors: true,
      transparent: source.transparent,
      side: source.side,
      depthWrite: source.depthWrite,
      depthTest: source.depthTest,
      colorWrite: source.colorWrite,
      blending: source.blending,
      alphaTest: source.alphaTest,
      fog: source.fog,
      toneMapped: source.toneMapped,
      polygonOffset: source.polygonOffset,
      polygonOffsetFactor: source.polygonOffsetFactor,
      polygonOffsetUnits: source.polygonOffsetUnits,
      forceSinglePass:
        source.forceSinglePass ||
        (source.transparent && source.side === THREE.DoubleSide),
    }, "MeshBasicMaterial");
    batch.material = atlas.material;
    batch.atlasState = atlas;
    atlases.push(atlas);
    yield;
  }
  for (const [key, batch] of batches) {
    if (!batch.indices.length) continue;
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute(
      "position",
      new THREE.Float32BufferAttribute(batch.positions, 3),
    );
    geometry.setAttribute(
      "normal",
      new THREE.Float32BufferAttribute(batch.normals, 3),
    );
    if (batch.baked) {
      geometry.setAttribute(
        "color",
        new THREE.Float32BufferAttribute(batch.colors, 4),
      );
      if (batch.lit) {
        geometry.setAttribute(
          "batchSurface",
          new THREE.Float32BufferAttribute(batch.surfaces, 2),
        );
      }
      if (batch.emissive) {
        geometry.setAttribute(
          "batchEmissive",
          new THREE.Float32BufferAttribute(batch.emissives, 3),
        );
      }
    } else {
      geometry.setAttribute("uv", new THREE.Float32BufferAttribute(batch.uvs, 2));
      if (batch.atlasState) {
        geometry.setAttribute(
          "color",
          new THREE.Float32BufferAttribute(batch.colors, 4),
        );
      }
    }
    geometry.setIndex(batch.indices);
    geometry.computeBoundingSphere();
    geometry.computeBoundingBox();
    const mesh = new THREE.Mesh(geometry, batch.material);
    mesh.name = `${name}-batch-${draws + 1}`;
    mesh.renderOrder = batch.renderOrder;
    mesh.castShadow = false;
    mesh.receiveShadow = false;
    mesh.userData.mergedBatchKey = key;
    if (batch.atlasState) mesh.userData.batchAtlas = batch.atlasState;
    group.add(mesh);
    draws += 1;
    yield;
  }
  clones.forEach((copy) => group.add(copy));
  group.userData.drawCount = draws + clones.length;
  group.userData.sharedTextures = textures;
  group.userData.atlases = atlases;
  return group;
}

export function mergeMeshesByMaterial(THREE, root, { name = root.name || "forkmesh-merged", skip = () => false } = {}) {
  root.updateWorldMatrix(true, true);
  const rootInverse = new THREE.Matrix4().copy(root.matrixWorld).invert();
  const relative = new THREE.Matrix4();
  const normalMatrix = new THREE.Matrix3();
  const vertex = new THREE.Vector3();
  const groups = new Map();
  root.traverse((object) => {
    if (object === root || !object.isMesh || object.isInstancedMesh || object.isSkinnedMesh) return;
    if (Array.isArray(object.material) || !object.material || object.visible === false) return;
    if (skip(object)) return;
    const geometry = object.geometry;
    if (!geometry?.attributes?.position || !geometry.attributes.normal) return;
    if (Object.keys(geometry.morphAttributes || {}).length) return;
    const uv = Boolean(geometry.attributes.uv);
    const key = `${object.material.id}|${uv}|${object.renderOrder}|${object.castShadow}|${object.receiveShadow}`;
    let list = groups.get(key);
    if (!list) groups.set(key, (list = []));
    list.push(object);
  });
  let merged = 0;
  groups.forEach((list) => {
    if (list.length < 2) return;
    const positions = [];
    const normals = [];
    const uvs = [];
    const indices = [];
    let base = 0;
    for (const part of list) {
      const geometry = part.geometry;
      const position = geometry.attributes.position;
      const normal = geometry.attributes.normal;
      const uv = geometry.attributes.uv;
      relative.multiplyMatrices(rootInverse, part.matrixWorld);
      normalMatrix.getNormalMatrix(relative);
      for (let index = 0; index < position.count; index += 1) {
        vertex.fromBufferAttribute(position, index).applyMatrix4(relative);
        positions.push(vertex.x, vertex.y, vertex.z);
        vertex.fromBufferAttribute(normal, index).applyMatrix3(normalMatrix).normalize();
        normals.push(vertex.x, vertex.y, vertex.z);
        if (uv) uvs.push(uv.getX(index), uv.getY(index));
      }
      const flip = relative.determinant() < 0;
      const source = geometry.index;
      const count = source ? source.count : position.count;
      for (let corner = 0; corner + 2 < count; corner += 3) {
        const a = base + (source ? source.getX(corner) : corner);
        const b = base + (source ? source.getX(corner + 1) : corner + 1);
        const c = base + (source ? source.getX(corner + 2) : corner + 2);
        if (flip) indices.push(a, c, b);
        else indices.push(a, b, c);
      }
      base += position.count;
    }
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute("position", new THREE.Float32BufferAttribute(positions, 3));
    geometry.setAttribute("normal", new THREE.Float32BufferAttribute(normals, 3));
    if (uvs.length) geometry.setAttribute("uv", new THREE.Float32BufferAttribute(uvs, 2));
    geometry.setIndex(indices);
    geometry.computeBoundingSphere();
    geometry.computeBoundingBox();
    const first = list[0];
    const mesh = new THREE.Mesh(geometry, first.material);
    merged += 1;
    mesh.name = `${name}-merged-${merged}`;
    mesh.renderOrder = first.renderOrder;
    mesh.castShadow = first.castShadow;
    mesh.receiveShadow = first.receiveShadow;
    mesh.userData.mergedMeshCount = list.length;
    for (const part of list) part.parent?.remove(part);
    root.add(mesh);
  });
  return merged;
}
