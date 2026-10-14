import * as THREE from "three";

export const WIDTH = 1920;
export const HEIGHT = 1080;

let seedState = 1;
export function setSeed(value) {
  seedState = (Math.abs(Math.floor(value)) % 2147483646) + 1;
}
export function rand() {
  seedState = (seedState * 16807) % 2147483647;
  return (seedState - 1) / 2147483646;
}
const range = (a, b) => a + (b - a) * rand();
const pick = (list) => list[Math.floor(rand() * list.length) % list.length];

function hash2(x, y, s) {
  let h = (x * 374761393 + y * 668265263 + s * 2147483647) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967295;
}

function valueNoise(x, y, s) {
  const ix = Math.floor(x);
  const iy = Math.floor(y);
  const fx = x - ix;
  const fy = y - iy;
  const ux = fx * fx * (3 - 2 * fx);
  const uy = fy * fy * (3 - 2 * fy);
  const a = hash2(ix, iy, s);
  const b = hash2(ix + 1, iy, s);
  const c = hash2(ix, iy + 1, s);
  const d = hash2(ix + 1, iy + 1, s);
  return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

export function fbm(x, y, octaves = 4, s = 7) {
  let sum = 0;
  let amp = 0.5;
  let freq = 1;
  let norm = 0;
  for (let i = 0; i < octaves; i += 1) {
    sum += amp * valueNoise(x * freq, y * freq, s + i * 31);
    norm += amp;
    amp *= 0.5;
    freq *= 2.03;
  }
  return sum / norm;
}

export function makeCanvas(w, h) {
  const canvas = document.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  return [canvas, canvas.getContext("2d", { willReadFrequently: true })];
}

export function canvasTexture(canvas, { repeat = [1, 1], srgb = true } = {}) {
  const texture = new THREE.CanvasTexture(canvas);
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.repeat.set(repeat[0], repeat[1]);
  if (srgb) texture.colorSpace = THREE.SRGBColorSpace;
  texture.anisotropy = 8;
  texture.needsUpdate = true;
  return texture;
}

const hexRgb = (hex) => {
  const c = new THREE.Color(hex);
  return [c.r * 255, c.g * 255, c.b * 255];
};
const lerp = (a, b, t) => a + (b - a) * t;

function pixelTexture(w, h, shade, options) {
  const [canvas, ctx] = makeCanvas(w, h);
  const image = ctx.createImageData(w, h);
  const data = image.data;
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const rgb = shade(x, y);
      const i = (y * w + x) * 4;
      data[i] = rgb[0];
      data[i + 1] = rgb[1];
      data[i + 2] = rgb[2];
      data[i + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
  return canvasTexture(canvas, options);
}

export function woodTexture({
  light = "#c49a6c",
  dark = "#8a5e3b",
  planks = 6,
  seams = true,
  size = 1024,
  repeat = [1, 1],
  seed = 3,
  stretch = 1,
} = {}) {
  const a = hexRgb(light);
  const b = hexRgb(dark);
  const plankH = size / planks;
  const offsets = Array.from({ length: planks }, (_, i) => hash2(i, 5, seed));
  const tones = Array.from({ length: planks }, (_, i) => 0.85 + 0.3 * hash2(i, 9, seed));
  return pixelTexture(size, size, (x, y) => {
    const p = Math.floor(y / plankH);
    const local = (y % plankH) / plankH;
    const ox = offsets[p] * 900;
    const warp = fbm((x + ox) * 0.004 * stretch, y * 0.02, 3, seed + p) * 6;
    const ring = 0.5 + 0.5 * Math.sin((y * 0.55 + warp * 6 + fbm((x + ox) * 0.01, y * 0.1, 2, seed) * 4) * 1.3);
    const fleck = fbm((x + ox) * 0.08, y * 0.6, 2, seed + 11);
    let t = 0.25 + ring * 0.35 + (fleck - 0.5) * 0.35;
    t = Math.min(1, Math.max(0, t));
    let tone = tones[p];
    if (seams) {
      const edge = Math.min(local, 1 - local) * plankH;
      if (edge < 1.2) tone *= 0.62;
      const joint = ((x + ox) % size) / size;
      if (joint < 0.0015) tone *= 0.7;
    }
    return [
      Math.min(255, lerp(a[0], b[0], t) * tone),
      Math.min(255, lerp(a[1], b[1], t) * tone),
      Math.min(255, lerp(a[2], b[2], t) * tone),
    ];
  }, { repeat });
}

export function grainTexture({ base = "#f2efe9", amount = 0.04, scale = 0.02, size = 512, repeat = [1, 1], seed = 5 } = {}) {
  const c = hexRgb(base);
  return pixelTexture(size, size, (x, y) => {
    const n = (fbm(x * scale, y * scale, 4, seed) - 0.5) * 2 * amount + (hash2(x, y, seed) - 0.5) * amount * 0.6;
    return [c[0] * (1 + n), c[1] * (1 + n), c[2] * (1 + n)].map((v) => Math.min(255, Math.max(0, v)));
  }, { repeat });
}

export function fabricTexture({ base = "#5d6b7a", amount = 0.08, size = 256, repeat = [4, 4], seed = 9 } = {}) {
  const c = hexRgb(base);
  return pixelTexture(size, size, (x, y) => {
    const weave = ((x % 4 < 2) !== (y % 4 < 2) ? 1 : -1) * 0.25;
    const n = (fbm(x * 0.05, y * 0.05, 3, seed) - 0.5) + weave * 0.4 + (hash2(x, y, seed) - 0.5) * 0.5;
    return [c[0] * (1 + n * amount), c[1] * (1 + n * amount), c[2] * (1 + n * amount)].map((v) => Math.min(255, Math.max(0, v)));
  }, { repeat });
}

export function std(color, options = {}) {
  return new THREE.MeshStandardMaterial({ color, roughness: 0.8, metalness: 0, ...options });
}

export function mesh(geometry, material, { cast = true, receive = true } = {}) {
  const m = new THREE.Mesh(geometry, material);
  m.castShadow = cast;
  m.receiveShadow = receive;
  return m;
}

export function roundedBoxGeometry(w, h, d, r = 0.01, segments = 3) {
  r = Math.min(r, w / 2 - 1e-4, h / 2 - 1e-4, d / 2 - 1e-4);
  const shape = new THREE.Shape();
  const x0 = -w / 2 + r;
  const y0 = -h / 2 + r;
  const iw = w - 2 * r;
  const ih = h - 2 * r;
  shape.moveTo(x0, y0);
  shape.lineTo(x0 + iw, y0);
  shape.lineTo(x0 + iw, y0 + ih);
  shape.lineTo(x0, y0 + ih);
  shape.lineTo(x0, y0);
  const geometry = new THREE.ExtrudeGeometry(shape, {
    depth: Math.max(1e-4, d - 2 * r),
    bevelEnabled: true,
    bevelSize: r,
    bevelThickness: r,
    bevelSegments: segments,
    curveSegments: 4,
  });
  geometry.translate(0, 0, -(d - 2 * r) / 2);
  geometry.computeVertexNormals();
  const uv = geometry.attributes.uv;
  for (let i = 0; i < uv.count; i += 1) uv.setXY(i, uv.getX(i) / Math.max(w, h, d), uv.getY(i) / Math.max(w, h, d));
  return geometry;
}

export function box(parent, w, h, d, material, x, y, z, options = {}) {
  const geometry = options.round
    ? roundedBoxGeometry(w, h, d, options.round)
    : new THREE.BoxGeometry(w, h, d);
  const m = mesh(geometry, material, options);
  m.position.set(x, y, z);
  if (options.ry) m.rotation.y = options.ry;
  if (options.rx) m.rotation.x = options.rx;
  if (options.rz) m.rotation.z = options.rz;
  if (options.shell) m.layers.set(1);
  parent.add(m);
  return m;
}

export function cylinder(parent, rt, rb, h, material, x, y, z, segments = 48, options = {}) {
  const m = mesh(new THREE.CylinderGeometry(rt, rb, h, segments), material, options);
  m.position.set(x, y, z);
  parent.add(m);
  return m;
}

export function lathe(parent, points, material, x, y, z, segments = 64, options = {}) {
  const geometry = new THREE.LatheGeometry(points.map(([px, py]) => new THREE.Vector2(px, py)), segments);
  const m = mesh(geometry, material, options);
  m.position.set(x, y, z);
  parent.add(m);
  return m;
}

function leafGeometry(length, width, curl, segments = 10) {
  const positions = [];
  const indices = [];
  const uvs = [];
  for (let i = 0; i <= segments; i += 1) {
    const t = i / segments;
    const half = width * Math.sin(Math.PI * Math.pow(t, 0.8)) * (1 - 0.15 * t) * 0.5;
    const z = t * length;
    const droop = -curl * t * t * length;
    for (let j = -1; j <= 1; j += 1) {
      const x = j * half;
      const fold = Math.abs(j) * half * 0.35;
      positions.push(x, droop + fold, z);
      uvs.push((j + 1) / 2, t);
    }
  }
  for (let i = 0; i < segments; i += 1) {
    for (let j = 0; j < 2; j += 1) {
      const a = i * 3 + j;
      const b = a + 1;
      const c = a + 3;
      const d = c + 1;
      indices.push(a, c, b, b, c, d);
    }
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.Float32BufferAttribute(positions, 3));
  geometry.setAttribute("uv", new THREE.Float32BufferAttribute(uvs, 2));
  geometry.setIndex(indices);
  geometry.computeVertexNormals();
  return geometry;
}

export function plant(parent, x, z, {
  kind = "leafy",
  height = 1.4,
  pot = "#e9e4dc",
  potHeight = 0.42,
  potRadius = 0.22,
  leaf = "#3f6f3a",
  leaves = 70,
  seed = 1,
  floor = 0,
} = {}) {
  setSeed(seed * 7919);
  const group = new THREE.Group();
  group.position.set(x, floor, z);
  const potMaterial = std(pot, { roughness: 0.55 });
  lathe(group, [
    [0, 0], [potRadius * 0.78, 0], [potRadius * 0.82, 0.02], [potRadius, potHeight * 0.96], [potRadius * 1.03, potHeight],
    [potRadius * 0.95, potHeight], [potRadius * 0.93, potHeight * 0.93], [0, potHeight * 0.93],
  ], potMaterial, 0, 0, 0, 64);
  cylinder(group, potRadius * 0.92, potRadius * 0.92, 0.01, std("#3b2f26", { roughness: 1 }), 0, potHeight * 0.92, 0, 32);
  const leafMaterial = new THREE.MeshStandardMaterial({ color: "#ffffff", roughness: 0.55, side: THREE.DoubleSide });
  const baseColor = new THREE.Color(leaf);
  const geometries = {
    leafy: leafGeometry(0.2, 0.11, 0.25),
    blade: leafGeometry(1, 0.075, 0.08, 14),
    palm: leafGeometry(0.34, 0.06, 0.6),
  };
  const geometry = geometries[kind] || geometries.leafy;
  const instanced = new THREE.InstancedMesh(geometry, leafMaterial, leaves);
  instanced.castShadow = true;
  instanced.receiveShadow = true;
  const stemMaterial = std("#4d5a34", { roughness: 0.8 });
  const dummy = new THREE.Object3D();
  const color = new THREE.Color();
  const top = potHeight * 0.93;
  for (let i = 0; i < leaves; i += 1) {
    const angle = rand() * Math.PI * 2;
    let px;
    let py;
    let pz;
    let scale;
    let pitch;
    if (kind === "blade") {
      const r = rand() * potRadius * 0.55;
      px = Math.cos(angle) * r;
      pz = Math.sin(angle) * r;
      py = top;
      scale = height * range(0.55, 1);
      pitch = -Math.PI / 2 + range(0.05, 0.38);
      dummy.position.set(px, py, pz);
      dummy.rotation.set(0, 0, 0);
      dummy.lookAt(px + Math.cos(angle), py, pz + Math.sin(angle));
      dummy.rotateX(pitch);
      dummy.rotateZ(range(-0.4, 0.4));
      dummy.scale.set(range(0.8, 1.2), 1, scale);
    } else {
      const t = Math.pow(rand(), 0.7);
      const h = top + 0.1 + t * (height - top - 0.1);
      const spread = (kind === "palm" ? 0.5 : 0.36) * height * Math.sin(Math.PI * (0.25 + 0.75 * t)) * range(0.6, 1.1);
      px = Math.cos(angle) * spread * range(0.2, 1);
      pz = Math.sin(angle) * spread * range(0.2, 1);
      py = h;
      scale = range(0.75, 1.3) * (kind === "palm" ? 1.4 : 1.2) * (height / 1.4);
      dummy.position.set(px, py, pz);
      dummy.rotation.set(0, 0, 0);
      dummy.lookAt(px * 2.2, py + range(-0.15, 0.35), pz * 2.2);
      dummy.rotateZ(range(-0.6, 0.6));
      dummy.scale.setScalar(scale);
      if (rand() < 0.6) {
        const stem = mesh(new THREE.CylinderGeometry(0.004, 0.006, 1, 5), stemMaterial);
        const from = new THREE.Vector3(px * 0.15, top, pz * 0.15);
        const to = new THREE.Vector3(px, py, pz);
        stem.position.copy(from).add(to).multiplyScalar(0.5);
        stem.scale.y = from.distanceTo(to);
        stem.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), to.clone().sub(from).normalize());
        group.add(stem);
      }
    }
    dummy.updateMatrix();
    instanced.setMatrixAt(i, dummy.matrix);
    color.copy(baseColor).offsetHSL(range(-0.02, 0.03), range(-0.08, 0.06), range(-0.08, 0.07));
    instanced.setColorAt(i, color);
  }
  group.add(instanced);
  parent.add(group);
  return group;
}

export function gradientSkyTexture(stops, { w = 16, h = 1024 } = {}) {
  const [canvas, ctx] = makeCanvas(w, h);
  const gradient = ctx.createLinearGradient(0, 0, 0, h);
  stops.forEach(([at, color]) => gradient.addColorStop(at, color));
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, w, h);
  const texture = canvasTexture(canvas, { repeat: [1, 1] });
  texture.wrapS = THREE.ClampToEdgeWrapping;
  texture.wrapT = THREE.ClampToEdgeWrapping;
  return texture;
}

export function exteriorCanvas({ w = 2048, h = 1024, horizon = 0.62, sky = ["#9cc7e8", "#d6e9f5", "#eef5f7"], seed = 4, trees = true, buildings = true } = {}) {
  setSeed(seed);
  const [canvas, ctx] = makeCanvas(w, h);
  const gradient = ctx.createLinearGradient(0, 0, 0, h * horizon);
  gradient.addColorStop(0, sky[0]);
  gradient.addColorStop(0.7, sky[1]);
  gradient.addColorStop(1, sky[2]);
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, w, h);
  if (buildings) {
    for (let layer = 0; layer < 2; layer += 1) {
      let x = -20;
      while (x < w) {
        const bw = range(40, 140);
        const bh = range(0.08, layer ? 0.2 : 0.32) * h;
        ctx.fillStyle = layer ? "#b7c7d2" : "#c9d6de";
        ctx.fillRect(x, h * horizon - bh, bw, bh + 4);
        if (layer) {
          ctx.fillStyle = "rgba(255,255,255,0.18)";
          for (let wy = h * horizon - bh + 8; wy < h * horizon - 6; wy += 12) ctx.fillRect(x + 4, wy, bw - 8, 3);
        }
        x += bw + range(4, 40);
      }
    }
  }
  ctx.fillStyle = "#8e9a80";
  ctx.fillRect(0, h * horizon, w, h * (1 - horizon));
  if (trees) {
    const crowns = Array.from({ length: 900 }, () => ({ x: rand() * w, y: h * horizon + Math.pow(rand(), 1.5) * 0.3 * h }));
    crowns.sort((a, b) => a.y - b.y);
    crowns.forEach(({ x: tx, y: ty }) => {
      const near = (ty / h - horizon) / 0.3;
      const size = 7 + near * 22;
      const haze = 1 - near;
      const hue = range(78, 118);
      const sat = 16 + near * 12;
      const light = range(30, 40) + haze * 14;
      ctx.fillStyle = `hsl(28,${10 + near * 10}%,${30 + haze * 20}%)`;
      ctx.fillRect(tx - size * 0.06, ty, size * 0.12, size * 0.9);
      for (let k = 0; k < 7; k += 1) {
        const lx = tx + (rand() - 0.5) * size * 1.3;
        const ly = ty - rand() * size * 0.9;
        const r = size * range(0.35, 0.6);
        const g = ctx.createRadialGradient(lx - r * 0.3, ly - r * 0.4, r * 0.1, lx, ly, r);
        g.addColorStop(0, `hsl(${hue},${sat}%,${light + 9}%)`);
        g.addColorStop(1, `hsl(${hue},${sat}%,${light - 6}%)`);
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(lx, ly, r, 0, Math.PI * 2);
        ctx.fill();
      }
    });
    const haze = ctx.createLinearGradient(0, h * horizon - 0.12 * h, 0, h * horizon + 0.12 * h);
    haze.addColorStop(0, "rgba(230,238,242,0)");
    haze.addColorStop(0.5, "rgba(230,238,242,0.35)");
    haze.addColorStop(1, "rgba(230,238,242,0)");
    ctx.fillStyle = haze;
    ctx.fillRect(0, h * horizon - 0.12 * h, w, 0.24 * h);
  }
  return canvas;
}

export function backdrop(parent, canvas, { width, height, x, y, z, ry = 0, brightness = 1.6 }) {
  const texture = canvasTexture(canvas);
  texture.wrapS = THREE.ClampToEdgeWrapping;
  texture.wrapT = THREE.ClampToEdgeWrapping;
  const material = new THREE.MeshBasicMaterial({ map: texture, fog: false });
  material.color.setScalar(brightness);
  const m = new THREE.Mesh(new THREE.PlaneGeometry(width, height), material);
  m.position.set(x, y, z);
  m.rotation.y = ry;
  m.userData.backdrop = true;
  parent.add(m);
  return m;
}

export function environmentFrom(renderer, build) {
  const scene = new THREE.Scene();
  build(scene);
  const pmrem = new THREE.PMREMGenerator(renderer);
  const target = pmrem.fromScene(scene, 0.04);
  pmrem.dispose();
  return target.texture;
}

export function roomEnvironment(renderer, { sky = "#dfe9f2", floor = "#9c8a76", walls = "#d9d4cc", windowColor = "#ffffff", windowPower = 6, windowDir = [1, 0, 0], lamps = [] } = {}) {
  return environmentFrom(renderer, (scene) => {
    const room = new THREE.Mesh(new THREE.BoxGeometry(10, 6, 10), new THREE.MeshBasicMaterial({ color: walls, side: THREE.BackSide }));
    room.position.y = 2;
    scene.add(room);
    const floorMesh = new THREE.Mesh(new THREE.PlaneGeometry(10, 10), new THREE.MeshBasicMaterial({ color: floor }));
    floorMesh.rotation.x = -Math.PI / 2;
    floorMesh.position.y = -0.99;
    scene.add(floorMesh);
    const ceiling = new THREE.Mesh(new THREE.PlaneGeometry(10, 10), new THREE.MeshBasicMaterial({ color: sky }));
    ceiling.rotation.x = Math.PI / 2;
    ceiling.position.y = 4.9;
    scene.add(ceiling);
    const win = new THREE.Mesh(new THREE.PlaneGeometry(6, 3), new THREE.MeshBasicMaterial({ color: windowColor }));
    win.material.color.multiplyScalar(windowPower);
    const dir = new THREE.Vector3(...windowDir).normalize();
    win.position.copy(dir).multiplyScalar(4.9).setY(2);
    win.lookAt(0, 2, 0);
    scene.add(win);
    lamps.forEach(([lx, ly, lz, color, power, size]) => {
      const lamp = new THREE.Mesh(new THREE.SphereGeometry(size || 0.4, 16, 8), new THREE.MeshBasicMaterial({ color }));
      lamp.material.color.multiplyScalar(power);
      lamp.position.set(lx, ly, lz);
      scene.add(lamp);
    });
  });
}

export function createRenderer() {
  const canvas = document.createElement("canvas");
  canvas.width = WIDTH;
  canvas.height = HEIGHT;
  const renderer = new THREE.WebGLRenderer({ canvas, antialias: false, preserveDrawingBuffer: true, powerPreference: "high-performance" });
  renderer.setPixelRatio(1);
  renderer.setSize(WIDTH, HEIGHT, false);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure = 1;
  renderer.shadowMap.enabled = true;
  renderer.shadowMap.type = THREE.PCFShadowMap;
  return renderer;
}

export function shadowLight(light, { size = 2048, extent = 6, near = 0.5, far = 40, bias = -0.0004, normalBias = 0.02, shell = true } = {}) {
  light.castShadow = true;
  if (shell) light.shadow.camera.layers.enable(1);
  light.shadow.mapSize.set(size, size);
  light.shadow.bias = bias;
  light.shadow.normalBias = normalBias;
  const cam = light.shadow.camera;
  if (cam.isOrthographicCamera) {
    cam.left = -extent;
    cam.right = extent;
    cam.top = extent;
    cam.bottom = -extent;
  }
  cam.near = near;
  cam.far = far;
  cam.updateProjectionMatrix();
  return light;
}

export function skyOcclusionLight(scene, { intensity = 0.8, color = "#eef2f7", target = [0, 1, -3], extent = 7, radius = 14, elevation = [0.15, 1] } = {}) {
  const light = shadowLight(new THREE.DirectionalLight(color, intensity), { extent, far: radius * 2.5, size: 2048, normalBias: 0.03, shell: false });
  light.target.position.set(...target);
  scene.add(light, light.target);
  return (r) => {
    const u = r();
    const v = r();
    const cosTheta = Math.sqrt(lerp(elevation[0], elevation[1], u));
    const sinTheta = Math.sqrt(1 - cosTheta * cosTheta);
    const phi = v * Math.PI * 2;
    light.position.set(
      target[0] + Math.cos(phi) * sinTheta * radius,
      target[1] + cosTheta * radius,
      target[2] + Math.sin(phi) * sinTheta * radius,
    );
    light.shadow.needsUpdate = true;
  };
}

export function fillLight(from, intensity, color = "#ffffff") {
  const light = new THREE.DirectionalLight(color, intensity);
  light.position.set(...from);
  light.add(light.target);
  light.target.position.set(0, -0.6, -5);
  return light;
}

function halton(index, base) {
  let f = 1;
  let r = 0;
  let i = index;
  while (i > 0) {
    f /= base;
    r += f * (i % base);
    i = Math.floor(i / base);
  }
  return r;
}

export async function accumulate(renderer, scene, camera, { samples = 40, aperture = 0, focus = null, jitters = [], onSample = null, prepare = null } = {}) {
  const width = renderer.domElement.width;
  const height = renderer.domElement.height;
  const [canvas, ctx] = makeCanvas(width, height);
  const accum = new Float32Array(width * height * 3);
  const basePosition = camera.position.clone();
  const baseQuaternion = camera.quaternion.clone();
  const focusPoint = focus ? new THREE.Vector3(...focus) : null;
  const right = new THREE.Vector3(1, 0, 0).applyQuaternion(baseQuaternion);
  const up = new THREE.Vector3(0, 1, 0).applyQuaternion(baseQuaternion);
  let randomState = 99991;
  const r = () => {
    randomState = (randomState * 48271) % 2147483647;
    return randomState / 2147483647;
  };
  for (let i = 0; i < samples; i += 1) {
    const jx = halton(i + 1, 2) - 0.5;
    const jy = halton(i + 1, 3) - 0.5;
    camera.setViewOffset(width, height, jx, jy, width, height);
    camera.position.copy(basePosition);
    camera.quaternion.copy(baseQuaternion);
    if (aperture > 0 && focusPoint) {
      const radius = Math.sqrt(halton(i + 1, 5)) * aperture;
      const angle = halton(i + 1, 7) * Math.PI * 2;
      camera.position.addScaledVector(right, Math.cos(angle) * radius).addScaledVector(up, Math.sin(angle) * radius);
      const toFocus = focusPoint.clone().sub(basePosition);
      const forward = new THREE.Vector3(0, 0, -1).applyQuaternion(baseQuaternion);
      const depth = toFocus.dot(forward);
      const lookTarget = basePosition.clone().addScaledVector(forward, depth);
      const m = new THREE.Matrix4().lookAt(camera.position, lookTarget, up);
      camera.quaternion.setFromRotationMatrix(m);
    }
    camera.updateMatrixWorld(true);
    jitters.forEach((jitter) => jitter(r, i));
    if (prepare) prepare(i);
    renderer.render(scene, camera);
    ctx.drawImage(renderer.domElement, 0, 0);
    const data = ctx.getImageData(0, 0, width, height).data;
    for (let p = 0, q = 0; p < data.length; p += 4, q += 3) {
      accum[q] += data[p];
      accum[q + 1] += data[p + 1];
      accum[q + 2] += data[p + 2];
    }
    if (onSample) await onSample(i);
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
  camera.clearViewOffset();
  camera.position.copy(basePosition);
  camera.quaternion.copy(baseQuaternion);
  const out = ctx.createImageData(width, height);
  for (let p = 0, q = 0; p < out.data.length; p += 4, q += 3) {
    out.data[p] = accum[q] / samples;
    out.data[p + 1] = accum[q + 1] / samples;
    out.data[p + 2] = accum[q + 2] / samples;
    out.data[p + 3] = 255;
  }
  ctx.putImageData(out, 0, 0);
  return canvas;
}

function jpegBytes(dataUrl) {
  return Math.floor(((dataUrl.length - dataUrl.indexOf(",") - 1) * 3) / 4);
}

export function grade(source, { blur = 1.2, vignette = 0.16, warmth = 0, lift = 0, saturation = 1, contrast = 1, brightness = 1 } = {}) {
  const [canvas, ctx] = makeCanvas(WIDTH, HEIGHT);
  ctx.filter = `blur(${blur}px) saturate(${saturation}) contrast(${contrast}) brightness(${brightness})`;
  ctx.drawImage(source, -8, -8, WIDTH + 16, HEIGHT + 16);
  ctx.filter = "none";
  if (warmth) {
    ctx.globalCompositeOperation = "soft-light";
    ctx.fillStyle = warmth > 0 ? `rgba(255,170,90,${warmth})` : `rgba(90,150,255,${-warmth})`;
    ctx.fillRect(0, 0, WIDTH, HEIGHT);
  }
  if (lift) {
    ctx.globalCompositeOperation = "screen";
    ctx.fillStyle = `rgba(40,40,48,${lift})`;
    ctx.fillRect(0, 0, WIDTH, HEIGHT);
  }
  if (vignette) {
    ctx.globalCompositeOperation = "multiply";
    const g = ctx.createRadialGradient(WIDTH / 2, HEIGHT * 0.48, HEIGHT * 0.35, WIDTH / 2, HEIGHT * 0.5, WIDTH * 0.62);
    g.addColorStop(0, "rgba(255,255,255,1)");
    g.addColorStop(1, `rgba(${Math.round(255 * (1 - vignette))},${Math.round(255 * (1 - vignette))},${Math.round(255 * (1 - vignette))},1)`);
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, WIDTH, HEIGHT);
  }
  ctx.globalCompositeOperation = "source-over";
  const data = ctx.getImageData(0, 0, WIDTH, HEIGHT).data;
  for (let i = 0; i < data.length; i += 4) {
    const n = (hash2(i >> 2, 17, 3) - 0.5) * 3;
    data[i] += n;
    data[i + 1] += n;
    data[i + 2] += n;
  }
  const image = new ImageData(data, WIDTH, HEIGHT);
  ctx.putImageData(image, 0, 0);
  return canvas;
}

export function encode(canvas, { targetBytes = 235000, maxBytes = 270000 } = {}) {
  let low = 0.5;
  let high = 0.92;
  let best = canvas.toDataURL("image/jpeg", low);
  let quality = low;
  for (let step = 0; step < 7; step += 1) {
    const q = (low + high) / 2;
    const url = canvas.toDataURL("image/jpeg", q);
    if (jpegBytes(url) <= targetBytes) {
      best = url;
      quality = q;
      low = q;
    } else {
      high = q;
    }
  }
  const top = canvas.toDataURL("image/jpeg", 0.92);
  if (jpegBytes(top) <= maxBytes && jpegBytes(top) <= targetBytes * 1.05) {
    best = top;
    quality = 0.92;
  }
  const [half, hctx] = makeCanvas(WIDTH / 2, HEIGHT / 2);
  hctx.imageSmoothingQuality = "high";
  hctx.drawImage(canvas, 0, 0, WIDTH / 2, HEIGHT / 2);
  const [quarter, qctx] = makeCanvas(WIDTH / 4, HEIGHT / 4);
  qctx.imageSmoothingQuality = "high";
  qctx.drawImage(half, 0, 0, WIDTH / 4, HEIGHT / 4);
  const [thumb, tctx] = makeCanvas(320, 180);
  tctx.imageSmoothingQuality = "high";
  tctx.drawImage(quarter, 0, 0, 320, 180);
  const thumbUrl = thumb.toDataURL("image/jpeg", 0.84);
  const pixels = canvas.getContext("2d").getImageData(0, 0, WIDTH, HEIGHT).data;
  let sum = 0;
  for (let i = 0; i < pixels.length; i += 16) sum += 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
  return {
    full: best,
    thumb: thumbUrl,
    quality: Number(quality.toFixed(3)),
    bytes: jpegBytes(best),
    thumbBytes: jpegBytes(thumbUrl),
    meanLuma: Number((sum / (pixels.length / 16) / 255).toFixed(3)),
  };
}

export async function loadImage(url) {
  const image = new Image();
  image.decoding = "sync";
  image.src = url;
  await image.decode();
  return image;
}

export async function loadBrandFont() {
  const face = new FontFace("ForkMesh Favorit", "url(/www/public/assets/fonts/ABCFavorit-Bold.woff2)");
  await face.load();
  document.fonts.add(face);
  return "ForkMesh Favorit";
}

export function camera(fov, position, target) {
  const cam = new THREE.PerspectiveCamera(fov, WIDTH / HEIGHT, 0.05, 6000);
  cam.position.set(...position);
  cam.lookAt(...target);
  cam.layers.enable(1);
  cam.updateMatrixWorld(true);
  return cam;
}

export { THREE };

function artCanvas({ w = 1024, h = 768, palette = ["#e8dccb", "#c46b4b", "#2f4858", "#d9a441", "#8aa39b"], seed = 21 } = {}) {
  setSeed(seed);
  const [canvas, ctx] = makeCanvas(w, h);
  ctx.fillStyle = palette[0];
  ctx.fillRect(0, 0, w, h);
  for (let i = 0; i < 7; i += 1) {
    ctx.fillStyle = palette[1 + (i % (palette.length - 1))];
    ctx.globalAlpha = range(0.55, 0.95);
    ctx.beginPath();
    const cx = range(0.15, 0.85) * w;
    const cy = range(0.15, 0.85) * h;
    if (i % 3 === 0) {
      ctx.arc(cx, cy, range(0.12, 0.28) * h, 0, Math.PI * 2);
    } else if (i % 3 === 1) {
      ctx.ellipse(cx, cy, range(0.1, 0.3) * w, range(0.04, 0.1) * h, range(-0.4, 0.4), 0, Math.PI * 2);
    } else {
      ctx.rect(cx - 0.12 * w, cy - 0.2 * h, range(0.05, 0.12) * w, range(0.25, 0.45) * h);
    }
    ctx.fill();
  }
  ctx.globalAlpha = 1;
  return canvas;
}

function framedArt(parent, x, y, z, w, h, { ry = 0, frame = "#1f1d1b", seed = 21, palette } = {}) {
  const group = new THREE.Group();
  group.position.set(x, y, z);
  group.rotation.y = ry;
  const frameMaterial = std(frame, { roughness: 0.5 });
  box(group, w + 0.06, h + 0.06, 0.035, frameMaterial, 0, 0, 0.0175);
  const matTexture = canvasTexture(artCanvas({ seed, palette }));
  matTexture.wrapS = THREE.ClampToEdgeWrapping;
  matTexture.wrapT = THREE.ClampToEdgeWrapping;
  const art = mesh(new THREE.PlaneGeometry(w * 0.82, h * 0.82), std("#ffffff", { map: matTexture, roughness: 0.9 }), { cast: false });
  art.position.set(0, 0, 0.0365);
  const passe = mesh(new THREE.PlaneGeometry(w, h), std("#f4f1ea", { roughness: 0.95 }), { cast: false });
  passe.position.set(0, 0, 0.036);
  group.add(passe, art);
  parent.add(group);
  return group;
}

function swivelChair(parent, x, z, ry, { fabric = "#4a5560", frame = "#2a2c2f", floor = 0 } = {}) {
  const group = new THREE.Group();
  group.position.set(x, floor, z);
  group.rotation.y = ry;
  const fabricMaterial = std(fabric, { map: fabricTexture({ base: "#ffffff", amount: 0.1 }), roughness: 0.92 });
  const frameMaterial = std(frame, { roughness: 0.35, metalness: 0.6 });
  box(group, 0.52, 0.09, 0.5, fabricMaterial, 0, 0.47, 0.01, { round: 0.04 });
  const back = box(group, 0.52, 0.36, 0.07, fabricMaterial, 0, 0.78, -0.24, { round: 0.035 });
  back.rotation.x = -0.16;
  [-1, 1].forEach((s) => {
    box(group, 0.035, 0.03, 0.34, frameMaterial, s * 0.27, 0.66, -0.02, { round: 0.012 });
    box(group, 0.025, 0.2, 0.025, frameMaterial, s * 0.27, 0.56, -0.15, { round: 0.008 });
  });
  box(group, 0.04, 0.3, 0.03, frameMaterial, 0, 0.6, -0.27, { round: 0.01 });
  cylinder(group, 0.025, 0.025, 0.36, frameMaterial, 0, 0.27, 0, 16);
  for (let i = 0; i < 5; i += 1) {
    const a = (i / 5) * Math.PI * 2;
    const spoke = box(group, 0.3, 0.025, 0.04, frameMaterial, Math.cos(a) * 0.15, 0.07, Math.sin(a) * 0.15, { round: 0.008 });
    spoke.rotation.y = -a;
    const wheel = cylinder(group, 0.025, 0.025, 0.02, std("#111214", { roughness: 0.6 }), Math.cos(a) * 0.29, 0.025, Math.sin(a) * 0.29, 12);
    wheel.rotation.x = Math.PI / 2;
  }
  parent.add(group);
  return group;
}

export const SCENES = {};

SCENES.meeting = async (renderer) => {
  const scene = new THREE.Scene();
  const W = 8.4;
  const D0 = -6.2;
  const D1 = 2.2;
  const H = 2.85;
  const floorTexture = woodTexture({ light: "#e2c9a6", dark: "#c29d74", planks: 10, repeat: [6, 6], seed: 11 });
  floorTexture.center.set(0.5, 0.5);
  floorTexture.rotation = Math.PI / 2;
  const floorMaterial = std("#ffffff", { map: floorTexture, roughness: 0.42 });
  box(scene, W, 0.1, D1 - D0, floorMaterial, 0, -0.05, (D0 + D1) / 2, { shell: true });
  const wallMaterial = std("#ffffff", { map: grainTexture({ base: "#efebe4", amount: 0.025, repeat: [3, 2] }), roughness: 0.92 });
  box(scene, W, H, 0.12, wallMaterial, 0, H / 2, D0 - 0.06, { shell: true });
  box(scene, 0.12, H, D1 - D0, wallMaterial, -W / 2 - 0.06, H / 2, (D0 + D1) / 2, { shell: true });
  box(scene, W, 0.1, D1 - D0, std("#f6f5f2", { roughness: 0.95, emissive: "#f3efe8", emissiveIntensity: 0.18 }), 0, H + 0.05, (D0 + D1) / 2, { shell: true });
  const skirting = std("#e4dfd6", { roughness: 0.7 });
  box(scene, W, 0.08, 0.015, skirting, 0, 0.04, D0 + 0.008, { shell: true });
  const mullion = std("#2c3034", { roughness: 0.45, metalness: 0.5 });
  for (let z = D0; z <= D1 + 0.01; z += 1.4) box(scene, 0.07, H, 0.07, mullion, W / 2, H / 2, z, { shell: true });
  box(scene, 0.12, 0.1, D1 - D0, mullion, W / 2, 0.05, (D0 + D1) / 2, { shell: true });
  box(scene, 0.12, 0.12, D1 - D0, mullion, W / 2, H - 0.06, (D0 + D1) / 2, { shell: true });
  box(scene, 0.05, 0.04, D1 - D0, mullion, W / 2, 0.95, (D0 + D1) / 2, { shell: true });
  const glass = new THREE.Mesh(new THREE.PlaneGeometry(D1 - D0, H), new THREE.MeshPhysicalMaterial({ color: "#ffffff", roughness: 0.05, metalness: 0, transparent: true, opacity: 0.08, envMapIntensity: 0.6 }));
  glass.rotation.y = -Math.PI / 2;
  glass.position.set(W / 2 + 0.01, H / 2, (D0 + D1) / 2);
  scene.add(glass);
  backdrop(scene, exteriorCanvas({ horizon: 0.7, seed: 8 }), { width: 320, height: 80, x: 40, y: 14, z: -20, ry: -Math.PI / 2, brightness: 1.55 });
  const screenGroup = new THREE.Group();
  box(screenGroup, 1.72, 0.99, 0.04, std("#16171a", { roughness: 0.4, metalness: 0.3 }), 0, 0, 0, { round: 0.01 });
  const screen = mesh(new THREE.PlaneGeometry(1.68, 0.95), new THREE.MeshPhysicalMaterial({ color: "#07080a", roughness: 0.08, clearcoat: 1, clearcoatRoughness: 0.05, emissive: "#1b2633", emissiveIntensity: 0.25 }), { cast: false });
  screen.position.z = 0.021;
  screenGroup.add(screen);
  screenGroup.position.set(-2.55, 1.5, D0 + 0.04);
  scene.add(screenGroup);
  const oak = std("#ffffff", { map: woodTexture({ light: "#d6b48b", dark: "#b18860", planks: 1, seams: false, seed: 4, repeat: [1, 1], stretch: 0.4 }), roughness: 0.5 });
  const white = std("#f3f1ed", { roughness: 0.45 });
  box(scene, 2.0, 0.5, 0.45, white, -2.55, 0.3, D0 + 0.26, { round: 0.015 });
  box(scene, 2.02, 0.03, 0.47, oak, -2.55, 0.565, D0 + 0.26, { round: 0.008 });
  for (let i = 0; i < 4; i += 1) box(scene, 0.008, 0.38, 0.003, std("#d8d4cc"), -3.4 + i * 0.5 + 0.25, 0.3, D0 + 0.486);
  lathe(scene, [[0, 0], [0.07, 0], [0.09, 0.08], [0.06, 0.2], [0.03, 0.26], [0.035, 0.28], [0, 0.28]], std("#2f4b45", { roughness: 0.3 }), -1.85, 0.58, D0 + 0.25);
  plant(scene, -3.45, D0 + 0.26, { kind: "blade", height: 0.55, pot: "#d6cfc4", potHeight: 0.16, potRadius: 0.08, leaves: 26, seed: 5, floor: 0.58 });
  framedArt(scene, 2.35, 1.55, D0, 1.2, 0.86, { seed: 33, palette: ["#efe7da", "#d8b38a", "#5f7f86", "#e3c9a5", "#9fb3a6"] });
  box(scene, 1.4, 0.04, 0.3, oak, 2.35, 0.88, D0 + 0.16, { round: 0.008 });
  plant(scene, 3.55, D0 + 0.55, { kind: "leafy", height: 1.75, pot: "#ece8e1", potHeight: 0.5, potRadius: 0.25, leaves: 150, seed: 2, leaf: "#3e6b35" });
  plant(scene, -3.65, -3.8, { kind: "palm", height: 1.55, pot: "#3b3d40", potHeight: 0.45, potRadius: 0.22, leaves: 90, seed: 9, leaf: "#4a7a3d" });
  const tableZ = -3.25;
  const tableL = 3.6;
  const top = mesh(roundedBoxGeometry(1.15, 0.045, tableL, 0.02), oak);
  top.position.set(0, 0.74, tableZ);
  scene.add(top);
  const legMaterial = std("#2a2c2f", { roughness: 0.4, metalness: 0.6 });
  [-1, 1].forEach((s) => {
    const z = tableZ + s * (tableL / 2 - 0.45);
    [-1, 1].forEach((t) => box(scene, 0.05, 0.7, 0.05, legMaterial, t * 0.44, 0.36, z, { round: 0.008 }));
    box(scene, 0.93, 0.05, 0.05, legMaterial, 0, 0.69, z, { round: 0.008 });
  });
  box(scene, 0.05, 0.05, tableL - 0.9, legMaterial, 0, 0.69, tableZ, { round: 0.008 });
  for (let i = 0; i < 4; i += 1) {
    const z = tableZ - tableL / 2 + 0.55 + i * ((tableL - 1.1) / 3);
    swivelChair(scene, -0.86, z + range(-0.05, 0.05), Math.PI / 2 + range(-0.15, 0.15), { fabric: "#7d8a86" });
    swivelChair(scene, 0.86, z + range(-0.05, 0.05), -Math.PI / 2 + range(-0.15, 0.15), { fabric: "#7d8a86" });
  }
  const pendant = std("#1f2124", { roughness: 0.35, metalness: 0.5 });
  box(scene, 0.12, 0.05, 2.6, pendant, 0, 2.05, tableZ, { round: 0.01 });
  const glow = mesh(new THREE.PlaneGeometry(0.09, 2.55), new THREE.MeshStandardMaterial({ color: "#000000", emissive: "#fff3e0", emissiveIntensity: 4 }), { cast: false });
  glow.rotation.x = Math.PI / 2;
  glow.position.set(0, 2.024, tableZ);
  scene.add(glow);
  [-1, 1].forEach((s) => box(scene, 0.004, H - 2.075, 0.004, pendant, 0, (H + 2.075) / 2, tableZ + s * 1.1, { cast: false }));
  scene.environment = roomEnvironment(renderer, { walls: "#e9e5de", floor: "#b99a77", windowDir: [1, 0.15, -0.2], windowPower: 5 });
  scene.environmentIntensity = 0.55;
  scene.add(new THREE.HemisphereLight("#eef4fb", "#efe4d4", 0.75));
  const sun = shadowLight(new THREE.DirectionalLight("#fff1dc", 3.2), { extent: 7, far: 60 });
  sun.target.position.set(-1, 0, -3);
  scene.add(sun, sun.target);
  const sunBase = new THREE.Vector3(18, 14, 4);
  const spots = [0, 1].map(() => {
    const spot = shadowLight(new THREE.SpotLight("#e8f1ff", 16, 0, 1.25, 1, 2), { far: 20, size: 1024 });
    scene.add(spot, spot.target);
    return spot;
  });
  const ao = skyOcclusionLight(scene, { intensity: 0.9, target: [0, 0.8, -2.5], extent: 6.5, elevation: [0.25, 1] });
  const ceilingSpot = shadowLight(new THREE.SpotLight("#fff2df", 9, 0, 1.0, 1, 2), { far: 8, size: 1024 });
  ceilingSpot.position.set(0, 2.0, tableZ);
  ceilingSpot.target.position.set(0, 0, tableZ);
  scene.add(ceilingSpot, ceilingSpot.target);
  const jitters = [
    (r) => {
      sun.position.copy(sunBase).add(new THREE.Vector3((r() - 0.5) * 1.6, (r() - 0.5) * 1.6, (r() - 0.5) * 1.6));
      sun.shadow.needsUpdate = true;
    },
    (r) => spots.forEach((spot, k) => {
      const z = lerp(D0 + 0.3, D1 - 0.3, (k + r()) / 2);
      spot.position.set(W / 2 + 0.3, lerp(0.4, H - 0.2, r()), z);
      spot.target.position.set(-2, 0.6, z - 0.5);
      spot.shadow.needsUpdate = true;
    }),
    ao,
    (r) => {
      ceilingSpot.position.set(0, 2.0, tableZ + (r() - 0.5) * 2.4);
      ceilingSpot.target.position.set(0, 0, ceilingSpot.position.z);
    },
  ];
  const cam = camera(48, [0.15, 1.24, 1.6], [0.05, 1.18, -6.2]);
  renderer.toneMappingExposure = 0.95;
  return { scene, camera: cam, jitters, aperture: 0.035, focus: [0, 1.2, -3.2], grade: { blur: 1.4, vignette: 0.14, warmth: 0.04 } };
};

const BOOK_COLORS = ["#2d3e50", "#7a2e2e", "#c49a3a", "#2f5241", "#e8e0cc", "#5b5f66", "#1f1f22", "#a4583a", "#6d7f8f", "#8c6b4f", "#d7c7a3", "#3c4a6b", "#9a8f7a", "#4e3a2e"];

function bookSpineTexture() {
  const [canvas, ctx] = makeCanvas(64, 256);
  ctx.fillStyle = "#ffffff";
  ctx.fillRect(0, 0, 64, 256);
  ctx.fillStyle = "rgba(0,0,0,0.18)";
  ctx.fillRect(0, 22, 64, 6);
  ctx.fillRect(0, 226, 64, 6);
  ctx.fillStyle = "rgba(255,240,200,0.55)";
  ctx.fillRect(14, 60, 36, 3);
  ctx.fillRect(18, 70, 28, 2);
  ctx.fillRect(20, 190, 24, 2);
  const texture = canvasTexture(canvas);
  texture.wrapS = THREE.ClampToEdgeWrapping;
  texture.wrapT = THREE.ClampToEdgeWrapping;
  return texture;
}

function shelfBooks(parent, x0, x1, y, z, depth, { seed = 1, maxHeight = 0.3, decor = true } = {}) {
  setSeed(seed * 104729);
  const spines = bookSpineTexture();
  const material = new THREE.MeshStandardMaterial({ color: "#ffffff", map: spines, roughness: 0.75 });
  const geometry = new THREE.BoxGeometry(1, 1, 1);
  const uv = geometry.attributes.uv;
  for (let i = 0; i < uv.count; i += 1) uv.setXY(i, uv.getX(i), uv.getY(i));
  const items = [];
  let x = x0 + range(0.01, 0.06);
  while (x < x1 - 0.04) {
    const roll = rand();
    if (decor && roll < 0.07 && x1 - x > 0.3) {
      const vaseH = range(0.12, maxHeight * 0.75);
      const r = range(0.04, 0.07);
      lathe(parent, [[0, 0], [r, 0], [r * 1.1, vaseH * 0.4], [r * 0.6, vaseH * 0.85], [r * 0.7, vaseH], [0, vaseH]], std(pick(["#e9e3d8", "#3d5c57", "#b7714f", "#2b2b2e"]), { roughness: 0.35 }), x + r + 0.03, y, z, 32);
      x += r * 2 + range(0.08, 0.14);
      continue;
    }
    if (roll < 0.14 && x1 - x > 0.3) {
      const stack = Math.floor(range(3, 6));
      let sy = y;
      const w = range(0.17, 0.24);
      for (let k = 0; k < stack; k += 1) {
        const t = range(0.025, 0.045);
        items.push({ w, h: t, d: range(0.14, 0.2), x: x + w / 2, y: sy + t / 2, rz: 0, ry: range(-0.06, 0.06), lying: true });
        sy += t;
      }
      x += w + range(0.03, 0.08);
      continue;
    }
    const run = Math.floor(range(4, 14));
    for (let k = 0; k < run && x < x1 - 0.04; k += 1) {
      const w = range(0.018, 0.05);
      const h = range(0.6, 1) * maxHeight;
      items.push({ w, h, d: range(0.13, 0.21), x: x + w / 2, y: y + h / 2, rz: 0, ry: 0 });
      x += w + 0.002;
    }
    if (rand() < 0.3 && x < x1 - 0.12) {
      const w = range(0.022, 0.04);
      const h = range(0.7, 0.95) * maxHeight;
      const lean = range(0.18, 0.32);
      items.push({ w, h, d: range(0.14, 0.2), x: x + Math.sin(lean) * h / 2 + w / 2, y: y + Math.cos(lean) * h / 2 + Math.sin(lean) * w / 2, rz: -lean, ry: 0 });
      x += Math.sin(lean) * h + w + 0.01;
    }
    x += range(0.0, 0.05);
  }
  const instanced = new THREE.InstancedMesh(geometry, material, items.length);
  instanced.castShadow = true;
  instanced.receiveShadow = true;
  const dummy = new THREE.Object3D();
  const color = new THREE.Color();
  items.forEach((item, i) => {
    dummy.position.set(item.x, item.y, z + (depth - item.d) * -0.5 + depth * 0.5 - item.d / 2 + range(-0.01, 0.01));
    dummy.rotation.set(0, item.ry, item.rz);
    if (item.lying) dummy.scale.set(item.w, item.h, item.d);
    else dummy.scale.set(item.w, item.h, item.d);
    dummy.updateMatrix();
    instanced.setMatrixAt(i, dummy.matrix);
    color.set(pick(BOOK_COLORS)).offsetHSL(0, range(-0.05, 0.05), range(-0.05, 0.05));
    instanced.setColorAt(i, color);
  });
  parent.add(instanced);
  return instanced;
}

function cityWindowsTexture({ seed = 3, lit = 0.35, cols = 16, rows = 32 } = {}) {
  setSeed(seed);
  const size = 512;
  const [canvas, ctx] = makeCanvas(size, size * 2);
  ctx.fillStyle = "#000000";
  ctx.fillRect(0, 0, size, size * 2);
  const cw = size / cols;
  const ch = (size * 2) / rows;
  for (let r = 0; r < rows; r += 1) {
    const mode = rand();
    if (mode < 0.45) continue;
    const density = mode > 0.88 ? 0.85 : lit * 1.6;
    const warmFloor = rand() < 0.85;
    let c = 0;
    while (c < cols) {
      const runLength = Math.floor(range(2, 7));
      const on = rand() < density;
      const v = range(0.55, 1);
      for (let k = 0; k < runLength && c < cols; k += 1, c += 1) {
        if (!on) continue;
        ctx.fillStyle = warmFloor ? `rgba(255,${Math.round(200 + 25 * v)},${Math.round(140 + 30 * v)},${v})` : `rgba(${Math.round(215 * v)},${Math.round(230 * v)},255,${v * 0.8})`;
        ctx.fillRect(c * cw + cw * 0.08, r * ch + ch * 0.2, cw * 0.84, ch * 0.6);
      }
    }
  }
  const texture = canvasTexture(canvas);
  texture.magFilter = THREE.LinearFilter;
  return texture;
}

function facadeTexture(base) {
  const [canvas, ctx] = makeCanvas(256, 512);
  ctx.fillStyle = base;
  ctx.fillRect(0, 0, 256, 512);
  ctx.fillStyle = "rgba(255,255,255,0.05)";
  for (let r = 0; r < 32; r += 1) ctx.fillRect(0, r * 16 + 13, 256, 3);
  ctx.fillStyle = "rgba(0,0,0,0.25)";
  for (let c = 0; c < 16; c += 1) ctx.fillRect(c * 16, 0, 2, 512);
  return canvasTexture(canvas);
}

function buildingGeometry(w, h, d, floorHeight = 3.6, bay = 3.2) {
  const geometry = new THREE.BoxGeometry(w, h, d);
  const uv = geometry.attributes.uv;
  const normal = geometry.attributes.normal;
  for (let i = 0; i < uv.count; i += 1) {
    const nx = Math.abs(normal.getX(i));
    const ny = Math.abs(normal.getY(i));
    const span = ny > 0.5 ? 0 : nx > 0.5 ? d : w;
    uv.setXY(i, uv.getX(i) * (span / (bay * 16)), ny > 0.5 ? 0 : uv.getY(i) * (h / (floorHeight * 32)));
  }
  return geometry;
}

SCENES.skyline = async (renderer) => {
  const scene = new THREE.Scene();
  const W = 8;
  const Z0 = -3.4;
  const Z1 = 2.2;
  const H = 2.9;
  const altitude = 165;
  const floorMaterial = std("#ffffff", { map: woodTexture({ light: "#6e5440", dark: "#4a3628", planks: 10, repeat: [3, 3], seed: 19 }), roughness: 0.32 });
  floorMaterial.map.center.set(0.5, 0.5);
  floorMaterial.map.rotation = Math.PI / 2;
  box(scene, W, 0.1, Z1 - Z0, floorMaterial, 0, -0.05, (Z0 + Z1) / 2, { shell: true });
  box(scene, W, 0.1, Z1 - Z0, std("#e9e6e1", { roughness: 0.95, emissive: "#d8d2c8", emissiveIntensity: 0.05 }), 0, H + 0.05, (Z0 + Z1) / 2, { shell: true });
  const concrete = std("#ffffff", { map: grainTexture({ base: "#8d8a86", amount: 0.06, scale: 0.012, repeat: [2, 1], seed: 13 }), roughness: 0.85 });
  box(scene, 0.15, H, Z1 - Z0, concrete, -W / 2 - 0.075, H / 2, (Z0 + Z1) / 2, { shell: true });
  box(scene, 0.15, H, Z1 - Z0, concrete, W / 2 + 0.075, H / 2, (Z0 + Z1) / 2, { shell: true });
  const frame = std("#17191c", { roughness: 0.4, metalness: 0.6 });
  [-3.2, -1.6, 0, 1.6, 3.2].forEach((x) => box(scene, 0.07, H, 0.12, frame, x, H / 2, Z0, { shell: true }));
  box(scene, W, 0.12, 0.16, frame, 0, 0.06, Z0, { shell: true });
  box(scene, W, 0.22, 0.4, std("#e9e6e1", { roughness: 0.9 }), 0, H - 0.11, Z0 + 0.12, { shell: true });
  const glass = new THREE.Mesh(new THREE.PlaneGeometry(W, H), new THREE.MeshPhysicalMaterial({ color: "#bcd0e6", roughness: 0.04, transparent: true, opacity: 0.07, envMapIntensity: 0.4 }));
  glass.position.set(0, H / 2, Z0 - 0.02);
  scene.add(glass);
  const sky = new THREE.Mesh(new THREE.SphereGeometry(5000, 64, 32), new THREE.MeshBasicMaterial({
    map: gradientSkyTexture([[0, "#0e1a33"], [0.3, "#24365f"], [0.42, "#55577f"], [0.475, "#c47a6e"], [0.5, "#f2a35e"], [0.52, "#d6865c"], [0.6, "#3a3440"], [1, "#151318"]]),
    side: THREE.BackSide,
    fog: false,
  }));
  sky.position.y = 0;
  scene.add(sky);
  const glowCanvas = makeCanvas(512, 256);
  const g = glowCanvas[1].createRadialGradient(256, 256, 0, 256, 256, 256);
  g.addColorStop(0, "rgba(255,190,120,0.9)");
  g.addColorStop(0.35, "rgba(255,150,90,0.35)");
  g.addColorStop(1, "rgba(255,120,80,0)");
  glowCanvas[1].fillStyle = g;
  glowCanvas[1].fillRect(0, 0, 512, 256);
  const glow = new THREE.Mesh(new THREE.PlaneGeometry(3200, 1600), new THREE.MeshBasicMaterial({ map: canvasTexture(glowCanvas[0]), transparent: true, depthWrite: false, fog: false, blending: THREE.AdditiveBlending }));
  glow.position.set(-1400, -altitude + 10, -4300);
  glow.lookAt(0, 0, 0);
  scene.add(glow);
  scene.fog = new THREE.Fog("#6d6274", 500, 4200);
  const ground = mesh(new THREE.PlaneGeometry(12000, 12000), std("#14151a", { roughness: 0.9 }), { cast: false });
  ground.rotation.x = -Math.PI / 2;
  ground.position.y = -altitude;
  scene.add(ground);
  const windowTextures = [1, 2, 3, 4].map((seed) => cityWindowsTexture({ seed, lit: 0.12 + seed * 0.04 }));
  const facades = ["#4a5468", "#565c6a", "#5f6470", "#47505e", "#6a6e78"].map(facadeTexture);
  setSeed(777);
  const blocks = [];
  for (let gz = 0; gz < 34; gz += 1) {
    for (let gx = -26; gx <= 26; gx += 1) {
      const cx = gx * 95 + range(-12, 12);
      const cz = -140 - gz * 95 + range(-12, 12);
      if (rand() < 0.22 || (gz >= 3 && gz <= 4)) continue;
      const centrality = Math.exp(-Math.pow((cx + 250) / 1200, 2)) * Math.exp(-Math.pow((cz + 1400) / 1300, 2));
      const lots = rand() < 0.5 ? 1 : 2;
      for (let l = 0; l < lots; l += 1) {
        const w = range(28, 60);
        const d = range(28, 60);
        let h = 20 + Math.pow(rand(), 2.2) * 110 + centrality * Math.pow(rand(), 1.4) * 320;
        if (cz > -700) h = Math.min(h, (altitude - 25) * range(0.35, 1));
        blocks.push({ x: cx + (lots > 1 ? (l ? 22 : -22) : 0), z: cz + range(-8, 8), w: lots > 1 ? w * 0.7 : w, d, h });
      }
    }
  }
  blocks.forEach((b, i) => {
    const material = new THREE.MeshStandardMaterial({
      color: "#ffffff",
      map: facades[i % facades.length],
      emissive: "#ffffff",
      emissiveMap: windowTextures[i % windowTextures.length],
      emissiveIntensity: 1.25,
      roughness: 0.6,
      metalness: 0.2,
    });
    const building = new THREE.Mesh(buildingGeometry(b.w, b.h, b.d), material);
    building.position.set(b.x, -altitude + b.h / 2, b.z);
    scene.add(building);
    if (b.h > 180 && i % 2 === 0) {
      const crown = new THREE.Mesh(new THREE.CylinderGeometry(0.6, 0.6, 18, 8), new THREE.MeshBasicMaterial({ color: "#ff5a4a" }));
      crown.position.set(b.x, -altitude + b.h + 9, b.z);
      scene.add(crown);
    }
  });
  const lights = new THREE.InstancedMesh(new THREE.SphereGeometry(1.2, 6, 4), new THREE.MeshBasicMaterial({ color: "#ffc27a" }), 6000);
  const dummy = new THREE.Object3D();
  let count = 0;
  for (let gz = 0; gz < 34 && count < 6000; gz += 1) {
    for (let gx = -26; gx <= 26 && count < 6000; gx += 1) {
      for (let k = 0; k < 3 && count < 6000; k += 1) {
        dummy.position.set(gx * 95 + 47.5, -altitude + 6, -140 - gz * 95 + range(-40, 40));
        dummy.updateMatrix();
        lights.setMatrixAt(count, dummy.matrix);
        count += 1;
      }
    }
  }
  lights.count = count;
  scene.add(lights);
  plant(scene, -3.35, -2.7, { kind: "palm", height: 1.7, pot: "#2b2b2d", potHeight: 0.5, potRadius: 0.26, leaves: 110, seed: 4, leaf: "#3c5f37" });
  const leather = std("#5a3a27", { roughness: 0.55 });
  const chair = new THREE.Group();
  box(chair, 0.8, 0.16, 0.75, leather, 0, 0.36, 0, { round: 0.06 });
  const chairBack = box(chair, 0.8, 0.5, 0.14, leather, 0, 0.66, -0.34, { round: 0.06 });
  chairBack.rotation.x = -0.2;
  [-1, 1].forEach((s) => box(chair, 0.12, 0.22, 0.7, leather, s * 0.42, 0.5, 0, { round: 0.05 }));
  [-1, 1].forEach((s) => [-1, 1].forEach((t) => box(chair, 0.03, 0.28, 0.03, frame, s * 0.38, 0.14, t * 0.32)));
  chair.position.set(2.75, 0, -2.2);
  chair.rotation.y = -0.6;
  scene.add(chair);
  const lampPole = cylinder(scene, 0.012, 0.012, 1.5, frame, 3.45, 0.75, -2.9, 12);
  lampPole.castShadow = false;
  cylinder(scene, 0.16, 0.16, 0.015, frame, 3.45, 0.008, -2.9, 32);
  const shade = lathe(scene, [[0.12, 0], [0.2, 0], [0.16, 0.26], [0.1, 0.26]], new THREE.MeshStandardMaterial({ color: "#efe3cc", emissive: "#ffcf8a", emissiveIntensity: 0.9, side: THREE.DoubleSide, roughness: 0.9 }), 3.45, 1.45, -2.9, 48, { cast: false });
  const lampLight = new THREE.PointLight("#ffc98a", 4, 0, 2);
  lampLight.position.set(3.45, 1.55, -2.9);
  scene.add(lampLight);
  for (let i = 0; i < 3; i += 1) {
    const disc = mesh(new THREE.CircleGeometry(0.06, 24), new THREE.MeshBasicMaterial({ color: "#fff1d6" }), { cast: false });
    disc.rotation.x = Math.PI / 2;
    disc.position.set(-2.4 + i * 2.4, H - 0.002, -1.6);
    scene.add(disc);
    const spot = shadowLight(new THREE.SpotLight("#ffd9a8", 7, 0, 0.75, 0.9, 2), { far: 6, size: 1024 });
    spot.position.set(-2.4 + i * 2.4, H - 0.05, -1.6);
    spot.target.position.set(-2.4 + i * 2.4, 0, -2.0);
    scene.add(spot, spot.target);
  }
  scene.environment = roomEnvironment(renderer, { walls: "#3c3843", floor: "#2a2420", sky: "#e2d6c8", windowColor: "#f0a46a", windowPower: 2, windowDir: [0, 0, -1] });
  scene.environmentIntensity = 0.45;
  scene.add(new THREE.HemisphereLight("#8d93b8", "#5a4636", 0.45));
  const cityLight = new THREE.DirectionalLight("#ff9a66", 2.4);
  cityLight.position.set(-1400, 200, -4300);
  scene.add(cityLight);
  const cityFill = new THREE.DirectionalLight("#6f7fb8", 1.3);
  cityFill.position.set(800, 600, 1000);
  scene.add(cityFill);
  const dusk = shadowLight(new THREE.DirectionalLight("#ffb27a", 0.9), { extent: 6, far: 60 });
  dusk.target.position.set(0, 0, -1);
  scene.add(dusk, dusk.target);
  const ao = skyOcclusionLight(scene, { intensity: 0.35, color: "#c9c3d6", target: [0, 0.8, -1.2], extent: 5, elevation: [0.3, 1] });
  const jitters = [
    (r) => {
      dusk.position.set(-14 + (r() - 0.5) * 3, 4 + (r() - 0.5) * 2, -30 + (r() - 0.5) * 3);
      dusk.shadow.needsUpdate = true;
    },
    ao,
  ];
  const cam = camera(48, [0.1, 1.25, 1.7], [0.05, 1.2, -6]);
  renderer.toneMappingExposure = 1.05;
  return { scene, camera: cam, jitters, aperture: 0.03, focus: [0, 1.25, -3.4], grade: { blur: 1.4, vignette: 0.12 } };
};

SCENES.library = async (renderer) => {
  const scene = new THREE.Scene();
  const W = 6.4;
  const Z0 = -3.4;
  const Z1 = 2.2;
  const H = 2.7;
  const floorMaterial = std("#ffffff", { map: woodTexture({ light: "#a87a54", dark: "#7a5236", planks: 10, repeat: [3, 3], seed: 23 }), roughness: 0.45 });
  floorMaterial.map.center.set(0.5, 0.5);
  floorMaterial.map.rotation = Math.PI / 2;
  box(scene, W, 0.1, Z1 - Z0, floorMaterial, 0, -0.05, (Z0 + Z1) / 2, { shell: true });
  const paint = std("#ffffff", { map: grainTexture({ base: "#8b9a85", amount: 0.03, repeat: [2, 1], seed: 31 }), roughness: 0.9 });
  const sidePaint = std("#ffffff", { map: grainTexture({ base: "#e7dfd2", amount: 0.03, repeat: [2, 1], seed: 32 }), roughness: 0.9 });
  const winX0 = 1.05;
  const winX1 = 2.45;
  const sill = 0.75;
  const head = 2.35;
  box(scene, winX0 + W / 2, H, 0.14, paint, (-W / 2 + winX0) / 2, H / 2, Z0 - 0.07, { shell: true });
  box(scene, W / 2 - winX1, H, 0.14, paint, (winX1 + W / 2) / 2, H / 2, Z0 - 0.07, { shell: true });
  box(scene, winX1 - winX0, sill, 0.14, paint, (winX0 + winX1) / 2, sill / 2, Z0 - 0.07, { shell: true });
  box(scene, winX1 - winX0, H - head, 0.14, paint, (winX0 + winX1) / 2, (H + head) / 2, Z0 - 0.07, { shell: true });
  box(scene, 0.14, H, Z1 - Z0, sidePaint, -W / 2 - 0.07, H / 2, (Z0 + Z1) / 2, { shell: true });
  box(scene, 0.14, H, Z1 - Z0, sidePaint, W / 2 + 0.07, H / 2, (Z0 + Z1) / 2, { shell: true });
  box(scene, W, 0.1, Z1 - Z0, std("#f1ece4", { roughness: 0.95, emissive: "#efe7da", emissiveIntensity: 0.06 }), 0, H + 0.05, (Z0 + Z1) / 2, { shell: true });
  const trim = std("#f2ede4", { roughness: 0.6 });
  box(scene, W, 0.12, 0.02, trim, 0, 0.06, Z0 + 0.01, { shell: true });
  const frameWhite = std("#f4f1ea", { roughness: 0.5 });
  box(scene, winX1 - winX0 + 0.12, 0.05, 0.22, frameWhite, (winX0 + winX1) / 2, sill + 0.025, Z0 + 0.04);
  [winX0, winX1].forEach((x) => box(scene, 0.06, head - sill, 0.1, frameWhite, x, (sill + head) / 2, Z0 - 0.02, { shell: true }));
  box(scene, winX1 - winX0, 0.06, 0.1, frameWhite, (winX0 + winX1) / 2, head, Z0 - 0.02, { shell: true });
  box(scene, 0.04, head - sill, 0.06, frameWhite, (winX0 + winX1) / 2, (sill + head) / 2, Z0 - 0.04, { shell: true });
  box(scene, winX1 - winX0, 0.04, 0.06, frameWhite, (winX0 + winX1) / 2, 1.75, Z0 - 0.04, { shell: true });
  backdrop(scene, exteriorCanvas({ horizon: 0.58, seed: 15, sky: ["#8fb3d6", "#e6d2b8", "#f3dfc2"], buildings: false }), { width: 120, height: 60, x: 10, y: 6, z: -40, brightness: 1.3 });
  const linen = new THREE.MeshStandardMaterial({ color: "#efe6d6", roughness: 0.95, side: THREE.DoubleSide, transparent: true, opacity: 0.92 });
  [winX0 - 0.18, winX1 + 0.18].forEach((cx, k) => {
    const curtain = new THREE.PlaneGeometry(0.42, 2.5, 40, 1);
    const pos = curtain.attributes.position;
    for (let i = 0; i < pos.count; i += 1) pos.setZ(i, Math.sin(pos.getX(i) * 52 + k) * 0.03);
    curtain.computeVertexNormals();
    const m = mesh(curtain, linen);
    m.position.set(cx, 1.27, Z0 + 0.12);
    scene.add(m);
  });
  cylinder(scene, 0.012, 0.012, winX1 - winX0 + 1.0, std("#2b2724", { metalness: 0.7, roughness: 0.35 }), (winX0 + winX1) / 2, 2.53, Z0 + 0.12, 12).rotation.z = Math.PI / 2;
  const walnut = std("#ffffff", { map: woodTexture({ light: "#94664a", dark: "#664330", planks: 1, seams: false, seed: 41, stretch: 0.5 }), roughness: 0.5 });
  const shelfX0 = -3.1;
  const shelfX1 = -0.75;
  const shelfDepth = 0.36;
  const shelfZ = Z0 + shelfDepth / 2;
  const shelfTop = 2.45;
  box(scene, shelfX1 - shelfX0, shelfTop, 0.02, walnut, (shelfX0 + shelfX1) / 2, shelfTop / 2, Z0 + 0.01);
  [shelfX0, (shelfX0 + shelfX1) / 2, shelfX1].forEach((x) => box(scene, 0.035, shelfTop, shelfDepth, walnut, x, shelfTop / 2, shelfZ));
  const shelfLevels = [0.08, 0.5, 0.9, 1.28, 1.66, 2.04, shelfTop - 0.02];
  shelfLevels.forEach((y) => box(scene, shelfX1 - shelfX0 + 0.035, 0.035, shelfDepth, walnut, (shelfX0 + shelfX1) / 2, y, shelfZ));
  box(scene, shelfX1 - shelfX0, 0.08, 0.02, walnut, (shelfX0 + shelfX1) / 2, 0.04, Z0 + shelfDepth - 0.01);
  shelfLevels.slice(0, -1).forEach((y, level) => {
    const gap = shelfLevels[level + 1] - y - 0.06;
    const mid = (shelfX0 + shelfX1) / 2;
    shelfBooks(scene, shelfX0 + 0.02, mid - 0.02, y + 0.0175, Z0 + 0.02, shelfDepth - 0.04, { seed: level * 2 + 1, maxHeight: Math.min(0.32, gap) });
    shelfBooks(scene, mid + 0.02, shelfX1 - 0.02, y + 0.0175, Z0 + 0.02, shelfDepth - 0.04, { seed: level * 2 + 2, maxHeight: Math.min(0.32, gap) });
  });
  const credenzaX = 0.05;
  box(scene, 1.5, 0.56, 0.44, walnut, credenzaX, 0.34, Z0 + 0.23, { round: 0.01 });
  [-1, 1].forEach((s) => [-1, 1].forEach((t) => box(scene, 0.03, 0.06, 0.03, std("#1d1b19", { metalness: 0.6 }), credenzaX + s * 0.68, 0.03, Z0 + 0.23 + t * 0.17)));
  [-0.36, 0.36].forEach((dx) => box(scene, 0.006, 0.4, 0.004, std("#3a2a1e"), credenzaX + dx, 0.34, Z0 + 0.452));
  const lampX = credenzaX + 0.5;
  lathe(scene, [[0, 0], [0.09, 0], [0.11, 0.06], [0.1, 0.18], [0.05, 0.3], [0.02, 0.32], [0, 0.32]], std("#b8774a", { roughness: 0.3 }), lampX, 0.62, Z0 + 0.22, 48);
  lathe(scene, [[0.13, 0], [0.19, 0], [0.16, 0.24], [0.11, 0.24]], new THREE.MeshStandardMaterial({ color: "#f3e9d6", emissive: "#ffcf8f", emissiveIntensity: 1.1, roughness: 0.9, side: THREE.DoubleSide }), lampX, 0.94, Z0 + 0.22, 48, { cast: false });
  const lampLight = new THREE.PointLight("#ffc88a", 5, 0, 2);
  lampLight.position.set(lampX, 1.02, Z0 + 0.22);
  scene.add(lampLight);
  shelfBooks(scene, credenzaX - 0.6, credenzaX - 0.2, 0.62, Z0 + 0.04, 0.36, { seed: 77, maxHeight: 0.24, decor: false });
  lathe(scene, [[0, 0], [0.06, 0], [0.08, 0.1], [0.04, 0.22], [0.03, 0.3], [0, 0.3]], std("#2f4b45", { roughness: 0.25 }), credenzaX - 0.08, 0.62, Z0 + 0.2, 32);
  framedArt(scene, credenzaX + 0.05, 1.62, Z0, 0.72, 0.9, { seed: 52, frame: "#2a211b", palette: ["#efe6d6", "#b5653f", "#273a44", "#d4a85b", "#7f9283"] });
  const leather = std("#6b3f26", { roughness: 0.5 });
  const chair = new THREE.Group();
  box(chair, 0.82, 0.18, 0.78, leather, 0, 0.38, 0, { round: 0.07 });
  const back = box(chair, 0.82, 0.62, 0.16, leather, 0, 0.74, -0.36, { round: 0.07 });
  back.rotation.x = -0.18;
  [-1, 1].forEach((s) => box(chair, 0.14, 0.26, 0.76, leather, s * 0.45, 0.55, 0, { round: 0.06 }));
  [-1, 1].forEach((s) => [-1, 1].forEach((t) => box(chair, 0.035, 0.3, 0.035, std("#2b211b"), s * 0.38, 0.15, t * 0.32)));
  chair.position.set(2.35, 0, -1.95);
  chair.rotation.y = -0.75;
  scene.add(chair);
  plant(scene, -2.55, -1.55, { kind: "leafy", height: 1.5, pot: "#c9b9a3", potHeight: 0.42, potRadius: 0.21, leaves: 130, seed: 12, leaf: "#3b6334" });
  const rugCanvas = makeCanvas(1024, 768);
  const rc = rugCanvas[1];
  rc.fillStyle = "#b9a58a";
  rc.fillRect(0, 0, 1024, 768);
  rc.strokeStyle = "#8a5a3c";
  rc.lineWidth = 26;
  rc.strokeRect(40, 40, 944, 688);
  rc.strokeStyle = "#e9dcc6";
  rc.lineWidth = 8;
  rc.strokeRect(84, 84, 856, 600);
  const rugTexture = canvasTexture(rugCanvas[0]);
  const rug = mesh(new THREE.BoxGeometry(2.8, 0.012, 2.0), std("#ffffff", { map: rugTexture, roughness: 1 }), { cast: false });
  rug.position.set(0.2, 0.006, -1.6);
  scene.add(rug);
  scene.environment = roomEnvironment(renderer, { walls: "#8d8c78", floor: "#7a5236", sky: "#efe6d8", windowColor: "#ffe2b8", windowPower: 4, windowDir: [0.3, 0.1, -1] });
  scene.environmentIntensity = 0.45;
  scene.add(new THREE.HemisphereLight("#f4e8d8", "#b08e6e", 0.75));
  scene.add(fillLight([0, 1.2, 1.3], 0.5, "#fff1e0"));
  const sun = shadowLight(new THREE.DirectionalLight("#ffc88f", 3.4), { extent: 6, far: 60 });
  sun.target.position.set(0.3, 0, -1.6);
  scene.add(sun, sun.target);
  const sunBase = new THREE.Vector3(17.7, 18, -23.2);
  const windowSpot = shadowLight(new THREE.SpotLight("#ffe9cc", 10, 0, 1.2, 1, 2), { far: 12, size: 1024 });
  scene.add(windowSpot, windowSpot.target);
  const ao = skyOcclusionLight(scene, { intensity: 0.8, color: "#f1e6d6", target: [0, 0.8, -1.8], extent: 5, elevation: [0.25, 1] });
  const jitters = [
    (r) => {
      sun.position.copy(sunBase).add(new THREE.Vector3((r() - 0.5) * 1.2, (r() - 0.5) * 1.2, (r() - 0.5) * 1.2));
      sun.shadow.needsUpdate = true;
    },
    (r) => {
      windowSpot.position.set(lerp(winX0, winX1, r()), lerp(sill, head, r()), Z0 - 0.3);
      windowSpot.target.position.set(-0.5, 0.6, 0.5);
      windowSpot.shadow.needsUpdate = true;
    },
    ao,
  ];
  const cam = camera(48, [0.0, 1.2, 1.3], [-0.05, 1.18, -3.4]);
  renderer.toneMappingExposure = 1.15;
  return { scene, camera: cam, jitters, aperture: 0.025, focus: [0, 1.2, -3.0], grade: { blur: 1.3, vignette: 0.16, warmth: 0.05 } };
};

async function brandSignCanvas() {
  const font = await loadBrandFont();
  const logo = await loadImage("/www/public/assets/logo.svg");
  const [canvas, ctx] = makeCanvas(2048, 512);
  ctx.clearRect(0, 0, 2048, 512);
  const logoH = 420;
  const logoW = (logo.width / logo.height) * logoH;
  ctx.drawImage(logo, 40, 46, logoW, logoH);
  ctx.fillStyle = "#ffffff";
  ctx.font = `300px "${font}"`;
  ctx.textBaseline = "middle";
  ctx.fillText("ForkMesh", 40 + logoW + 70, 270);
  return canvas;
}

SCENES.office = async (renderer) => {
  const scene = new THREE.Scene();
  const W = 12;
  const Z0 = -8.5;
  const Z1 = 2.5;
  const H = 3.2;
  const concreteMaterial = std("#ffffff", { map: grainTexture({ base: "#bdbab4", amount: 0.07, scale: 0.008, repeat: [3, 3], seed: 61 }), roughness: 0.38 });
  box(scene, W, 0.1, Z1 - Z0, concreteMaterial, 0, -0.05, (Z0 + Z1) / 2, { shell: true });
  const wall = std("#ffffff", { map: grainTexture({ base: "#ece9e3", amount: 0.025, repeat: [3, 1], seed: 62 }), roughness: 0.9 });
  box(scene, W, H, 0.14, wall, 0, H / 2, Z0 - 0.07, { shell: true });
  box(scene, 0.14, H, Z1 - Z0, wall, -W / 2 - 0.07, H / 2, (Z0 + Z1) / 2, { shell: true });
  box(scene, W, 0.1, Z1 - Z0, std("#f4f3f0", { roughness: 0.95, emissive: "#f1eee8", emissiveIntensity: 0.12 }), 0, H + 0.05, (Z0 + Z1) / 2, { shell: true });
  const featureX0 = -5.9;
  const featureX1 = -0.7;
  box(scene, featureX1 - featureX0, H, 0.05, std("#1c1e21", { roughness: 0.8 }), (featureX0 + featureX1) / 2, H / 2, Z0 + 0.025, { shell: true });
  const slatMaterial = std("#ffffff", { map: woodTexture({ light: "#c99e70", dark: "#9f7148", planks: 1, seams: false, seed: 64, stretch: 0.3 }), roughness: 0.55 });
  const slatCount = Math.floor((featureX1 - featureX0) / 0.085);
  const slats = new THREE.InstancedMesh(new THREE.BoxGeometry(0.045, H, 0.035), slatMaterial, slatCount);
  slats.castShadow = true;
  slats.receiveShadow = true;
  const dummy = new THREE.Object3D();
  for (let i = 0; i < slatCount; i += 1) {
    dummy.position.set(featureX0 + 0.04 + i * 0.085, H / 2, Z0 + 0.07);
    dummy.updateMatrix();
    slats.setMatrixAt(i, dummy.matrix);
  }
  scene.add(slats);
  const signCanvas = await brandSignCanvas();
  const signTexture = canvasTexture(signCanvas);
  signTexture.wrapS = THREE.ClampToEdgeWrapping;
  signTexture.wrapT = THREE.ClampToEdgeWrapping;
  const signW = 2.8;
  const signX = -4.2;
  const signH = signW / 4;
  const signPanel = box(scene, signW + 0.5, signH + 0.36, 0.04, std("#17181b", { roughness: 0.5, metalness: 0.2 }), signX, 1.78, Z0 + 0.11, { round: 0.01 });
  signPanel.castShadow = true;
  const sign = mesh(new THREE.PlaneGeometry(signW, signH), new THREE.MeshStandardMaterial({ map: signTexture, emissive: "#ffffff", emissiveMap: signTexture, emissiveIntensity: 1.25, transparent: true, alphaTest: 0.05, roughness: 0.4 }), { cast: false });
  sign.position.set(signX, 1.78, Z0 + 0.135);
  scene.add(sign);
  const accent = mesh(new THREE.PlaneGeometry(signW + 0.5, 0.025), new THREE.MeshStandardMaterial({ color: "#000000", emissive: "#4ade80", emissiveIntensity: 1.6 }), { cast: false });
  accent.position.set(signX, 1.78 - (signH + 0.36) / 2 + 0.03, Z0 + 0.133);
  scene.add(accent);
  const mull = std("#26292d", { roughness: 0.4, metalness: 0.5 });
  for (let x = -0.7; x <= W / 2 + 0.01; x += 1.45) box(scene, 0.07, H, 0.12, mull, x, H / 2, Z0 + 0.02, { shell: true });
  box(scene, W / 2 + 0.7, 0.1, 0.16, mull, (W / 2 - 0.7) / 2, 0.05, Z0 + 0.02, { shell: true });
  box(scene, W / 2 + 0.7, 0.04, 0.08, mull, (W / 2 - 0.7) / 2, 2.4, Z0 + 0.02, { shell: true });
  const cut = new THREE.Mesh(new THREE.PlaneGeometry(W / 2 + 0.7, H), new THREE.MeshBasicMaterial({ colorWrite: false }));
  backdrop(scene, exteriorCanvas({ horizon: 0.6, seed: 27, sky: ["#7fb0dc", "#b9d6ec", "#e3eef4"] }), { width: 260, height: 90, x: 10, y: 10, z: -60, brightness: 1.2 });
  scene.children.filter((c) => c.isMesh && c.position.z === Z0 - 0.07 && c.geometry.parameters?.width === W).forEach((c) => {
    c.geometry.dispose();
    c.geometry = new THREE.BoxGeometry(W / 2 - 0.7, H, 0.14);
    c.position.x = -W / 2 + (W / 2 - 0.7) / 2;
  });
  void cut;
  const oak = std("#ffffff", { map: woodTexture({ light: "#dcc19c", dark: "#bb966e", planks: 1, seams: false, seed: 66, stretch: 0.4 }), roughness: 0.45 });
  const white = std("#f2f1ee", { roughness: 0.4 });
  const black = std("#1c1d20", { roughness: 0.4, metalness: 0.4 });
  const screenMaterial = new THREE.MeshStandardMaterial({ color: "#0b0c0e", roughness: 0.15, emissive: "#22344a", emissiveIntensity: 0.35 });
  const deskRow = (z, x0, x1, facing) => {
    const length = x1 - x0;
    box(scene, length, 0.035, 1.5, oak, (x0 + x1) / 2, 0.74, z, { round: 0.006 });
    for (let x = x0 + 0.05; x <= x1 - 0.04; x += length / Math.max(1, Math.round(length / 1.6))) {
      box(scene, 0.05, 0.72, 1.3, white, x, 0.36, z, { round: 0.006 });
    }
    box(scene, length - 0.1, 0.38, 0.02, std("#d9d6cf", { roughness: 0.95 }), (x0 + x1) / 2, 0.95, z, { round: 0.006 });
    const seats = Math.max(1, Math.round(length / 1.6));
    for (let i = 0; i < seats; i += 1) {
      const x = x0 + (i + 0.5) * (length / seats);
      [-1, 1].forEach((side) => {
        const mz = z + side * 0.42;
        const monitor = new THREE.Group();
        box(monitor, 0.62, 0.37, 0.025, black, 0, 0, 0, { round: 0.006 });
        const panel = mesh(new THREE.PlaneGeometry(0.6, 0.34), screenMaterial, { cast: false });
        panel.position.z = side > 0 ? -0.0135 : 0.0135;
        panel.rotation.y = side > 0 ? Math.PI : 0;
        monitor.add(panel);
        box(monitor, 0.03, 0.2, 0.03, black, 0, -0.22, 0.02 * side);
        box(monitor, 0.2, 0.012, 0.14, black, 0, -0.31, 0.02 * side);
        monitor.position.set(x + range(-0.08, 0.08), 1.07, mz - side * 0.0);
        scene.add(monitor);
        swivelChair(scene, x + range(-0.15, 0.15), z + side * 1.05, (side > 0 ? Math.PI : 0) + range(-0.35, 0.35), { fabric: "#2f6a47" });
      });
    }
    void facing;
  };
  deskRow(-4.6, 0.6, 5.4);
  box(scene, 3.0, 0.62, 0.5, oak, signX, 0.31, Z0 + 0.4, { round: 0.01 });
  plant(scene, -5.55, Z0 + 1.6, { kind: "leafy", height: 2.0, pot: "#1f2023", potHeight: 0.55, potRadius: 0.28, leaves: 170, seed: 31, leaf: "#3d6a36" });
  plant(scene, -1.3, Z0 + 0.6, { kind: "blade", height: 1.0, pot: "#1f2023", potHeight: 0.5, potRadius: 0.22, leaves: 40, seed: 32, leaf: "#40693a" });
  plant(scene, 5.4, -6.8, { kind: "palm", height: 1.9, pot: "#efece6", potHeight: 0.55, potRadius: 0.28, leaves: 120, seed: 33, leaf: "#47753c" });
  plant(scene, -5.4, -2.6, { kind: "leafy", height: 1.4, pot: "#efece6", potHeight: 0.45, potRadius: 0.24, leaves: 120, seed: 34, leaf: "#3f6c38" });
  const pendantMat = std("#1f2124", { roughness: 0.35, metalness: 0.5 });
  [-3.2, 0.2, 3.2].forEach((x) => {
    box(scene, 0.1, 0.05, Z1 - Z0 - 2, pendantMat, x, H - 0.45, (Z0 + Z1) / 2 - 0.5, { round: 0.01, cast: false });
    const strip = mesh(new THREE.PlaneGeometry(0.075, Z1 - Z0 - 2.05), new THREE.MeshStandardMaterial({ color: "#000000", emissive: "#fff6ea", emissiveIntensity: 3.5 }), { cast: false });
    strip.rotation.x = Math.PI / 2;
    strip.position.set(x, H - 0.476, (Z0 + Z1) / 2 - 0.5);
    scene.add(strip);
  });
  scene.environment = roomEnvironment(renderer, { walls: "#dcd9d3", floor: "#a9a6a0", windowDir: [0.4, 0.1, -1], windowPower: 5 });
  scene.environmentIntensity = 0.55;
  scene.add(new THREE.HemisphereLight("#eef3fa", "#e6e0d6", 0.7));
  const sun = shadowLight(new THREE.DirectionalLight("#fff0da", 3.0), { extent: 9, far: 80 });
  sun.target.position.set(1, 0, -5);
  scene.add(sun, sun.target);
  const sunBase = new THREE.Vector3(16, 18, -28);
  const spots = [0, 1, 2].map(() => {
    const spot = shadowLight(new THREE.SpotLight("#e9f1ff", 22, 0, 1.2, 1, 2), { far: 25, size: 1024 });
    scene.add(spot, spot.target);
    return spot;
  });
  const ceilingSpots = [-3.2, 0.2, 3.2].map((x) => {
    const spot = shadowLight(new THREE.SpotLight("#fff2e0", 9, 0, 1.15, 1, 2), { far: 8, size: 1024 });
    spot.position.set(x, H - 0.5, -3);
    spot.target.position.set(x, 0, -3);
    scene.add(spot, spot.target);
    return spot;
  });
  const ao = skyOcclusionLight(scene, { intensity: 0.85, target: [0, 0.8, -4], extent: 9, radius: 18, elevation: [0.25, 1] });
  const jitters = [
    (r) => {
      sun.position.copy(sunBase).add(new THREE.Vector3((r() - 0.5) * 2, (r() - 0.5) * 2, (r() - 0.5) * 2));
      sun.shadow.needsUpdate = true;
    },
    (r) => spots.forEach((spot, k) => {
      const x = lerp(-0.6, W / 2 - 0.2, (k + r()) / 3);
      spot.position.set(x, lerp(0.4, H - 0.3, r()), Z0 - 0.4);
      spot.target.position.set(x - 1.5, 0.5, -2);
      spot.shadow.needsUpdate = true;
    }),
    (r) => ceilingSpots.forEach((spot) => {
      const z = lerp(Z0 + 0.5, Z1 - 1.5, r());
      spot.position.z = z;
      spot.target.position.z = z;
      spot.shadow.needsUpdate = true;
    }),
    ao,
  ];
  const cam = camera(48, [0.2, 1.25, 1.9], [0.0, 1.2, -8.5]);
  renderer.toneMappingExposure = 0.95;
  return { scene, camera: cam, jitters, aperture: 0.035, focus: [0, 1.2, -5], grade: { blur: 1.4, vignette: 0.14, warmth: 0.03 } };
};

export async function renderStem(stem, { samples = 40 } = {}) {
  const renderer = createRenderer();
  const built = await SCENES[stem](renderer);
  const started = performance.now();
  const raw = await accumulate(renderer, built.scene, built.camera, {
    samples: built.samples || samples,
    aperture: built.aperture,
    focus: built.focus,
    jitters: built.jitters,
  });
  const graded = grade(raw, built.grade || {});
  const result = encode(graded, built.encode || {});
  result.renderMs = Math.round(performance.now() - started);
  renderer.dispose();
  return result;
}

function worldSkyTexture({ horizon = 0.56 } = {}) {
  setSeed(4242);
  const [canvas, ctx] = makeCanvas(1920, 1080);
  const h = 1080 * horizon;
  const gradient = ctx.createLinearGradient(0, 0, 0, 1080);
  gradient.addColorStop(0, "#3d78b8");
  gradient.addColorStop(horizon * 0.55, "#7fb0da");
  gradient.addColorStop(horizon * 0.92, "#cfe2ee");
  gradient.addColorStop(horizon, "#e4eef2");
  gradient.addColorStop(1, "#e4eef2");
  ctx.fillStyle = gradient;
  ctx.fillRect(0, 0, 1920, 1080);
  for (let i = 0; i < 46; i += 1) {
    const cy = Math.pow(rand(), 1.6) * h * 0.8 + h * 0.04;
    const cx = rand() * 1920;
    const depth = cy / h;
    const width = range(160, 520) * (0.5 + depth);
    const height = width * range(0.06, 0.12);
    for (let k = 0; k < 18; k += 1) {
      const x = cx + (rand() - 0.5) * width;
      const y = cy + (rand() - 0.5) * height;
      const r = height * range(0.6, 1.4);
      const puff = ctx.createRadialGradient(x, y, 0, x, y, r);
      const alpha = 0.06 + 0.1 * (1 - depth);
      puff.addColorStop(0, `rgba(255,255,255,${alpha})`);
      puff.addColorStop(1, "rgba(255,255,255,0)");
      ctx.save();
      ctx.translate(x, y);
      ctx.scale(2.6, 1);
      ctx.translate(-x, -y);
      ctx.fillStyle = puff;
      ctx.fillRect(x - r, y - r, r * 2, r * 2);
      ctx.restore();
    }
  }
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

export async function renderWorld(world, { samples = 24 } = {}) {
  const renderer = world.renderer;
  const scene = world.scene;
  world.setDaylightMode("auto");
  world.setTheme("world");
  world.setSpawn({ x: 40, z: -140 });
  await new Promise((resolve) => setTimeout(resolve, 6000));
  renderer.setAnimationLoop(null);
  world.player.visible = false;
  scene.traverse((object) => {
    if (/^forkmesh-office-floor-(marketing|engineering|rooftop)$/.test(object.name)) object.visible = true;
  });
  scene.background = worldSkyTexture();
  if (scene.fog) scene.fog.color.set("#d8e7ef");
  const cam = world.camera;
  cam.fov = 45;
  cam.near = 0.3;
  cam.far = 3000;
  cam.aspect = WIDTH / HEIGHT;
  cam.clearViewOffset();
  cam.updateProjectionMatrix();
  const from = new THREE.Vector3(90, 9, -60);
  const yaw = Math.atan2(0 - from.x, -209 - from.z) + (24 * Math.PI) / 180;
  const pitch = (3 * Math.PI) / 180;
  cam.position.copy(from);
  cam.lookAt(from.x + Math.sin(yaw) * Math.cos(pitch), from.y + Math.sin(pitch), from.z + Math.cos(yaw) * Math.cos(pitch));
  cam.updateMatrixWorld(true);
  scene.updateMatrixWorld(true);
  const started = performance.now();
  const raw = await accumulate(renderer, scene, cam, { samples, aperture: 0.35, focus: [20, 8, -185] });
  const graded = grade(raw, { blur: 1.6, vignette: 0.12, warmth: 0.05, saturation: 0.92 });
  const result = encode(graded, {});
  result.renderMs = Math.round(performance.now() - started);
  return result;
}
