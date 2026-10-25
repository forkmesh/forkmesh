import * as resources from "./resources.js";

export const GENIE_SPIDER_KEY = "forkmesh.world.genie";
export const GENIE_SPIDER_OFF_KEY = "forkmesh.genie.off";
export const GENIE_SPIDER_OFF_COOKIE = "forkmesh_genie";
export const GENIE_SPIDER_COLORS = Object.freeze({
  shell: "#2a2a55",
  shellLight: "#4a4a80",
  hat: "#2e1a4a",
  violet: "#663da8",
  lilac: "#8f66d1",
  glow: "#40f2db",
  silver: "#ccd1e0",
  pitch: "#080814",
  gold: "#ffce54",
});
export const GENIE_SPIDER_BODY_Y = 0.62;
export const GENIE_SPIDER_SIZE = 1.6;
export const GENIE_SPIDER_LEGS = 8;
export const GENIE_TARGET_KINDS = Object.freeze([
  "ground", "player", "avatar", "bot", "fountain", "campfire", "bike", "car",
  "drone", "swing", "portal", "landmark", "board", "thing",
]);
export const GENIE_TARGET_SIZES = Object.freeze({
  ground: { radius: 0, height: 0 },
  player: { radius: 1.1, height: 4.9 },
  avatar: { radius: 1.1, height: 4.9 },
  bot: { radius: 1.4, height: 3 },
  fountain: { radius: 4.6, height: 1.4 },
  campfire: { radius: 3, height: 0.8 },
  bike: { radius: 1.2, height: 1.6 },
  car: { radius: 2.4, height: 2 },
  drone: { radius: 1.4, height: 1.1 },
  swing: { radius: 2.2, height: 3.8 },
  portal: { radius: 2.4, height: 4 },
  landmark: { radius: 6.5, height: 9 },
  board: { radius: 2, height: 3.4 },
  thing: { radius: 1.2, height: 1.2 },
});

const C = GENIE_SPIDER_COLORS;
const TAU = Math.PI * 2;
const clamp01 = (value) => Math.max(0, Math.min(1, value));
const ease = (value) => {
  const t = clamp01(value);
  return t * t * (3 - 2 * t);
};
const arc = (value) => Math.sin(Math.PI * clamp01(value));
const lerp = (a, b, t) => a + (b - a) * t;
const phase = (t, from, to) => clamp01((t - from) / (to - from));

const EYES = Object.freeze([
  [-0.12, 0.12, 0.49, 0.075],
  [0.12, 0.12, 0.49, 0.075],
  [-0.25, 0.17, 0.4, 0.055],
  [0.25, 0.17, 0.4, 0.055],
  [-0.08, 0.25, 0.47, 0.04],
  [0.08, 0.25, 0.47, 0.04],
]);
const LIT_PARTS = Object.freeze([
  [0, 0.12, -0.5, 0.55, 0.46, 0.66, C.shell],
  [0, 0.34, -0.55, 0.3, 0.2, 0.46, C.shellLight],
  [0, 0.05, 0.18, 0.38, 0.33, 0.36, C.shellLight],
  [0, 0.36, 0.16, 0.37, 0.13, 0.37, C.violet],
  [0, 0.5, 0.14, 0.3, 0.26, 0.3, C.hat],
  [0, 0.74, 0.1, 0.08, 0.1, 0.08, C.lilac],
  [0, 0.4, 0.5, 0.1, 0.1, 0.04, C.gold],
]);
const GLOW_PARTS = Object.freeze([[0, 0.4, 0.53, 0.065, 0.065, 0.04, C.glow]]);
const HIPS_Z = Object.freeze([0.32, 0.12, -0.12, -0.38]);
const FEET_Z = Object.freeze([0.95, 0.42, -0.25, -0.85]);

export function genieSpiderAllowed({ cookie = "", offFlag = null, worldFlag = null } = {}) {
  if (worldFlag === "off") return false;
  if (worldFlag === "on") return true;
  const entry = String(cookie || "").split(";").map((part) => part.trim())
    .find((part) => part.startsWith(`${GENIE_SPIDER_OFF_COOKIE}=`));
  const value = entry ? entry.slice(GENIE_SPIDER_OFF_COOKIE.length + 1) : "";
  if (value === "off") return false;
  if (value === "on") return true;
  return offFlag !== "1";
}

function placeSphere(THREE, mesh, index, [x, y, z, sx, sy, sz, color], matrix, color3) {
  matrix.compose(new THREE.Vector3(x, y, z), new THREE.Quaternion(), new THREE.Vector3(sx, sy, sz));
  mesh.setMatrixAt(index, matrix);
  mesh.setColorAt(index, color3.set(color));
}

export function createGenieSpiderModel(THREE) {
  const root = new THREE.Group();
  root.name = "forkmesh-genie-spider";
  const body = new THREE.Group();
  body.name = "forkmesh-genie-spider-body";
  body.position.y = GENIE_SPIDER_BODY_Y;
  root.add(body);
  const sphere = resources.primitive(THREE, "SphereGeometry", 1, 18, 12);
  const matrix = new THREE.Matrix4();
  const color = new THREE.Color();
  const litCount = LIT_PARTS.length + EYES.length;
  const lit = new THREE.InstancedMesh(sphere, resources.uniqueMaterial(THREE, { color: "#ffffff", roughness: 0.45, metalness: 0.2, emissive: "#1c1240", emissiveIntensity: 0.7 }, "MeshStandardMaterial"), litCount);
  lit.name = "forkmesh-genie-spider-shell";
  LIT_PARTS.forEach((part, index) => placeSphere(THREE, lit, index, part, matrix, color));
  const glowCount = GLOW_PARTS.length + EYES.length;
  const glow = new THREE.InstancedMesh(sphere, resources.uniqueMaterial(THREE, { color: "#ffffff", toneMapped: false }, "MeshBasicMaterial"), glowCount);
  glow.name = "forkmesh-genie-spider-glow";
  GLOW_PARTS.forEach((part, index) => placeSphere(THREE, glow, index, part, matrix, color));
  const eyeSlots = EYES.map(([x, y, z, r], index) => {
    const eye = GLOW_PARTS.length + index;
    const pupil = LIT_PARTS.length + index;
    placeSphere(THREE, glow, eye, [x, y, z, r, r, r * 0.7, C.glow], matrix, color);
    placeSphere(THREE, lit, pupil, [x * 1.02, y + r * 0.1, z + r * 0.55, r * 0.5, r * 0.5, r * 0.3, C.pitch], matrix, color);
    return { eye, pupil, x, y, z, r };
  });
  for (const mesh of [lit, glow]) {
    mesh.frustumCulled = false;
    mesh.castShadow = true;
    mesh.instanceMatrix.needsUpdate = true;
    if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
    body.add(mesh);
  }
  const legs = new THREE.InstancedMesh(
    resources.primitive(THREE, "CylinderGeometry", 0.04, 0.05, 1, 6, 1),
    resources.uniqueMaterial(THREE, { color: C.shellLight, roughness: 0.5, metalness: 0.2 }, "MeshStandardMaterial"),
    GENIE_SPIDER_LEGS * 2,
  );
  legs.name = "forkmesh-genie-spider-legs";
  legs.frustumCulled = false;
  legs.castShadow = true;
  root.add(legs);
  root.userData.genieSpider = { body, lit, glow, legs, eyeSlots, eyeScale: 1 };
  return root;
}

export function setGenieSpiderEyes(THREE, model, openness) {
  const data = model?.userData?.genieSpider;
  if (!data) return;
  const value = Math.max(0.08, Math.min(1.5, Number(openness) || 0));
  if (Math.abs(value - data.eyeScale) < 0.01) return;
  data.eyeScale = value;
  const matrix = new THREE.Matrix4();
  const position = new THREE.Vector3();
  const rotation = new THREE.Quaternion();
  const scale = new THREE.Vector3();
  for (const slot of data.eyeSlots) {
    const width = value > 1 ? value : 1;
    position.set(slot.x, slot.y, slot.z);
    scale.set(slot.r * width, slot.r * value, slot.r * 0.7);
    data.glow.setMatrixAt(slot.eye, matrix.compose(position, rotation, scale));
    position.set(slot.x * 1.02, slot.y + slot.r * 0.1, slot.z + slot.r * 0.55);
    scale.set(slot.r * 0.5 * width, slot.r * 0.5 * Math.min(1, value), slot.r * 0.3);
    data.lit.setMatrixAt(slot.pupil, matrix.compose(position, rotation, scale));
  }
  data.glow.instanceMatrix.needsUpdate = true;
  data.lit.instanceMatrix.needsUpdate = true;
}

export const GENIE_EYES = Object.freeze({ open: 1, closed: 0.1, wide: 1.35, happy: 0.45 });

export function genieLegTargets(mode, { phase: gait = 0, time = 0, stride = 0.34 } = {}) {
  const feet = [];
  for (let leg = 0; leg < GENIE_SPIDER_LEGS; leg += 1) {
    const side = leg < 4 ? -1 : 1;
    const row = leg % 4;
    const group = (row + (side > 0 ? 1 : 0)) % 2;
    let x = side * 1.05;
    let y = 0;
    let z = FEET_Z[row];
    if (mode === "walk") {
      const swing = gait + group * Math.PI;
      y = Math.max(0, Math.sin(swing)) * 0.3;
      z += Math.cos(swing) * stride;
    } else if (mode === "dance") {
      const swing = time * 9 + group * Math.PI;
      y = Math.max(0, Math.sin(swing)) * 0.45;
      x += side * Math.max(0, Math.sin(swing)) * 0.15;
    } else if (mode === "tuck") {
      x = side * 0.5;
      y = 0.2;
      z = FEET_Z[row] * 0.55;
    } else if (mode === "spread") {
      x = side * 1.35;
      y = 0.35;
      z = FEET_Z[row] * 1.3;
    } else if (mode === "wave" && leg === 0) {
      x = -0.55;
      y = 1.45 + Math.sin(time * 14) * 0.22;
      z = 0.85 + Math.cos(time * 14) * 0.1;
    } else if (mode === "drum" && row === 0) {
      y = Math.max(0, Math.sin(time * 18 + (side > 0 ? Math.PI : 0))) * 0.4;
      z = 1.05;
      x = side * 0.6;
    } else if (mode === "punch" && row === 0) {
      const jab = Math.max(0, Math.sin(time * 10 + (side > 0 ? Math.PI : 0)));
      y = 0.65;
      z = 0.9 + jab * 0.7;
      x = side * 0.45;
    }
    feet.push([x, y, z]);
  }
  return feet;
}

export function genieLegHips() {
  const hips = [];
  for (let leg = 0; leg < GENIE_SPIDER_LEGS; leg += 1) {
    const side = leg < 4 ? -1 : 1;
    hips.push([side * 0.3, 0.05, HIPS_Z[leg % 4]]);
  }
  return hips;
}

export function genieKnee(hip, foot, side) {
  return [
    lerp(hip[0], foot[0], 0.42) + side * 0.18,
    Math.max(hip[1], foot[1]) + 0.42,
    lerp(hip[2], foot[2], 0.42),
  ];
}

const pose = (fields = {}) => ({
  base: "anchor", x: 0, y: 0, z: 0, yaw: 0, pitch: 0, roll: 0, scale: 1,
  stretch: 0, crouch: 0, legs: "stand", eyes: "open", web: "", stride: 1, ...fields,
});

function topMotion(name, e, t) {
  if (name === "spin") return { yaw: e * 5, legs: "spread" };
  if (name === "dance") return { x: Math.sin(e * 5) * 0.25, yaw: Math.sin(e * 3) * 0.6, legs: "dance", eyes: "happy" };
  if (name === "wave") return { legs: "wave", eyes: "happy" };
  if (name === "hop") return { y: Math.abs(Math.sin(e * 7)) * 0.6 };
  if (name === "flip") return { y: arc(t) * 1.2, pitch: -ease(t) * TAU, legs: "tuck" };
  if (name === "sway") return { yaw: Math.sin(e * 2) * 0.5, roll: Math.sin(e * 2) * 0.15, eyes: "happy" };
  return { crouch: 0.4, legs: "tuck", eyes: "happy" };
}

export const GENIE_MOTIONS = Object.freeze({
  hop: ({ t, p }) => pose({ y: Math.abs(Math.sin(t * Math.PI * (p.hops || 3))) * (p.height || 0.9) }),
  bigHop: ({ t }) => pose({ y: arc(t) * 3.2, yaw: ease(t) * TAU, legs: "spread" }),
  spin: ({ t, p }) => pose({ yaw: ease(t) * TAU * (p.turns || 3), y: arc(t) * 0.3, legs: "spread" }),
  orbit: ({ t, e, m, p }) => {
    const a = t * TAU * (p.turns || 1) * (p.reverse ? -1 : 1);
    const r = p.radius || m.radius + 1.4;
    const dir = p.reverse ? -1 : 1;
    return pose({ base: "center", x: Math.sin(a) * r, z: -Math.cos(a) * r, yaw: Math.atan2(Math.cos(a) * dir, Math.sin(a) * dir), legs: "walk", stride: p.fast ? 2.4 : 1.4, web: p.web ? "target" : "", y: p.hover ? 0.4 + Math.sin(e * 4) * 0.2 : 0 });
  },
  climb: ({ t, e, m, p }) => {
    const h = Math.min(14, (p.height || m.height) + (p.lift || 0));
    if (t < 0.25) {
      const u = phase(t, 0, 0.25);
      return pose({ base: "center", z: lerp(-m.reach, 0, ease(u)), y: h * ease(u) + arc(u) * 1.2, legs: "tuck" });
    }
    if (t > 0.78) {
      const u = phase(t, 0.78, 1);
      return pose({ base: "center", z: lerp(0, -m.reach, ease(u)), y: h * (1 - ease(u)) + arc(u) * 1.2, legs: "tuck" });
    }
    const top = topMotion(p.top, e, phase(t, 0.25, 0.78));
    return pose({ base: "center", ...top, y: h + (top.y || 0) });
  },
  peek: ({ t, e, m }) => {
    const r = m.radius + 1;
    let a = 0;
    let y = 0;
    let eyes = "open";
    if (t < 0.4) a = ease(phase(t, 0, 0.4)) * Math.PI;
    else if (t < 0.6) a = Math.PI;
    else if (t < 0.7) {
      a = lerp(Math.PI, Math.PI / 2, ease(phase(t, 0.6, 0.7)));
      y = arc(phase(t, 0.6, 0.7)) * 0.9;
      eyes = "wide";
    } else a = lerp(Math.PI / 2, 0, ease(phase(t, 0.7, 1)));
    return pose({ base: "center", x: Math.sin(a) * r, z: -Math.cos(a) * r, y: y + Math.sin(e * 6) * 0.03, yaw: t > 0.6 && t < 0.75 ? -Math.PI / 2 : a + Math.PI / 2, legs: t < 0.4 || t > 0.7 ? "walk" : "stand", eyes });
  },
  wiggle: ({ t, e }) => pose({ roll: Math.sin(e * 22) * 0.35 * arc(t), x: Math.sin(e * 11) * 0.1, eyes: "happy" }),
  dance: ({ e }) => pose({ x: Math.sin(e * 5) * 0.5, y: Math.abs(Math.sin(e * 10)) * 0.25, yaw: Math.sin(e * 2.5) * 0.8, legs: "dance", eyes: "happy" }),
  robot: ({ e }) => {
    const step = Math.floor(e * 4);
    return pose({ yaw: ((step % 4) - 1.5) * 0.4, y: (step % 2) * 0.15, legs: "drum" });
  },
  flip: ({ t }) => pose({ y: arc(t) * 2.4, pitch: -ease(t) * TAU, legs: "tuck" }),
  tiptoe: ({ t, m }) => pose({ base: "center", z: lerp(-m.reach, -(m.radius + 0.6), ease(t)), y: 0.14, stretch: 0.12, legs: "walk", stride: 0.5, eyes: "wide" }),
  zoomies: ({ t }) => {
    const a = t * TAU * 3;
    const r = 3;
    return pose({ x: Math.sin(a) * r, z: r - Math.cos(a) * r, yaw: Math.atan2(Math.cos(a), Math.sin(a)), legs: "walk", stride: 3 });
  },
  bounce: ({ t, e }) => pose({ y: Math.abs(Math.sin(e * 9)) * (1 - t) * 1.4, stretch: Math.sin(e * 18) * 0.12 }),
  dangle: ({ t, e }) => {
    const up = ease(phase(t, 0, 0.3)) * (1 - ease(phase(t, 0.8, 1)));
    return pose({ y: up * 4.6 + Math.sin(e * 1.5) * 0.2 * up, x: Math.sin(e * 3) * 1.1 * up, roll: up * Math.PI, legs: "spread", web: "up", eyes: "happy" });
  },
  nap: ({ e }) => pose({ crouch: 0.75, legs: "tuck", eyes: "closed", pitch: 0.05, stretch: Math.sin(e * 2) * 0.04 }),
  stretch: ({ t }) => pose({ stretch: arc(t) * 0.5, legs: "spread", eyes: "closed" }),
  shiver: ({ e }) => pose({ x: Math.sin(e * 60) * 0.06, roll: Math.sin(e * 47) * 0.06, crouch: 0.2, eyes: "wide" }),
  moonwalk: ({ t, e }) => pose({ z: -ease(t) * 3.5, y: Math.abs(Math.sin(e * 6)) * 0.06, legs: "walk", stride: -1.2, eyes: "happy" }),
  wave: ({ e }) => pose({ y: Math.abs(Math.sin(e * 3)) * 0.08, legs: "wave", eyes: "happy" }),
  cartwheel: ({ t }) => pose({ roll: ease(t) * TAU * 2, x: ease(t) * 3, y: 0.5 * arc(t), legs: "spread" }),
  levitate: ({ t, e }) => pose({ y: arc(t) * 2.2, yaw: e * 1.5, legs: "spread", eyes: "happy" }),
  juggle: ({ e }) => pose({ y: Math.abs(Math.sin(e * 4)) * 0.12, legs: "drum", eyes: "happy" }),
  sit: ({ e }) => pose({ crouch: 0.4, yaw: Math.sin(e * 1.2) * 0.15, eyes: "happy" }),
  inspect: ({ e, m }) => pose({ base: "center", z: -(m.radius + 0.9), pitch: 0.35 + Math.sin(e * 3) * 0.08, yaw: Math.sin(e * 1.2) * 0.3, eyes: "wide" }),
  stargaze: ({ e }) => pose({ pitch: -0.5, crouch: 0.3, yaw: Math.sin(e * 0.8) * 0.4, eyes: "wide" }),
  knock: ({ e, m }) => pose({ base: "center", z: -(m.radius + 0.7), pitch: Math.sin(e * 14) > 0 ? 0.22 : 0, legs: "drum" }),
  sniff: ({ e, m }) => pose({ base: "center", z: -(m.radius + 0.8) + Math.sin(e * 6) * 0.25, pitch: 0.25, yaw: Math.sin(e * 2) * 0.4 }),
  pulse: ({ t, e }) => pose({ scale: 1 + Math.sin(e * 10) * 0.12 * arc(t), eyes: "happy" }),
  spiral: ({ t }) => {
    const a = t * TAU * 3;
    const r = 1.5 * (1 - t);
    return pose({ x: Math.sin(a) * r, z: r - Math.cos(a) * r, y: arc(t) * 3.5, yaw: a, legs: "spread" });
  },
  zigzag: ({ t }) => pose({ z: ease(t) * 4, x: Math.sin(t * Math.PI * 6) * 1.2, yaw: Math.cos(t * Math.PI * 6) * 0.9, legs: "walk", stride: 2 }),
  pounce: ({ t, e, m }) => {
    const far = -m.reach;
    const near = -(m.radius + 0.6);
    if (t < 0.45) return pose({ base: "center", z: far, crouch: 0.5, roll: Math.sin(e * 16) * 0.06, eyes: "wide" });
    if (t < 0.75) {
      const u = phase(t, 0.45, 0.75);
      return pose({ base: "center", z: lerp(far, near, ease(u)), y: arc(u) * 1.8, legs: "spread", eyes: "wide" });
    }
    return pose({ base: "center", z: near, crouch: 0.4 * (1 - phase(t, 0.75, 1)), eyes: "happy" });
  },
  bow: ({ t }) => pose({ pitch: arc(t) * 0.6, crouch: arc(t) * 0.3 }),
  march: ({ e }) => pose({ yaw: Math.sin(e * 1.5) * 0.6, y: Math.abs(Math.sin(e * 9)) * 0.1, legs: "dance" }),
  drumroll: ({ e }) => pose({ pitch: 0.15, y: Math.abs(Math.sin(e * 20)) * 0.04, legs: "drum" }),
  shrink: ({ t }) => {
    let scale = 1;
    if (t < 0.35) scale = lerp(1, 0.15, ease(phase(t, 0, 0.35)));
    else if (t < 0.6) scale = 0.15;
    else if (t < 0.75) scale = lerp(0.15, 1.35, ease(phase(t, 0.6, 0.75)));
    else scale = lerp(1.35, 1, ease(phase(t, 0.75, 1)));
    return pose({ scale, eyes: t > 0.6 && t < 0.8 ? "wide" : "open" });
  },
  photo: ({ t }) => pose({ z: -arc(t) * 1.2, y: arc(t * 2) * 0.4, eyes: t > 0.4 && t < 0.5 ? "closed" : "happy" }),
  sway: ({ e }) => pose({ yaw: Math.sin(e * 1.6) * 0.5, roll: Math.sin(e * 1.6) * 0.15, eyes: "happy" }),
  conga: ({ e }) => pose({ base: "follow", x: Math.sin(e * 3) * 0.6, y: Math.abs(Math.sin(e * 6)) * 0.2, legs: "walk", stride: 1.6, eyes: "happy" }),
  punch: ({ e }) => pose({ x: Math.sin(e * 4) * 0.3, legs: "punch", eyes: "wide" }),
  starfish: ({ e }) => pose({ crouch: 0.8, legs: "spread", roll: Math.sin(e * 4) * 0.1, eyes: "happy" }),
  poof: ({ t }) => pose({ scale: t < 0.2 ? ease(t / 0.2) : 1, y: arc(t) * 0.4 }),
});

const act = (id, title, on, motion, fx, emoji, say, ms = 4200, p = {}) => Object.freeze({ id, title, on: Object.freeze(on), motion, fx, emoji, say, ms, p: Object.freeze(p) });
const G = ["ground"];

export const GENIE_SPIDER_ACTS = Object.freeze([
  act("happy-hops", "Hops for joy", G, "hop", "twinkle", "✨", "Hop hop hop!", 2600, { hops: 4 }),
  act("tornado-spin", "Spins like a tornado", G, "spin", "spiral", "🌪️", "Wheee!", 2600, { turns: 4 }),
  act("backflip", "Does a backflip", G, "flip", "burst", "⭐", "Ta-da!", 1800),
  act("moonwalk", "Moonwalks", G, "moonwalk", "trail", "🎵", "Smooth.", 3400),
  act("web-yo-yo", "Yo-yos on a web", G, "dangle", "none", "🕸️", "Just hanging out.", 5200),
  act("cat-nap", "Takes a cat nap", G, "nap", "rise", "💤", "Five more minutes…", 6000),
  act("big-stretch", "Has a big stretch", G, "stretch", "none", "🙆", "Mmm, stretchy.", 2400),
  act("cartwheels", "Turns cartwheels", G, "cartwheel", "trail", "🌈", "Cartwheel!", 2600),
  act("genie-float", "Floats on genie magic", G, "levitate", "orbit", "✨", "Genie magic.", 4200),
  act("moon-juggling", "Juggles three moons", G, "juggle", "orbit", "🌙", "Moon juggling.", 4200),
  act("zoomies", "Gets the zoomies", G, "zoomies", "trail", "💨", "ZOOMIES!", 3600),
  act("disco", "Throws a disco", G, "dance", "rain", "🪩", "Disco time!", 4800),
  act("rain-dance", "Does a rain dance", G, "dance", "rain", "🌧️", "Rain dance!", 4200),
  act("snow-angel", "Makes a snow angel", G, "starfish", "fountain", "❄️", "Snow angel!", 4200),
  act("shadowbox", "Shadowboxes", G, "punch", "burst", "🥊", "Float like a butterfly…", 3200),
  act("drum-solo", "Plays a drum solo", G, "drumroll", "rise", "🥁", "Drum solo!", 3600),
  act("now-you-see-me", "Shrinks away and pops back", G, "shrink", "burst", "🎉", "Now you see me…", 3000),
  act("sky-spiral", "Spirals into the sky", G, "spiral", "spiral", "💫", "To the sky!", 3400),
  act("zigzag-dash", "Dashes in zigzags", G, "zigzag", "trail", "⚡", "Zig! Zag!", 2800),
  act("bubble-blowing", "Blows bubbles", G, "sit", "rise", "🫧", "Blowing bubbles.", 4600),
  act("stargazing", "Gazes at the stars", G, "stargaze", "twinkle", "⭐", "So many stars.", 4600),
  act("shooting-star", "Wishes on a shooting star", G, "levitate", "fountain", "🌠", "Make a wish!", 3600),
  act("confetti-cannon", "Fires a confetti cannon", G, "bounce", "fountain", "🎊", "Party!", 3000),
  act("heart-dance", "Dances with hearts", G, "dance", "rise", "💜", "Feeling the love.", 3800),
  act("marching-band", "Leads a marching band", G, "march", "rise", "🎺", "Left, right, left!", 4200),
  act("hiccups", "Gets the hiccups", G, "bounce", "burst", "🫢", "*hic*", 2600),
  act("brrr", "Shivers in the cold", G, "shiver", "rain", "❄️", "Brrr!", 3000),
  act("sunbathing", "Sunbathes", G, "nap", "twinkle", "😎", "Soaking up the sun.", 5000),
  act("pirouette", "Does a pirouette", G, "spin", "spiral", "🩰", "Pirouette!", 2400, { turns: 2 }),
  act("make-it-rain", "Makes it rain coins", G, "hop", "rain", "🪙", "Make it rain!", 3200, { hops: 2 }),
  act("high-five", "High-fives you", ["player"], "wave", "burst", "🙌", "High five!", 2600),
  act("group-hug", "Gives you a hug", ["player"], "pulse", "rise", "🤗", "Group hug!", 3000),
  act("orbit-you", "Orbits you like a moon", ["player"], "orbit", "trail", "✨", "You're my sun.", 4200, { turns: 2 }),
  act("head-throne", "Sits on your head", ["player"], "climb", "twinkle", "👑", "Best seat in town.", 6000, { top: "sit" }),
  act("dance-off", "Challenges you to a dance-off", ["player"], "dance", "rain", "🕺", "Dance-off!", 4800),
  act("conga-line", "Starts a conga line behind you", ["player"], "conga", "trail", "🎶", "Conga line!", 7000),
  act("tickle-attack", "Tickles you", ["player"], "wiggle", "burst", "🤭", "Tickle tickle!", 2600),
  act("royal-bow", "Bows to you", ["player"], "bow", "twinkle", "🙇", "Your Majesty.", 2600),
  act("flowers-for-you", "Brings you flowers", ["player"], "sit", "fountain", "💐", "For you!", 3200),
  act("cheerleader", "Cheers you on", ["player"], "bounce", "fountain", "📣", "Go you!", 3000),
  act("photo-op", "Takes your photo", ["player"], "photo", "burst", "📸", "Say cheese!", 2400),
  act("peekaboo", "Plays peekaboo around you", ["player"], "peek", "burst", "👀", "Peekaboo!", 4200),
  act("magic-trick", "Does a magic trick for you", ["player"], "levitate", "orbit", "🎩", "Abracadabra!", 3600),
  act("heart-shower", "Showers you with hearts", ["player"], "hop", "rain", "💖", "Love you!", 3200, { hops: 2 }),
  act("surprise-gift", "Gives you a present", ["player"], "pulse", "fountain", "🎁", "A gift!", 3000),
  act("serenade", "Serenades you", ["player"], "sway", "rise", "🎵", "La la la~", 4600),
  act("pounce-play", "Pounces on your feet", ["player"], "pounce", "burst", "🐾", "Gotcha!", 2600),
  act("shoulder-ride", "Rides on your shoulder", ["player"], "climb", "twinkle", "🦜", "Shoulder ride!", 6000, { top: "sway", height: 3.4 }),
  act("bodyguard", "Guards you", ["player"], "orbit", "none", "🕶️", "Nobody messes with you.", 5200, { turns: 1 }),
  act("web-hammock", "Hangs a hammock next to you", ["player"], "dangle", "rise", "🕸️", "Hammock time.", 5200),
  act("fortune-teller", "Tells your fortune", ["player"], "inspect", "orbit", "🔮", "I see… good commits ahead.", 4200),
  act("cookie-break", "Shares cookies", ["player"], "hop", "fountain", "🍪", "Cookie break!", 3000, { hops: 2 }),
  act("wave-hello", "Waves hello to someone", ["avatar"], "wave", "burst", "👋", "Hi there!", 2800),
  act("orbit-friend", "Circles a new friend", ["avatar"], "orbit", "trail", "💫", "New friend!", 4200, { turns: 1.5 }),
  act("hat-of-honour", "Sits on someone's head", ["avatar"], "climb", "twinkle", "🎩", "Comfy hat, right?", 6000, { top: "sit" }),
  act("introductions", "Introduces itself", ["avatar"], "bow", "twinkle", "🤝", "Have you met my human?", 2800),
  act("friend-dance", "Dances with someone", ["avatar"], "dance", "rise", "💃", "Join in!", 4200),
  act("sniff-hello", "Sniffs hello", ["avatar"], "sniff", "rise", "👃", "You smell like code.", 3200),
  act("boo", "Says boo to someone", ["avatar"], "peek", "burst", "🙈", "Boo!", 4200),
  act("aww", "Melts at someone", ["avatar"], "pulse", "rise", "💕", "Aww.", 3000),
  act("tag", "Plays tag", ["avatar"], "pounce", "burst", "🏃", "Tag, you're it!", 2600),
  act("up-top", "High-fives someone", ["avatar"], "wave", "fountain", "🙌", "Up top!", 2600),
  act("robot-dance", "Does the robot with Forkbot", ["bot"], "robot", "twinkle", "🤖", "Beep boop!", 4200),
  act("bot-gossip", "Gossips with Forkbot", ["bot"], "inspect", "rise", "💬", "What's new, Forkbot?", 4200),
  act("king-of-the-bots", "Climbs on top of Forkbot", ["bot"], "climb", "spiral", "⚙️", "King of the bots.", 6000, { top: "spin" }),
  act("bot-race", "Races around Forkbot", ["bot"], "orbit", "trail", "🏁", "Race you!", 3600, { turns: 3, fast: true }),
  act("splash", "Splashes in the fountain", ["fountain"], "hop", "fountain", "💦", "Splash!", 3000, { hops: 3 }),
  act("fountain-wish", "Throws a coin in the fountain", ["fountain"], "inspect", "fountain", "🪙", "Wishing for more stars.", 3600),
  act("fountain-laps", "Swims laps round the fountain", ["fountain"], "orbit", "trail", "🏊", "Laps!", 5200, { turns: 1 }),
  act("rim-balance", "Balances on the fountain rim", ["fountain"], "climb", "rise", "🌊", "Balance!", 5600, { top: "dance" }),
  act("rubber-duck", "Floats a rubber duck", ["fountain"], "sit", "rise", "🦆", "Quack.", 4200),
  act("marshmallows", "Toasts marshmallows", ["campfire"], "inspect", "rise", "🍡", "Toasty.", 4600),
  act("campfire-story", "Tells a campfire story", ["campfire"], "sway", "rise", "🔥", "Once upon a time…", 5200),
  act("fire-dance", "Dances by the fire", ["campfire"], "dance", "fountain", "🔥", "Hot moves!", 4200),
  act("ghost-story", "Tells a ghost story", ["campfire"], "shiver", "rise", "👻", "Spooooky.", 4200),
  act("sing-along", "Leads a sing-along", ["campfire"], "sway", "rise", "🎸", "Kumbaya~", 4600),
  act("bike-ride", "Rides a bike", ["bike"], "climb", "trail", "🚲", "Ding ding!", 5200, { top: "hop" }),
  act("pump-tyres", "Pumps up the tyres", ["bike"], "drumroll", "burst", "💨", "Pressure's good.", 3200),
  act("wheelie", "Pulls a wheelie", ["bike"], "climb", "burst", "🤸", "Wheelie!", 5200, { top: "flip" }),
  act("roof-party", "Parties on a car roof", ["car"], "climb", "rain", "🚗", "Roof party!", 5600, { top: "dance" }),
  act("honk", "Honks the horn", ["car"], "knock", "burst", "📯", "Beep beep!", 2600),
  act("car-wash", "Washes the car", ["car"], "wiggle", "rain", "🧽", "Squeaky clean.", 3600),
  act("drone-chase", "Chases a drone", ["drone"], "orbit", "trail", "🛸", "Come back!", 3600, { turns: 2, fast: true }),
  act("drone-ride", "Rides a drone", ["drone"], "climb", "spiral", "🚁", "Lift off!", 5600, { top: "spin" }),
  act("swing-high", "Swings on the swings", ["swing"], "climb", "rise", "🎠", "Higher!", 5600, { top: "sway" }),
  act("swing-push", "Gives the swing a push", ["swing"], "knock", "burst", "🌀", "Wheee!", 2600),
  act("portal-peek", "Peeks into a portal", ["portal"], "inspect", "spiral", "🌀", "What's in there?", 4200),
  act("portal-hop", "Hops through a portal", ["portal"], "pounce", "burst", "✨", "Brb!", 2800),
  act("star-the-repo", "Stars a repository", ["portal"], "hop", "fountain", "⭐", "Starred!", 2800, { hops: 2 }),
  act("commit-reader", "Reads the commits", ["portal"], "inspect", "rise", "📜", "Nice commits.", 4200),
  act("summit", "Climbs a landmark", ["landmark"], "climb", "twinkle", "🏔️", "I can see my house!", 6200, { top: "wave" }),
  act("admire", "Admires a landmark", ["landmark"], "stargaze", "twinkle", "🤩", "Impressive.", 3600),
  act("tour-guide", "Gives a guided tour", ["landmark"], "zigzag", "trail", "🗺️", "On your left…", 3600),
  act("web-signature", "Signs a landmark with a web", ["landmark"], "orbit", "none", "🕸️", "Signed it.", 4200, { turns: 1, web: true }),
  act("read-the-board", "Reads a board", ["board"], "inspect", "rise", "📰", "Interesting…", 4200),
  act("sticky-note", "Sticks a note on a board", ["board"], "knock", "burst", "🗒️", "Note to self.", 2800),
  act("look-at-this", "Points at a board", ["board"], "wave", "twinkle", "👉", "Look at this!", 3000),
  act("investigate", "Investigates something", ["thing"], "sniff", "rise", "🔍", "Hmm, what's this?", 3600),
  act("web-wrap", "Wraps something in web", ["thing", "bike", "board"], "orbit", "none", "🕸️", "Wrapped!", 3600, { turns: 3, fast: true, web: true }),
  act("king-of-the-hill", "Claims something as its own", ["thing", "car", "bot"], "climb", "fountain", "🏆", "Mine now.", 5600, { top: "hop" }),
]);

export const GENIE_POOF_ACT = act("poof", "Pops in beside you", G, "poof", "burst", "💨", "", 1200);

export const GENIE_REACTION_ACTS = Object.freeze({
  "👍": "cheerleader",
  "👏": "confetti-cannon",
  "🔥": "fire-dance",
  "❤️": "heart-dance",
  "🫶": "heart-dance",
  "😂": "hiccups",
  "👋": "high-five",
  "✌️": "pirouette",
  "🤘": "disco",
  "👌": "royal-bow",
  "✋": "high-five",
  "👎": "brrr",
  "☝️": "stargazing",
});

export function genieActById(id) {
  return GENIE_SPIDER_ACTS.find((entry) => entry.id === id) || null;
}

export function genieActsFor(kinds) {
  const available = new Set(kinds);
  available.add("ground");
  return GENIE_SPIDER_ACTS.filter((entry) => entry.on.some((kind) => available.has(kind)));
}

export function drawGenieAct(bag, kinds, random = Math.random) {
  const fitting = genieActsFor(kinds);
  if (!fitting.length) return null;
  const fits = new Set(fitting.map((entry) => entry.id));
  let index = bag.findIndex((id) => fits.has(id));
  if (index < 0) {
    const shuffled = GENIE_SPIDER_ACTS.map((entry) => entry.id);
    for (let i = shuffled.length - 1; i > 0; i -= 1) {
      const j = Math.floor(random() * (i + 1));
      [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
    }
    bag.splice(0, bag.length, ...shuffled);
    index = bag.findIndex((id) => fits.has(id));
  }
  const [id] = bag.splice(index, 1);
  return genieActById(id);
}

export function genieActTargetKind(entry, kinds) {
  const available = new Set(kinds);
  const preferred = entry.on.filter((kind) => kind !== "ground" && available.has(kind));
  return preferred[0] || "ground";
}

export function genieMotionPose(entry, t, elapsed, measure) {
  const motion = GENIE_MOTIONS[entry?.motion] || GENIE_MOTIONS.sit;
  return motion({ t: clamp01(t), e: Math.max(0, elapsed), m: measure, p: entry?.p || {} });
}

export function genieParticle(mode, index, count, t, elapsed, seed) {
  const r = ((Math.sin((index + 1) * 12.9898 + seed * 78.233) * 43758.5453) % 1 + 1) % 1;
  const q = ((Math.sin((index + 1) * 39.3468 + seed * 11.135) * 24634.6345) % 1 + 1) % 1;
  const a = r * TAU;
  if (mode === "burst") {
    const local = (t * 2) % 1;
    const out = ease(Math.min(1, local * 2.2)) * (1.6 + q * 1.4);
    return local > 0.7 ? null : [Math.cos(a) * out, 1 + Math.sin(q * TAU) * out * 0.6 + local, Math.sin(a) * out];
  }
  if (mode === "rise") {
    const local = (t * 2 + r) % 1;
    return [Math.sin(r * 20) * 0.8, 0.8 + local * 3, Math.cos(r * 20) * 0.8];
  }
  if (mode === "rain") {
    const local = (t * 2.5 + r) % 1;
    return [(r - 0.5) * 5, 5.2 - local * 5.2, (q - 0.5) * 5];
  }
  if (mode === "orbit") {
    const angle = elapsed * 3 + (index / count) * TAU;
    return [Math.cos(angle) * 1.2, 1.9 + Math.sin(elapsed * 4 + index) * 0.2, Math.sin(angle) * 1.2];
  }
  if (mode === "fountain") {
    const local = (t * 2 + r) % 1;
    return [Math.cos(a) * local * 2, 1 + 4 * local - 5 * local * local, Math.sin(a) * local * 2];
  }
  if (mode === "spiral") {
    const angle = t * 12 + (index / count) * TAU;
    const height = ((t * 2 + index / count) % 1) * 4;
    const radius = 0.4 + height * 0.35;
    return [Math.cos(angle) * radius, height, Math.sin(angle) * radius];
  }
  if (mode === "twinkle") {
    if (Math.sin(elapsed * 6 + index * 3) < 0) return null;
    return [(r - 0.5) * 4, 0.6 + q * 2.6, (Math.sin(index * 7.1) * 0.5) * 4];
  }
  if (mode === "trail") {
    const local = (t * 3 + index / count) % 1;
    return [0, 0.3 + local * 1.2, -local * 1.4];
  }
  return null;
}

export function paintGenieSpeech(context, text, width = 512, height = 128) {
  context.clearRect(0, 0, width, height);
  context.font = "600 40px system-ui, sans-serif";
  const label = String(text || "").slice(0, 40);
  const measured = Math.min(width - 24, context.measureText(label).width + 48);
  const left = (width - measured) / 2;
  context.fillStyle = "rgba(14, 14, 30, 0.92)";
  context.strokeStyle = "rgba(64, 242, 219, 0.75)";
  context.lineWidth = 4;
  context.beginPath();
  context.roundRect(left, 10, measured, 84, 26);
  context.moveTo(width / 2 - 14, 92);
  context.lineTo(width / 2, 116);
  context.lineTo(width / 2 + 14, 92);
  context.fill();
  context.stroke();
  context.fillStyle = "#eef0f7";
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillText(label, width / 2, 53);
}
