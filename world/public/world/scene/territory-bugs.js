// Bugs for an instance's terrarium: one bug per site with bug reports, its
// favicon on a little badge above it. The body type, colour, size and the way
// it moves all come from the domain, so a site always gets the same bug.
import { deterministicFraction } from "./layout.js";
import * as resources from "./resources.js";

const ATLAS_CELL = 128;
const ATLAS_COLUMNS = 8;
const ATLAS_SIZE = ATLAS_CELL * ATLAS_COLUMNS;
export const BUG_BADGE_SLOTS = ATLAS_COLUMNS * ATLAS_COLUMNS;
const BADGE_SIZE = 1.1;
const BADGE_GAP = 0.45;
const FLUSH_MS = 120;
const DARK = "#161616";

// Each kind: body colours, the [min, max] of size and walking speed, how it
// moves ("walk", "dash" = stop-and-go, "fly"), its turning and leg rate.
export const BUG_KINDS = Object.freeze({
  beetle: Object.freeze({
    colors: ["#2d6a4f", "#1d3557", "#6a4c93", "#8d5524", "#0f766e"],
    size: [0.95, 1.25], speed: [0.45, 0.85], motion: "walk", turn: 1.4, gait: 11,
  }),
  stag: Object.freeze({
    colors: ["#4a2c1d", "#2b2118", "#3d2b1f", "#5c3d2e"],
    size: [1.05, 1.35], speed: [0.35, 0.7], motion: "walk", turn: 1.1, gait: 9,
  }),
  ant: Object.freeze({
    colors: ["#3b2416", "#7a1f1f", "#1c1c1c", "#b5651d"],
    size: [0.7, 0.95], speed: [1.5, 2.3], motion: "walk", turn: 2.8, gait: 22,
  }),
  spider: Object.freeze({
    colors: ["#2b2b2b", "#5a3e2b", "#3d3d5c", "#6b705c"],
    size: [0.9, 1.3], speed: [1.8, 2.8], motion: "dash", turn: 2, gait: 18,
  }),
  ladybug: Object.freeze({
    colors: ["#d62828", "#f77f00", "#e5383b", "#ffb703"],
    size: [0.75, 1], speed: [0.4, 0.7], motion: "dash", turn: 1.6, gait: 14,
  }),
  moth: Object.freeze({
    colors: ["#c2b280", "#9d8189", "#e9edc9", "#a47148", "#b8c0ff"],
    size: [0.9, 1.2], speed: [1, 1.6], motion: "fly", turn: 1.2, gait: 0,
  }),
});
const KIND_NAMES = Object.freeze(Object.keys(BUG_KINDS));

function between([low, high], fraction) {
  return low + (high - low) * fraction;
}

export function bugTraits(domain) {
  const key = String(domain || "");
  const kind = KIND_NAMES[Math.floor(deterministicFraction(`bug-kind:${key}`) * KIND_NAMES.length)];
  const spec = BUG_KINDS[kind];
  const variant = Math.floor(deterministicFraction(`bug-color:${key}`) * spec.colors.length);
  const fly = spec.motion === "fly";
  return {
    kind,
    variant,
    color: spec.colors[variant],
    motion: spec.motion,
    scale: between(spec.size, deterministicFraction(`bug-size:${key}`)),
    speed: between(spec.speed, deterministicFraction(`bug-speed:${key}`)),
    turn: spec.turn,
    gait: spec.gait,
    hue: Math.floor(deterministicFraction(`bug-hue:${key}`) * 360),
    altitude: fly ? 2 + deterministicFraction(`bug-alt:${key}`) * 3.5 : 0,
  };
}

export function bugMonogram(domain) {
  const parts = String(domain || "").toLowerCase().split(".").filter(Boolean);
  const name = parts.find((part, index) =>
    !["www", "m", "app"].includes(part) || index === parts.length - 2) || "?";
  return name.charAt(0).toUpperCase() || "?";
}

// --- geometry --------------------------------------------------------------

function shade(color, amount) {
  const value = parseInt(color.slice(1), 16);
  const channel = (shift) => {
    const c = (value >> shift) & 255;
    const next = amount < 0 ? c * (1 + amount) : c + (255 - c) * amount;
    return Math.max(0, Math.min(255, Math.round(next)));
  };
  return `#${[16, 8, 0].map((shift) => channel(shift).toString(16).padStart(2, "0")).join("")}`;
}

function part(THREE, geometry, color, position = [0, 0, 0], scale = [1, 1, 1], rotation = [0, 0, 0]) {
  const flat = geometry.index ? geometry.toNonIndexed() : geometry;
  flat.applyMatrix4(new THREE.Matrix4().compose(
    new THREE.Vector3(...position),
    new THREE.Quaternion().setFromEuler(new THREE.Euler(...rotation)),
    new THREE.Vector3(...scale),
  ));
  if (flat !== geometry) geometry.dispose();
  const tint = new THREE.Color(color);
  const count = flat.attributes.position.count;
  const colors = new Float32Array(count * 3);
  for (let index = 0; index < count; index += 1) {
    colors[index * 3] = tint.r;
    colors[index * 3 + 1] = tint.g;
    colors[index * 3 + 2] = tint.b;
  }
  const out = {
    position: flat.attributes.position.array,
    normal: flat.attributes.normal.array,
    color: colors,
  };
  flat.dispose();
  return out;
}

export function mergeParts(THREE, parts) {
  const size = parts.reduce((sum, item) => sum + item.position.length, 0);
  const position = new Float32Array(size);
  const normal = new Float32Array(size);
  const color = new Float32Array(size);
  let offset = 0;
  parts.forEach((item) => {
    position.set(item.position, offset);
    normal.set(item.normal, offset);
    color.set(item.color, offset);
    offset += item.position.length;
  });
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(position, 3));
  geometry.setAttribute("normal", new THREE.BufferAttribute(normal, 3));
  geometry.setAttribute("color", new THREE.BufferAttribute(color, 3));
  geometry.computeBoundingSphere();
  return geometry;
}

const sphere = (THREE, radius, w = 12, h = 9) => new THREE.SphereGeometry(radius, w, h);
const box = (THREE, x, y, z) => new THREE.BoxGeometry(x, y, z);

// One leg: hinged on the body at `hinge`, reaching out to `side` (+1 right,
// -1 left), swept forward or back by `sweep` and dropping to the soil.
function leg(THREE, hinge, side, sweep, length, color, thickness = 0.05) {
  const geometry = box(THREE, length, thickness, thickness);
  geometry.translate((side * length) / 2, 0, 0);
  const drop = Math.atan2(hinge[1] - 0.03, length * 0.92);
  return part(THREE, geometry, color, hinge, [1, 1, 1], [0, side * sweep, -side * drop]);
}

function legSet(THREE, pairs, color, thickness) {
  const parts = [];
  pairs.forEach(([hinge, sweep, length]) => {
    [-1, 1].forEach((side) => {
      parts.push(leg(THREE, [side * hinge[0], hinge[1], hinge[2]], side, sweep, length, color, thickness));
    });
  });
  return parts;
}

function antennae(THREE, z, y, color, length = 0.5, spread = 0.45, lift = 0.35) {
  return [-1, 1].map((side) => {
    const geometry = box(THREE, 0.03, 0.03, length);
    geometry.translate(0, 0, length / 2);
    return part(THREE, geometry, color, [side * 0.08, y, z], [1, 1, 1], [-lift, side * spread, 0]);
  });
}

function domeY(x, z, radius, height, depth) {
  const inside = 1 - (x / radius) ** 2 - (z / depth) ** 2;
  return Math.sqrt(Math.max(0, inside)) * height;
}

function beetleParts(THREE, color, stag) {
  const dark = shade(color, -0.55);
  const parts = [
    part(THREE, sphere(THREE, 0.5), color, [0, 0.38, -0.12], [0.95, 0.58, 1.25]),
    part(THREE, box(THREE, 0.035, 0.03, 1.1), dark, [0, 0.66, -0.16]),
    part(THREE, sphere(THREE, 0.32), shade(color, -0.3), [0, 0.37, 0.52], [1.15, 0.7, 0.8]),
    part(THREE, sphere(THREE, 0.2, 10, 8), DARK, [0, 0.31, 0.82]),
    ...antennae(THREE, 0.92, 0.36, DARK, stag ? 0.3 : 0.5),
  ];
  if (stag) {
    [-1, 1].forEach((side) => {
      const jaw = box(THREE, 0.07, 0.07, 0.55);
      jaw.translate(0, 0, 0.27);
      parts.push(part(THREE, jaw, shade(color, 0.15), [side * 0.12, 0.3, 0.9], [1, 1, 1], [0.1, -side * 0.38, 0]));
      const tip = box(THREE, 0.06, 0.06, 0.22);
      tip.translate(0, 0, 0.1);
      parts.push(part(THREE, tip, shade(color, 0.15), [side * 0.3, 0.32, 1.38], [1, 1, 1], [0, side * 0.9, 0]));
    });
  }
  return parts;
}

function antParts(THREE, color) {
  const dark = shade(color, -0.35);
  return [
    part(THREE, sphere(THREE, 0.38), color, [0, 0.42, -0.58], [0.85, 0.75, 1.15]),
    part(THREE, sphere(THREE, 0.09, 8, 6), dark, [0, 0.36, -0.14]),
    part(THREE, sphere(THREE, 0.2), color, [0, 0.4, 0.15], [0.8, 0.75, 1.4]),
    part(THREE, sphere(THREE, 0.24), dark, [0, 0.46, 0.6], [1, 0.85, 0.95]),
    ...antennae(THREE, 0.75, 0.55, dark, 0.55, 0.5, 0.55),
  ];
}

function spiderParts(THREE, color, mark) {
  return [
    part(THREE, sphere(THREE, 0.48), color, [0, 0.56, -0.45], [1, 0.85, 1.15]),
    part(THREE, sphere(THREE, 0.14, 10, 6), mark, [0, 0.97, -0.45], [1, 0.3, 1.6]),
    part(THREE, sphere(THREE, 0.3), shade(color, -0.2), [0, 0.45, 0.2], [1, 0.7, 1.1]),
    ...[-1, 1].map((side) =>
      part(THREE, sphere(THREE, 0.055, 6, 5), "#e0fbfc", [side * 0.09, 0.56, 0.47])),
  ];
}

function ladybugParts(THREE, color) {
  const radius = 0.55;
  const height = 0.42;
  const depth = 0.6;
  const parts = [
    part(THREE, new THREE.SphereGeometry(radius, 16, 10, 0, Math.PI * 2, 0, Math.PI / 2),
      color, [0, 0.12, -0.05], [1, height / radius, depth / radius]),
    part(THREE, box(THREE, 0.03, 0.03, 1.05), DARK, [0, 0.12 + height, -0.05]),
    part(THREE, sphere(THREE, 0.22, 10, 8), DARK, [0, 0.2, 0.55], [1, 0.7, 0.8]),
    ...[-1, 1].map((side) =>
      part(THREE, sphere(THREE, 0.05, 6, 5), "#fdfffc", [side * 0.12, 0.27, 0.66])),
  ];
  [[0.22, 0.18], [0.3, -0.12], [0.16, -0.36], [0.06, 0.32]].forEach(([x, z]) => {
    [-1, 1].forEach((side) => {
      const sx = side * x;
      if (Math.abs(sx) < 0.05 && side > 0) return;
      const y = 0.12 + domeY(sx, z, radius, height, depth);
      parts.push(part(THREE, sphere(THREE, 0.085, 8, 6), DARK, [sx, y - 0.01, z - 0.05], [1, 0.35, 1]));
    });
  });
  return parts;
}

function mothParts(THREE, color) {
  const body = shade(color, -0.45);
  return [
    part(THREE, sphere(THREE, 0.17), body, [0, 0, -0.15], [1, 1, 3]),
    part(THREE, sphere(THREE, 0.2), shade(color, -0.2), [0, 0.02, 0.28]),
    part(THREE, sphere(THREE, 0.12, 8, 6), body, [0, 0, 0.52]),
    ...antennae(THREE, 0.58, 0.06, body, 0.42, 0.55, 0.3),
  ];
}

function wingParts(THREE, color, side) {
  const shape = new THREE.Shape();
  shape.moveTo(0, 0.3);
  shape.quadraticCurveTo(0.6, 0.75, 1.05, 0.4);
  shape.quadraticCurveTo(1.15, -0.1, 0.75, -0.45);
  shape.quadraticCurveTo(0.35, -0.65, 0, -0.3);
  shape.lineTo(0, 0.3);
  const spot = new THREE.CircleGeometry(0.13, 12);
  return [
    part(THREE, new THREE.ShapeGeometry(shape, 6), color, [0, 0, 0], [side, 1, 1], [Math.PI / 2, 0, 0]),
    part(THREE, spot, shade(color, -0.5), [side * 0.68, 0.01, 0.05], [1, 1, 1], [Math.PI / 2, 0, 0]),
  ];
}

function bodyGeometry(THREE, kind, color) {
  return resources.geometry(THREE, `territory-bug-body:${kind}:${color}`, () => {
    if (kind === "beetle" || kind === "stag") return mergeParts(THREE, beetleParts(THREE, color, kind === "stag"));
    if (kind === "ant") return mergeParts(THREE, antParts(THREE, color));
    if (kind === "spider") return mergeParts(THREE, spiderParts(THREE, color, color === "#2b2b2b" ? "#d62828" : "#f4a261"));
    if (kind === "ladybug") return mergeParts(THREE, ladybugParts(THREE, color));
    return mergeParts(THREE, mothParts(THREE, color));
  });
}

function legGeometry(THREE, kind) {
  return resources.geometry(THREE, `territory-bug-legs:${kind}`, () => {
    if (kind === "spider") {
      return mergeParts(THREE, legSet(THREE, [
        [[0.18, 0.5, 0.36], -0.75, 1.35],
        [[0.22, 0.5, 0.24], -0.25, 1.25],
        [[0.22, 0.5, 0.12], 0.25, 1.2],
        [[0.18, 0.5, 0.02], 0.7, 1.3],
      ], DARK, 0.06));
    }
    if (kind === "ant") {
      return mergeParts(THREE, legSet(THREE, [
        [[0.1, 0.38, 0.28], -0.6, 0.85],
        [[0.12, 0.38, 0.15], 0, 0.85],
        [[0.1, 0.38, 0.04], 0.6, 0.95],
      ], DARK, 0.04));
    }
    const ladybug = kind === "ladybug";
    return mergeParts(THREE, legSet(THREE, [
      [[0.3, ladybug ? 0.16 : 0.26, 0.38], -0.55, ladybug ? 0.45 : 0.75],
      [[0.36, ladybug ? 0.16 : 0.26, 0], 0, ladybug ? 0.45 : 0.8],
      [[0.3, ladybug ? 0.16 : 0.26, -0.38], 0.55, ladybug ? 0.48 : 0.85],
    ], DARK, 0.05));
  });
}

function wingGeometry(THREE, color, side) {
  return resources.geometry(THREE, `territory-bug-wing:${color}:${side}`, () =>
    mergeParts(THREE, wingParts(THREE, color, side)));
}

function bodyTop(kind) {
  return { beetle: 0.68, stag: 0.68, ant: 0.7, spider: 1, ladybug: 0.55, moth: 0.3 }[kind] || 0.7;
}

// --- favicon badges ----------------------------------------------------------

function roundedRect(context, x, y, size, radius) {
  context.beginPath();
  context.moveTo(x + radius, y);
  context.arcTo(x + size, y, x + size, y + size, radius);
  context.arcTo(x + size, y + size, x, y + size, radius);
  context.arcTo(x, y + size, x, y, radius);
  context.arcTo(x, y, x + size, y, radius);
  context.closePath();
}

// A favicon that decoded but shows nothing (a blank, fully transparent or
// one-pixel image) gets the monogram instead.
export function faviconUsable(image) {
  const width = image?.naturalWidth || 0;
  const height = image?.naturalHeight || 0;
  if (width < 8 || height < 8) return false;
  try {
    const probe = document.createElement("canvas");
    probe.width = 24;
    probe.height = 24;
    const context = probe.getContext("2d", { willReadFrequently: true });
    context.drawImage(image, 0, 0, 24, 24);
    const pixels = context.getImageData(0, 0, 24, 24).data;
    let visible = 0;
    for (let index = 3; index < pixels.length; index += 4) {
      if (pixels[index] > 24) visible += 1;
    }
    return visible >= 20;
  } catch (_) {
    return true;
  }
}

function drawBadge(context, cell, { image = null, domain = "", hue = 0, ring = "#e74c3c" } = {}) {
  const x0 = (cell % ATLAS_COLUMNS) * ATLAS_CELL;
  const y0 = Math.floor(cell / ATLAS_COLUMNS) * ATLAS_CELL;
  const inset = 8;
  const size = ATLAS_CELL - inset * 2;
  context.clearRect(x0, y0, ATLAS_CELL, ATLAS_CELL);
  context.save();
  roundedRect(context, x0 + inset, y0 + inset, size, 26);
  context.fillStyle = image ? "#ffffff" : `hsl(${hue} 52% 38%)`;
  context.fill();
  context.lineWidth = 7;
  context.strokeStyle = ring;
  context.stroke();
  roundedRect(context, x0 + inset + 4, y0 + inset + 4, size - 8, 22);
  context.clip();
  if (image) {
    const width = image.naturalWidth || 64;
    const height = image.naturalHeight || 64;
    const box = 76;
    // Small icons scale up by whole steps without smoothing so their pixels
    // stay sharp; larger ones are smoothed down into the box.
    const small = Math.max(width, height) < 48;
    const scale = small
      ? Math.max(1, Math.floor(box / Math.max(width, height)))
      : Math.min(box / width, box / height);
    context.imageSmoothingEnabled = !small;
    context.imageSmoothingQuality = "high";
    const drawWidth = width * scale;
    const drawHeight = height * scale;
    context.drawImage(
      image,
      Math.round(x0 + (ATLAS_CELL - drawWidth) / 2),
      Math.round(y0 + (ATLAS_CELL - drawHeight) / 2),
      drawWidth,
      drawHeight,
    );
  } else {
    context.fillStyle = "#ffffff";
    context.font = `800 ${Math.round(ATLAS_CELL * 0.5)}px system-ui, sans-serif`;
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillText(bugMonogram(domain), x0 + ATLAS_CELL / 2, y0 + ATLAS_CELL / 2 + 4);
  }
  context.restore();
}

function badgeGeometry(THREE, cell) {
  return resources.geometry(THREE, `territory-bug-badge:${cell}`, () => {
    const geometry = new THREE.PlaneGeometry(1, 1);
    const column = cell % ATLAS_COLUMNS;
    const row = Math.floor(cell / ATLAS_COLUMNS);
    const step = 1 / ATLAS_COLUMNS;
    const uv = geometry.attributes.uv;
    for (let index = 0; index < uv.count; index += 1) {
      uv.setXY(
        index,
        (column + uv.getX(index)) * step,
        1 - (row + 1 - uv.getY(index)) * step,
      );
    }
    return geometry;
  });
}

function makeAtlas(THREE) {
  const canvas = document.createElement("canvas");
  canvas.width = ATLAS_SIZE;
  canvas.height = ATLAS_SIZE;
  const context = canvas.getContext("2d");
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.anisotropy = 4;
  const material = resources.uniqueMaterial(THREE, {
    map: texture,
    transparent: true,
    alphaTest: 0.04,
    depthWrite: false,
    toneMapped: false,
  }, "MeshBasicMaterial");
  let timer = 0;
  return {
    material,
    draw(cell, badge) {
      drawBadge(context, cell, badge);
      if (timer) return;
      timer = setTimeout(() => {
        timer = 0;
        texture.needsUpdate = true;
      }, FLUSH_MS);
    },
    clear() {
      context.clearRect(0, 0, ATLAS_SIZE, ATLAS_SIZE);
      texture.needsUpdate = true;
    },
  };
}

// --- the swarm -------------------------------------------------------------

export function createBugSwarm(THREE, { bounds, ring, height = 8, interactive }) {
  const layer = new THREE.Group();
  layer.name = "territory-bugs";
  const bodyMaterial = resources.material(THREE, {
    color: "#ffffff", vertexColors: true, roughness: 0.45, metalness: 0.15,
  }, "MeshStandardMaterial");
  const wingMaterial = resources.material(THREE, {
    color: "#ffffff", vertexColors: true, roughness: 0.8, metalness: 0,
    side: THREE.DoubleSide,
  }, "MeshStandardMaterial");
  const atlas = makeAtlas(THREE);
  const states = [];
  const billboard = new THREE.Quaternion();
  let generation = 0;

  function clear() {
    generation += 1;
    states.splice(0).forEach((state) => {
      state.parts.forEach((piece) => {
        const index = interactive.indexOf(piece);
        if (index >= 0) interactive.splice(index, 1);
      });
    });
    layer.clear();
    atlas.clear();
  }

  function badgeFor(bug, traits, cell, current) {
    atlas.draw(cell, { domain: bug.domain, hue: traits.hue, ring });
    if (!bug.favicon) return;
    const image = new Image();
    image.decoding = "async";
    image.onload = () => {
      if (current !== generation) return;
      if (faviconUsable(image)) atlas.draw(cell, { image, domain: bug.domain, hue: traits.hue, ring });
    };
    image.src = bug.favicon;
  }

  function add(bug, index, current) {
    const traits = bugTraits(bug.domain);
    const seed = deterministicFraction(`${bug.domain}:${index}`);
    const group = new THREE.Group();
    group.name = `territory-bug:${traits.kind}`;
    const model = new THREE.Group();
    model.scale.setScalar(traits.scale);
    group.add(model);
    const body = new THREE.Mesh(bodyGeometry(THREE, traits.kind, traits.color), bodyMaterial);
    body.castShadow = true;
    model.add(body);
    const parts = [body];
    let legs = null;
    let wings = null;
    if (traits.motion === "fly") {
      wings = [-1, 1].map((side) => {
        const wing = new THREE.Mesh(wingGeometry(THREE, traits.color, side), wingMaterial);
        wing.position.set(side * 0.1, 0.05, 0.1);
        model.add(wing);
        parts.push(wing);
        return wing;
      });
    } else {
      legs = new THREE.Mesh(legGeometry(THREE, traits.kind), bodyMaterial);
      model.add(legs);
    }
    const badge = new THREE.Mesh(badgeGeometry(THREE, index), atlas.material);
    badge.name = "territory-bug-badge";
    badge.scale.setScalar(BADGE_SIZE);
    badge.position.y = bodyTop(traits.kind) * traits.scale + BADGE_GAP + BADGE_SIZE / 2;
    badge.renderOrder = 6;
    badge.frustumCulled = false;
    // Face the camera whatever way the bug is heading.
    badge.onBeforeRender = (_renderer, _scene, camera) => {
      group.getWorldQuaternion(billboard).invert().multiply(camera.quaternion);
      badge.quaternion.copy(billboard);
      badge.updateMatrixWorld();
    };
    group.add(badge);
    parts.push(badge);
    parts.forEach((piece) => {
      piece.userData.interactive = "territory-bug";
      piece.userData.bug = bug;
      interactive.push(piece);
    });
    const state = {
      group,
      model,
      legs,
      wings,
      parts,
      traits,
      x: (seed - 0.5) * (bounds.x * 2),
      z: (deterministicFraction(`${bug.domain}:z`) - 0.5) * (bounds.z * 2),
      y: traits.altitude,
      heading: seed * Math.PI * 2,
      turn: 0,
      phase: seed * 10,
      pause: 0,
    };
    group.position.set(state.x, state.y, state.z);
    group.rotation.y = state.heading;
    layer.add(group);
    states.push(state);
    badgeFor(bug, traits, index, current);
  }

  function update(list = []) {
    clear();
    const current = generation;
    (Array.isArray(list) ? list : []).slice(0, BUG_BADGE_SLOTS)
      .forEach((bug, index) => add(bug, index, current));
    return states.length;
  }

  function step(delta) {
    for (let index = 0; index < states.length; index += 1) {
      const state = states[index];
      const { traits } = state;
      state.phase += delta;
      if (Math.sin(state.phase * 1.7 + index) > 0.96) {
        state.turn = Math.sin(state.phase * 13.1 + index * 7.3) * traits.turn;
      }
      let speed = traits.speed;
      if (traits.motion === "dash") {
        // Stop, look about, then scurry off: a sawtooth of rest and run.
        const cycle = (state.phase * 0.45 + index * 0.37) % 1;
        speed = cycle < 0.45 ? 0 : traits.speed * Math.min(1, (cycle - 0.45) * 6);
      }
      state.heading += state.turn * delta * (speed > 0 ? 1 : 0.3);
      state.x += Math.sin(state.heading) * speed * delta;
      state.z += Math.cos(state.heading) * speed * delta;
      const limitX = bounds.x;
      const limitZ = bounds.z;
      if (Math.abs(state.x) > limitX || Math.abs(state.z) > limitZ) {
        state.x = Math.max(-limitX, Math.min(limitX, state.x));
        state.z = Math.max(-limitZ, Math.min(limitZ, state.z));
        state.heading = Math.atan2(-state.x, -state.z) + Math.sin(state.phase) * 0.6;
      }
      state.group.position.x = state.x;
      state.group.position.z = state.z;
      state.group.rotation.y = state.heading;
      if (state.wings) {
        const flap = Math.sin(state.phase * 22) * 0.75;
        state.wings[0].rotation.z = flap;
        state.wings[1].rotation.z = -flap;
        const ceiling = Math.max(1.2, height - 1.5);
        state.group.position.y = Math.min(ceiling, state.y + Math.sin(state.phase * 1.3) * 0.6);
        state.model.rotation.x = Math.sin(state.phase * 2.1) * 0.08;
      } else if (state.legs) {
        state.legs.rotation.y = speed > 0 ? Math.sin(state.phase * traits.gait) * 0.22 : 0;
        state.model.position.y = speed > 0 ? Math.abs(Math.sin(state.phase * traits.gait)) * 0.03 : 0;
      }
    }
  }

  return { group: layer, update, step, count: () => states.length };
}
