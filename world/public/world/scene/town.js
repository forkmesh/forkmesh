import { LANDMARKS } from "../world-data.js";
import { OFFICE_BRIDGE_WIDTH } from "../world-office-tower.js";
import { hashNumber } from "../world-avatar-face.js";
import { canvasTexture, roundedRect } from "./canvas.js";
import { embossPlankText } from "./labels.js";
import {
  ARRIVAL_GRID_BOUNDS,
  CONTINUOUS_CITY_CENTER_Z,
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  CONTINUOUS_CITY_SURFACE_Y,
  DISTRICT_GROUND_RADIUS,
  LEADERBOARD_CONNECTION_MAX_X,
  MAINLAND_DISTANT_TRIANGLE_BUDGET,
  MEMBER_PATH_END_Z,
  REPOSITORY_CONNECTION_MIN_X,
  SERVER_CABINET_YARD_ORIGIN,
  TREES_PER_LANDMARK,
  TREE_MIN_SPACING,
  WORLD_PATH_CENTER_Y,
  WORLD_PATH_HEIGHT,
  WORLD_HEX_TILES,
  WORLD_PATH_WIDTH,
  deterministicFraction,
  pointInsideBounds,
} from "./layout.js";
import { makeSharedMaterial } from "./materials.js";
import { roundedHexagonOutline } from "./movement.js";
import { setShadows, triangleWeight } from "./object-utils.js";
import * as resources from "./resources.js";

function deterministicTreeLayout() {
  const positions = [];
  const cabinetBounds = {
    minX: SERVER_CABINET_YARD_ORIGIN[0] + 6,
    maxX: SERVER_CABINET_YARD_ORIGIN[0] + 34,
    minZ: SERVER_CABINET_YARD_ORIGIN[2] - 14,
    maxZ: SERVER_CABINET_YARD_ORIGIN[2] + 14,
  };
  LANDMARKS.forEach((landmark) => {
    // The campfire's clearing is filled by its bench circle, whose radius
    // reaches past where these trees would stand.
    if (["campfire", "office"].includes(landmark.id)) return;
    for (let treeIndex = 0; treeIndex < TREES_PER_LANDMARK; treeIndex += 1) {
      const angle =
        deterministicFraction(`tree-angle:${landmark.id}:${treeIndex}`) * Math.PI * 2;
      const radius =
        6.4 + deterministicFraction(`tree-radius:${landmark.id}:${treeIndex}`) * 2.1;
      const x = landmark.position[0] + Math.cos(angle) * radius;
      const z = landmark.position[2] + Math.sin(angle) * radius;
    if (
      pointInsideBounds(x, z, ARRIVAL_GRID_BOUNDS) ||
      pointInsideBounds(x, z, cabinetBounds)
    ) {
        continue;
    }
      if (Math.hypot(x - 8, z - 8) < 5.5) continue;
    if (
      positions.some(
        (tree) => Math.hypot(x - tree.x, z - tree.z) < TREE_MIN_SPACING,
      )
    ) {
        continue;
    }
      positions.push({
        x,
        z,
        scale:
          0.7 + deterministicFraction(`tree-scale:${landmark.id}:${treeIndex}`) * 0.38,
        colorIndex: hashNumber(`tree-color:${landmark.id}:${treeIndex}`) % 4,
      });
    }
  });
  return positions;
}

function createTownTreeInstances(THREE, layout, colors) {
  const group = new THREE.Group();
  group.name = "forkmesh-town-tree-instances";
  const trees = Array.isArray(layout) ? layout : [];
  const trunk = new THREE.InstancedMesh(
    resources.primitive(THREE, "CylinderGeometry", 0.12, 0.18, 1.25, 7),
    makeSharedMaterial(THREE, "#694d34", { roughness: 1 }),
    trees.length,
  );
  trunk.name = "forkmesh-town-tree-trunks";
  group.add(trunk);
  const crown = new THREE.InstancedMesh(
    resources.primitive(THREE, "IcosahedronGeometry", 0.68, 1),
    makeSharedMaterial(THREE, "#ffffff", { roughness: 0.88 }),
    trees.length,
  );
  crown.name = "forkmesh-town-tree-crowns";
  group.add(crown);

  // Static transforms and per-instance crown colours collapse the entire
  // procedural woodland from two draw calls per tree to two calls total.
  // Geometry stays at unit scale so all copies share the same GPU buffers.
  const transform = new THREE.Object3D();
  const color = new THREE.Color();
  trees.forEach((tree, index) => {
    const scale = Math.max(0.01, Number(tree.scale) || 1);
    transform.position.set(tree.x, 0.62 * scale, tree.z);
    transform.scale.setScalar(scale);
    transform.updateMatrix();
    trunk.setMatrixAt(index, transform.matrix);

    transform.position.set(tree.x, 1.62 * scale, tree.z);
    transform.scale.set(scale, scale * 1.35, scale);
    transform.updateMatrix();
    crown.setMatrixAt(index, transform.matrix);
    crown.setColorAt(
      index,
      color.set(colors[tree.colorIndex % colors.length]),
    );
  });
  trunk.instanceMatrix.needsUpdate = true;
  crown.instanceMatrix.needsUpdate = true;
  if (crown.instanceColor) crown.instanceColor.needsUpdate = true;
  trunk.computeBoundingBox?.();
  trunk.computeBoundingSphere?.();
  crown.computeBoundingBox?.();
  crown.computeBoundingSphere?.();
  setShadows(group);
  return group;
}

export function createLandscapePath(
  THREE,
  name,
  start,
  end,
  width,
  material,
) {
  const dx = end[0] - start[0];
  const dz = end[1] - start[1];
  const length = Math.max(0.1, Math.hypot(dx, dz));
  const path = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", width, WORLD_PATH_HEIGHT, length),
    material,
  );
  path.name = name;
  path.position.set(
    (start[0] + end[0]) / 2,
    WORLD_PATH_CENTER_Y,
    (start[1] + end[1]) / 2,
  );
  path.rotation.y = Math.atan2(dx, dz);
  path.receiveShadow = true;
  path.userData.ground = true;
  return path;
}

export function roundedHexagonGeometry(
  THREE,
  apothem,
  cornerRadius,
  cornerSegments,
  { centers = [[0, 0]], uvScale = 0.5 / apothem } = {},
) {
  const outline = roundedHexagonOutline(apothem, cornerRadius, cornerSegments);
  const ring = outline.length;
  const positions = [];
  const normals = [];
  const uvs = [];
  const indices = [];
  centers.forEach(([centerX, centerY]) => {
    const base = positions.length / 3;
    [[0, 0], ...outline].forEach(([x, y]) => {
      positions.push(centerX + x, centerY + y, 0);
      normals.push(0, 0, 1);
      uvs.push(0.5 + (centerX + x) * uvScale, 0.5 + (centerY + y) * uvScale);
    });
    for (let index = 1; index <= ring; index += 1) {
      indices.push(base, base + index, base + (index % ring) + 1);
    }
  });
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute(
    "position",
    new THREE.Float32BufferAttribute(positions, 3),
  );
  geometry.setAttribute("normal", new THREE.Float32BufferAttribute(normals, 3));
  geometry.setAttribute("uv", new THREE.Float32BufferAttribute(uvs, 2));
  geometry.setIndex(indices);
  geometry.computeBoundingSphere();
  return geometry;
}

export const DESERT_SAND_TILE = 42;
const desertSandTextures = new WeakMap();
export function desertSandTexture(THREE) {
  if (desertSandTextures.has(THREE)) return desertSandTextures.get(THREE);
  const size = CITY_GRASS_PATTERN_SIZE;
  const pixels = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const offset = (y * size + x) * 4;
      const ripple = Math.sin(((x + y * 0.35) / size) * Math.PI * 8) * 5;
      const patch = Math.round((cityGrassPatchValue(x, y) - 127.5) / 11);
      const grain = (cityGrassPatternNoise(x, y, 3) & 7) - 4;
      const shade = Math.round(patch + grain + ripple);
      pixels[offset] = 222 + shade;
      pixels[offset + 1] = 188 + shade;
      pixels[offset + 2] = 128 + Math.round(shade * 0.7);
      pixels[offset + 3] = 255;
    }
  }
  const texture = new THREE.DataTexture(pixels, size, size, THREE.RGBAFormat);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.generateMipmaps = true;
  texture.needsUpdate = true;
  desertSandTextures.set(THREE, texture);
  return texture;
}

export function startWorldSignTexture(THREE) {
  return canvasTexture(THREE, 1024, 512, (context) => {
    context.fillStyle = "#9a6a3c";
    context.fillRect(0, 0, 1024, 512);
    context.strokeStyle = "rgba(70,40,18,0.35)";
    context.lineWidth = 3;
    for (let grain = 34; grain < 512; grain += 46) {
      context.beginPath();
      context.moveTo(0, grain);
      context.bezierCurveTo(300, grain - 10, 700, grain + 12, 1024, grain - 4);
      context.stroke();
    }
    context.strokeStyle = "#4a2c14";
    context.lineWidth = 18;
    context.strokeRect(9, 9, 1006, 494);
    context.textAlign = "center";
    context.textBaseline = "middle";
    embossPlankText(
      context,
      "START YOUR",
      512,
      118,
      '900 96px "ForkMesh Favorit", system-ui, sans-serif',
      "#ffd98a",
    );
    embossPlankText(
      context,
      "WORLD HERE",
      512,
      226,
      '900 96px "ForkMesh Favorit", system-ui, sans-serif',
      "#ffd98a",
    );
    embossPlankText(
      context,
      "YOUR OWN INSTANCE · YOUR NAME",
      512,
      330,
      '800 40px "ForkMesh Mono", ui-monospace, monospace',
      "#ffe9b8",
    );
    context.fillStyle = "#2f6f3a";
    roundedRect(context, 262, 388, 500, 76, 14);
    context.fill();
    context.fillStyle = "#f4ffe9";
    context.font = '900 36px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("CLICK TO START", 512, 427);
  });
}

const projectAssetTextures = new WeakMap();
export function projectAssetTexture(
  THREE,
  path,
  repeatX = 1,
  repeatY = 1,
) {
  let cache = projectAssetTextures.get(THREE);
  if (!cache) {
    cache = new Map();
    projectAssetTextures.set(THREE, cache);
  }
  const key = `${path}|${repeatX}|${repeatY}`;
  if (cache.has(key)) return cache.get(key);
  let texture;
  if (typeof createImageBitmap === "function" && THREE.ImageBitmapLoader) {
    texture = new THREE.Texture();
    texture.flipY = false;
    const loader = new THREE.ImageBitmapLoader();
    loader.setOptions({ imageOrientation: "flipY" });
    loader.load(path, (bitmap) => {
      texture.image = bitmap;
      texture.needsUpdate = true;
    });
  } else {
    texture = new THREE.TextureLoader().load(path);
  }
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.repeat.set(repeatX, repeatY);
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.magFilter = THREE.LinearFilter;
  cache.set(key, texture);
  return texture;
}

// A tiny deterministic tile keeps the city lawn legible without making every
// visitor fetch and decode a photographic 512px texture.  It is generated once
// per Three runtime, then repeated by the same single city-ground material.
// The coarser noise makes soft patches while the finer noise still reads as
// grass when the camera is near the ground.
const CITY_GRASS_PATTERN_SIZE = 64;
const CITY_GRASS_PATCH_SIZE = 8;
const CITY_GRASS_PATTERN_REPEAT_X = 18;
const CITY_GRASS_PATTERN_REPEAT_Y = 24;
const cityGrassTextures = new WeakMap();

function cityGrassPatternNoise(x, y, salt) {
  let value =
    Math.imul(x + salt * 17, 0x1f123bb5) ^
    Math.imul(y + salt * 29, 0x5f356495);
  value = Math.imul(value ^ (value >>> 15), 0x2c1b3c6d);
  return (value ^ (value >>> 12)) >>> 0;
}

function cityGrassPatchValue(x, y) {
  const cellCount = CITY_GRASS_PATTERN_SIZE / CITY_GRASS_PATCH_SIZE;
  const cellX = Math.floor(x / CITY_GRASS_PATCH_SIZE);
  const cellY = Math.floor(y / CITY_GRASS_PATCH_SIZE);
  const localX = (x % CITY_GRASS_PATCH_SIZE) / CITY_GRASS_PATCH_SIZE;
  const localY = (y % CITY_GRASS_PATCH_SIZE) / CITY_GRASS_PATCH_SIZE;
  const smoothX = localX * localX * (3 - 2 * localX);
  const smoothY = localY * localY * (3 - 2 * localY);
  const topLeft = cityGrassPatternNoise(cellX, cellY, 1) & 255;
  const topRight = cityGrassPatternNoise((cellX + 1) % cellCount, cellY, 1) &
    255;
  const bottomLeft = cityGrassPatternNoise(cellX, (cellY + 1) % cellCount, 1) &
    255;
  const bottomRight =
    cityGrassPatternNoise((cellX + 1) % cellCount, (cellY + 1) % cellCount, 1) &
    255;
  const top = topLeft + (topRight - topLeft) * smoothX;
  const bottom = bottomLeft + (bottomRight - bottomLeft) * smoothX;
  return top + (bottom - top) * smoothY;
}

function cityGrassTexture(THREE) {
  if (cityGrassTextures.has(THREE)) return cityGrassTextures.get(THREE);
  const size = CITY_GRASS_PATTERN_SIZE;
  const pixels = new Uint8Array(size * size * 4);
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const offset = (y * size + x) * 4;
      const patch = Math.round((cityGrassPatchValue(x, y) - 127.5) / 9);
      const blade = (cityGrassPatternNoise(x, y, 2) & 15) - 8;
      const shade = patch + blade;
      pixels[offset] = 66 + shade;
      pixels[offset + 1] = 114 + shade;
      pixels[offset + 2] = 48 + Math.round(shade * 0.55);
      pixels[offset + 3] = 255;
    }
  }
  const texture = new THREE.DataTexture(
    pixels,
    size,
    size,
    THREE.RGBAFormat,
  );
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.repeat.set(
    CITY_GRASS_PATTERN_REPEAT_X,
    CITY_GRASS_PATTERN_REPEAT_Y,
  );
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.generateMipmaps = true;
  texture.needsUpdate = true;
  cityGrassTextures.set(THREE, texture);
  return texture;
}

export function cityGrassMaterial(THREE) {
  return resources.uniqueMaterial(THREE, {
    color: "#aebf9c",
    map: cityGrassTexture(THREE),
    roughness: 1,
    metalness: 0,
  }, "MeshStandardMaterial");
}

export const GROUND_LOGO_TEXTURE_SIZE = 256;
export const GROUND_LOGO_VIEW = 220;
export const GROUND_LOGO_APOTHEM = CONTINUOUS_CITY_RADIUS * 0.8;
export const GROUND_LOGO_STRENGTH = 0.14;
export const GROUND_LOGO_FADE_START_Y = 24;
export const GROUND_LOGO_FADE_FULL_Y = 190;

const MARK_APOTHEM = 86.6;
const DARK = "#131316";
const logoTextures = new WeakMap();

function paintLogoMark(context, size) {
  const scale = size / GROUND_LOGO_VIEW;
  context.setTransform(1, 0, 0, 1, 0, 0);
  context.fillStyle = "#808080";
  context.fillRect(0, 0, size, size);
  const base = () => context.setTransform(scale, 0, 0, scale, size / 2, size / 2);
  base();
  context.lineJoin = "round";
  context.lineCap = "round";
  context.fillStyle = DARK;
  context.strokeStyle = DARK;
  context.lineWidth = 10;
  context.fill(new Path2D("M0 -100 L86.6 -50 L86.6 50 L0 100 L-86.6 50 L-86.6 -50 Z"));
  context.stroke(new Path2D("M0 -100 L86.6 -50 L86.6 50 L0 100 L-86.6 50 L-86.6 -50 Z"));
  base();
  context.transform(0.866, -0.5, 0.866, 0.5, -86.6, -50);
  context.fillStyle = "#f2f2f4";
  context.beginPath();
  context.roundRect(5, 5, 90, 90, 7);
  context.fill();
  context.strokeStyle = DARK;
  context.fillStyle = DARK;
  context.lineWidth = 7;
  context.stroke(new Path2D("M78 22 L22 78 M40 20 L40 60 M60 80 L60 40"));
  [[78, 22], [22, 78], [40, 20], [60, 80]].forEach(([x, y]) => {
    context.beginPath();
    context.arc(x, y, 7.5, 0, Math.PI * 2);
    context.fill();
  });
  context.lineWidth = 3;
  [
    [[0.866, 0.5, 0, 1, -86.6, -50], "#d4d4da", "M7 9 H93 V31 H31 V45 H76 V65 H31 V93 H7 Z"],
    [[0.866, -0.5, 0, 1, 0, 0], "#a1a1ab", "M7 93 V9 H28 L50 42 L72 9 H93 V93 H72 V44 L50 77 L28 44 V93 Z"],
  ].forEach(([matrix, color, path]) => {
    base();
    context.transform(...matrix);
    context.fillStyle = color;
    context.strokeStyle = color;
    const shape = new Path2D(path);
    context.fill(shape);
    context.stroke(shape);
  });
  context.setTransform(1, 0, 0, 1, 0, 0);
}

export function groundLogoTexture(THREE) {
  if (logoTextures.has(THREE)) return logoTextures.get(THREE);
  if (typeof document === "undefined") return null;
  const canvas = document.createElement("canvas");
  canvas.width = GROUND_LOGO_TEXTURE_SIZE;
  canvas.height = GROUND_LOGO_TEXTURE_SIZE;
  const context = canvas.getContext("2d");
  if (!context || typeof Path2D === "undefined") return null;
  paintLogoMark(context, GROUND_LOGO_TEXTURE_SIZE);
  const texture = new THREE.CanvasTexture(canvas);
  texture.name = "forkmesh-ground-logo";
  texture.colorSpace = THREE.NoColorSpace;
  texture.flipY = false;
  texture.wrapS = THREE.ClampToEdgeWrapping;
  texture.wrapT = THREE.ClampToEdgeWrapping;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.generateMipmaps = true;
  logoTextures.set(THREE, texture);
  return texture;
}

export function applyGroundLogo(THREE, material, tile = WORLD_HEX_TILES[0]) {
  const texture = groundLogoTexture(THREE);
  if (!texture) return material;
  const worldPerMark = GROUND_LOGO_APOTHEM / MARK_APOTHEM;
  const scale = 1 / (GROUND_LOGO_VIEW * worldPerMark);
  material.userData.groundLogo = {
    apothem: GROUND_LOGO_APOTHEM,
    center: [tile.x, tile.z],
    strength: GROUND_LOGO_STRENGTH,
    fade: [GROUND_LOGO_FADE_START_Y, GROUND_LOGO_FADE_FULL_Y],
  };
  material.onBeforeCompile = (shader) => {
    shader.uniforms.groundLogoMap = { value: texture };
    shader.uniforms.groundLogoCenter = { value: new THREE.Vector2(tile.x, tile.z) };
    shader.uniforms.groundLogoScale = { value: scale };
    shader.vertexShader = `varying vec2 vGroundLogoPosition;\n${shader.vertexShader}`
      .replace(
        "#include <project_vertex>",
        "#include <project_vertex>\nvGroundLogoPosition = (modelMatrix * vec4(transformed, 1.0)).xz;",
      );
    shader.fragmentShader = [
      "uniform sampler2D groundLogoMap;",
      "uniform vec2 groundLogoCenter;",
      "uniform float groundLogoScale;",
      "varying vec2 vGroundLogoPosition;",
      shader.fragmentShader,
    ].join("\n").replace(
      "#include <map_fragment>",
      [
        "#include <map_fragment>",
        `float groundLogoFade = ${GROUND_LOGO_STRENGTH.toFixed(4)} * smoothstep(${GROUND_LOGO_FADE_START_Y.toFixed(1)}, ${GROUND_LOGO_FADE_FULL_Y.toFixed(1)}, cameraPosition.y);`,
        "vec2 groundLogoUv = vec2(0.5) + (vGroundLogoPosition - groundLogoCenter) * groundLogoScale;",
        "float groundLogoTone = texture2D(groundLogoMap, groundLogoUv).r * 2.0 - 1.0;",
        "diffuseColor.rgb *= 1.0 + groundLogoFade * groundLogoTone;",
      ].join("\n"),
    );
  };
  material.customProgramCacheKey = () => "forkmesh-ground-logo";
  material.needsUpdate = true;
  return material;
}

const concreteBrickTextures = new WeakMap();
function concreteBrickTexture(THREE) {
  if (concreteBrickTextures.has(THREE)) {
    return concreteBrickTextures.get(THREE);
  }
  const texture = projectAssetTexture(
    THREE,
    "/world/assets/concrete-brick-path-v1.webp",
    8,
    2,
  );
  concreteBrickTextures.set(THREE, texture);
  return texture;
}

export function concreteBrickMaterial(THREE) {
  return resources.material(THREE, {
    color: "#c1bbb0",
    map: concreteBrickTexture(THREE),
    roughness: 0.96,
    metalness: 0.02,
  }, "MeshStandardMaterial");
}

export function districtGroundMaterial(THREE, kind) {
  const repository = kind === "repositories";
  return resources.uniqueMaterial(THREE, {
    // Repository portals now sit on a neutral concrete apron. The former
    // blue-gray circle read as a second, disconnected repository district.
    color: repository ? "#c1bbb0" : "#8a829b",
    map: projectAssetTexture(
      THREE,
      "/world/assets/concrete-brick-path-v1.webp",
      12,
      12,
    ),
    roughness: 0.97,
    metalness: 0.01,
    polygonOffset: true,
    polygonOffsetFactor: -1,
    polygonOffsetUnits: -1,
  }, "MeshStandardMaterial");
}

export function createDistrictGroundCircle(
  THREE,
  kind,
  radius = DISTRICT_GROUND_RADIUS,
) {
  const ground = new THREE.Mesh(
    resources.primitive(THREE, "CircleGeometry", radius, 96),
    districtGroundMaterial(THREE, kind),
  );
  ground.name = `forkmesh-${kind}-textured-ground`;
  ground.rotation.x = -Math.PI / 2;
  ground.position.y = 0.012;
  ground.receiveShadow = true;
  ground.userData.ground = true;
  return ground;
}

export const START_HERE_STEPS = Object.freeze([
  Object.freeze({ id: "start", label: "Find yourself on this map" }),
  Object.freeze({ id: "people", label: "Meet people at the Members Circle" }),
  Object.freeze({ id: "nodes", label: "Inspect the centered mirror nodes" }),
  Object.freeze({ id: "repositories", label: "Open a repository portal" }),
  Object.freeze({ id: "office", label: "Walk into the Office" }),
  Object.freeze({ id: "explore", label: "Ride, relax, and explore the city" }),
]);

export function startHereMapTexture(THREE, completed = new Set()) {
  return canvasTexture(THREE, 1024, 640, (context) => {
    context.fillStyle = "#0d1117";
    context.fillRect(0, 0, 1024, 640);
    context.strokeStyle = "#30363d";
    context.lineWidth = 16;
    context.strokeRect(8, 8, 1008, 624);
    context.fillStyle = "#2f81f7";
    context.fillRect(0, 0, 1024, 14);
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#f0f6fc";
    context.font = '900 66px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("YOU ARE HERE", 54, 72);
    context.fillStyle = "#3fb950";
    context.font = '900 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("START HERE · WORLD CHECKLIST", 56, 126);
    context.fillStyle = "#8c959f";
    context.font = '600 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      "Steps check off automatically as you visit each place.",
      56,
      168,
    );
    START_HERE_STEPS.forEach((step, index) => {
      const checked = completed.has(step.id);
      const y = 228 + index * 62;
      context.fillStyle = checked ? "#238636" : "#161b22";
      context.strokeStyle = checked ? "#3fb950" : "#484f58";
      context.lineWidth = 4;
      roundedRect(context, 56, y - 21, 42, 42, 8);
      context.fill();
      context.stroke();
      context.fillStyle = checked ? "#ffffff" : "#8c959f";
      context.textAlign = "center";
      context.font = '900 28px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(checked ? "✓" : "·", 77, y + 1);
      context.textAlign = "left";
      context.fillStyle = checked ? "#f0f6fc" : "#b1bac4";
      context.font = '700 27px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(step.label, 122, y);
    });
    context.fillStyle = "#58a6ff";
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("CLICK FOR CONTROLS · WASD / ARROWS · SHIFT RUN · SPACE JUMP", 56, 604);
  });
}

export function createTownLandscape(THREE) {
  const group = new THREE.Group();
  group.name = "forkmesh-town-mixed-landscape";
  const pathMaterial = concreteBrickMaterial(THREE);
  const junctionWidth = OFFICE_BRIDGE_WIDTH;
  const junctionHalfWidth = junctionWidth / 2;
  const junction = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      junctionWidth,
      WORLD_PATH_HEIGHT,
      junctionWidth,
    ),
    pathMaterial,
  );
  junction.name = "forkmesh-town-path-junction";
  junction.position.y = WORLD_PATH_CENTER_Y;
  junction.receiveShadow = true;
  junction.userData.ground = true;
  group.add(junction);

  [
    [
      "east-repositories",
      [junctionHalfWidth, 0],
      [REPOSITORY_CONNECTION_MIN_X, 0],
      WORLD_PATH_WIDTH,
    ],
    [
      "west-billboards",
      [-junctionHalfWidth, 0],
      [LEADERBOARD_CONNECTION_MAX_X, 0],
      WORLD_PATH_WIDTH,
    ],
    [
      "south-members",
      [0, junctionHalfWidth],
      [0, MEMBER_PATH_END_Z],
      WORLD_PATH_WIDTH,
    ],
  ].forEach(([id, start, end, width]) => {
    group.add(
      createLandscapePath(
        THREE,
        `forkmesh-town-path-${id}`,
        start,
        end,
        width,
        pathMaterial,
      ),
    );
  });

  const treeColors = ["#365443", "#486247", "#596d49", "#3f5949"];
  const treeLayout = deterministicTreeLayout();
  group.userData.treeLayout = treeLayout.map((tree) => ({ ...tree }));
  group.add(createTownTreeInstances(THREE, treeLayout, treeColors));
  return group;
}

export function createMainlandDistanceLod(THREE) {
  const lod = new THREE.LOD();
  lod.name = "forkmesh-mainland-distance-lod";
  // Its high-detail mainland lives in the surrounding World roots rather than
  // under this marker, so scene residency—not camera distance—owns the swap.
  lod.autoUpdate = false;
  lod.position.set(0, 0, CONTINUOUS_CITY_CENTER_Z);
  const high = new THREE.Group();
  high.name = "forkmesh-mainland-high-fidelity";
  const low = new THREE.Group();
  low.name = "forkmesh-mainland-low-poly";

  const land = new THREE.Mesh(
    roundedHexagonGeometry(
      THREE,
      CONTINUOUS_CITY_RADIUS,
      CONTINUOUS_CITY_CORNER_RADIUS,
      1,
    ),
    resources.material(THREE, { color: "#315e50" }, "MeshBasicMaterial"),
  );
  land.name = "forkmesh-mainland-low-land";
  land.rotation.x = -Math.PI / 2;
  land.position.y = CONTINUOUS_CITY_SURFACE_Y;
  low.add(land);

  const skylineMaterial = resources.uniqueMaterial(THREE, { color: "#173d35" }, "MeshBasicMaterial");
  [
    [0, 13, 0, 72, 26, 72],
    [128, 8, 45, 56, 16, 56],
    [-128, 8, 45, 56, 16, 56],
    [0, 6, 160, 70, 12, 48],
  ].forEach(([x, y, z, width, height, depth], index) => {
    const mass = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", width, height, depth),
      skylineMaterial,
    );
    mass.name = `forkmesh-mainland-low-skyline-${index + 1}`;
    mass.position.set(x, y, z);
    low.add(mass);
  });

  lod.addLevel(high, 0, 0.08);
  lod.addLevel(low, 1200, 0.12);
  lod.userData.mainlandHighFidelity = high;
  lod.userData.mainlandLowPoly = low;
  lod.userData.mainlandDistantTriangleCount = triangleWeight(low);
  lod.userData.mainlandDistantTriangleBudget =
    MAINLAND_DISTANT_TRIANGLE_BUDGET;
  return lod;
}
