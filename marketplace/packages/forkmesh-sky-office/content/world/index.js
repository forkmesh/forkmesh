// ForkMesh Aquarium Sky Office — a marketplace World district package.
//
// Everything the destination needs lives in this file: geometry, rides,
// floors, access rules, sound profiles, camera bounds, and the walk surface.
// The World host installs it through `install(host)` below and offers only
// bounded capabilities (THREE, shared raycast/animation arrays, the avatar,
// a sound emitter, floor/space setters, and access hydration). The module
// imports nothing and performs no network requests.
//
// The aquarium sky office is deliberately independent from world-office-tower.js.
// The latter is the long-lived team Office and must keep its floor ids, island,
// access rules, and scene lifecycle.  This module adds a second destination.

// Keep the mainland station where Town visitors already find it, but push the
// destination itself twenty times farther down the established northbound
// travel axis. At Town scale the 368-unit tower is now only a horizon sliver;
// the express remains the intentional way to reach it.
export const FORKMESH_SKY_OFFICE_DISTANCE_MULTIPLIER = 20;
const FORKMESH_SKY_OFFICE_REFERENCE_CENTER = Object.freeze([-330, 0, -720]);
export const FORKMESH_SKY_OFFICE_CENTER = Object.freeze([
  FORKMESH_SKY_OFFICE_REFERENCE_CENTER[0],
  FORKMESH_SKY_OFFICE_REFERENCE_CENTER[1],
  FORKMESH_SKY_OFFICE_REFERENCE_CENTER[2] *
    FORKMESH_SKY_OFFICE_DISTANCE_MULTIPLIER,
]);
export const FORKMESH_SKY_OFFICE_ISLAND_RADIUS = 112;
export const FORKMESH_SKY_OFFICE_RADIUS = 85;
export const FORKMESH_SKY_OFFICE_AQUARIUM_DEPTH = 6;
export const FORKMESH_SKY_OFFICE_FLOOR_HEIGHT = 16;
export const FORKMESH_SKY_OFFICE_LOUNGE_Y = 16;
// From the top of the glass lounge to the summit slab, the clear lift tube is
// exactly twenty times one original-office storey tall.
export const FORKMESH_SKY_OFFICE_ELEVATOR_RISE = 320;
export const FORKMESH_SKY_OFFICE_SUMMIT_Y =
  FORKMESH_SKY_OFFICE_LOUNGE_Y +
  FORKMESH_SKY_OFFICE_FLOOR_HEIGHT +
  FORKMESH_SKY_OFFICE_ELEVATOR_RISE;
export const FORKMESH_SKY_OFFICE_ROOF_Y =
  FORKMESH_SKY_OFFICE_SUMMIT_Y + FORKMESH_SKY_OFFICE_FLOOR_HEIGHT;
export const FORKMESH_SKY_OFFICE_FRONT_Z = 85;
export const FORKMESH_SKY_OFFICE_ELEVATOR_Z = 55;
export const FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z = -138;
export const FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z =
  FORKMESH_SKY_OFFICE_CENTER[2] + FORKMESH_SKY_OFFICE_FRONT_Z + 10;
export const FORKMESH_SKY_OFFICE_HYPERLOOP_WIDTH = 14;
// This includes the complete distant silhouette and the always-available
// express infrastructure. The furnished tower is behind its own LOD and does
// not contribute while a visitor is in Town.
export const FORKMESH_SKY_OFFICE_DISTANT_TRIANGLE_BUDGET = 900;

// Aquarium lighting is intentionally a small, explicit palette rather than a
// binary on/off switch. The last setting is animated in the render loop, so it
// can drift through RGB without allocating a new light or material each frame.
export const FORKMESH_SKY_OFFICE_LIGHT_SETTINGS = Object.freeze([
  Object.freeze({
    id: "daylight",
    label: "DAYLIGHT",
    color: "#7deeff",
    waterColor: "#087fba",
    waterEmissive: "#064d76",
    intensity: 2.5,
    waterIntensity: 0.34,
  }),
  Object.freeze({
    id: "moonlight",
    label: "MOONLIGHT",
    color: "#8f9dff",
    waterColor: "#143f86",
    waterEmissive: "#0b1d55",
    intensity: 1.65,
    waterIntensity: 0.24,
  }),
  Object.freeze({
    id: "sunset",
    label: "SUNSET",
    color: "#ff9d67",
    waterColor: "#88536d",
    waterEmissive: "#54264c",
    intensity: 2.1,
    waterIntensity: 0.3,
  }),
  Object.freeze({
    id: "rgb-slow-fade",
    label: "RGB SLOW FADE",
    color: "#ff7ee9",
    waterColor: "#4e5fc4",
    waterEmissive: "#291d70",
    intensity: 2.35,
    waterIntensity: 0.32,
  }),
]);

export const FORKMESH_SKY_OFFICE_FLOORS = Object.freeze([
  Object.freeze({
    id: "aquarium-lobby",
    level: 1,
    elevation: 0,
    label: "Aquarium Lobby",
    public: true,
  }),
  Object.freeze({
    id: "glass-lounge",
    level: 2,
    elevation: FORKMESH_SKY_OFFICE_LOUNGE_Y,
    label: "Glass Lounge",
    public: true,
  }),
  Object.freeze({
    id: "summit-office",
    level: 3,
    elevation: FORKMESH_SKY_OFFICE_SUMMIT_Y,
    label: "Forkmesh Office",
    organizationOnly: true,
  }),
  Object.freeze({
    id: "drone-roof",
    level: 4,
    elevation: FORKMESH_SKY_OFFICE_ROOF_Y,
    label: "Drone Roof",
    organizationOnly: true,
  }),
]);

// Every audible system in this destination has a named cue. Keeping the
// contract exported makes omissions visible in tests and keeps WebAudio code
// out of the geometry module.
export const FORKMESH_SKY_OFFICE_SOUND_EVENTS = Object.freeze([
  "sky-office-door-open",
  "sky-office-door-close",
  "sky-office-aquarium-water",
  "sky-office-aquarium-bubbles",
  "sky-office-aquarium-fish",
  "sky-office-aquarium-feed",
  "sky-office-aquarium-tap",
  "sky-office-aquarium-light",
  "sky-office-elevator-button",
  "sky-office-elevator-denied",
  "sky-office-elevator-door-close",
  "sky-office-elevator-start",
  "sky-office-elevator-motor",
  "sky-office-elevator-collar",
  "sky-office-elevator-brake",
  "sky-office-elevator-arrive",
  "sky-office-elevator-door-open",
  "sky-office-hyperloop-station",
  "sky-office-hyperloop-load",
  "sky-office-hyperloop-seal",
  "sky-office-hyperloop-charge",
  "sky-office-hyperloop-launch",
  "sky-office-hyperloop-whoosh",
  "sky-office-hyperloop-brake",
  "sky-office-hyperloop-arrive",
  "sky-office-hyperloop-unseal",
  "sky-office-cube-hum",
  "sky-office-cube-touch",
  "sky-office-reception-chime",
  "sky-office-lounge-seat",
  "sky-office-footstep",
  "sky-office-widget",
  "sky-office-summit-electronics",
  "sky-office-roof-wind",
  "sky-office-drone-pad",
]);

const FLOOR_BY_ID = new Map(
  FORKMESH_SKY_OFFICE_FLOORS.map((floor) => [floor.id, floor]),
);

export function forkmeshSkyOfficeFloorById(value) {
  return FLOOR_BY_ID.get(String(value || "").trim().toLowerCase()) || null;
}

export function normalizeForkmeshSkyOfficeAccess(payload = {}) {
  const authenticated = payload?.authenticated === true;
  const organizationRole = ["owner", "admin", "member"].includes(
    String(payload?.organizationRole || "").trim().toLowerCase(),
  )
    ? String(payload.organizationRole).trim().toLowerCase()
    : "";
  const allowedFloorIds = ["aquarium-lobby", "glass-lounge"];
  if (authenticated && organizationRole) {
    allowedFloorIds.push("summit-office", "drone-roof");
  }
  return {
    authenticated,
    organizationRole,
    account: authenticated
      ? String(payload?.account || "").trim().toLowerCase().slice(0, 64)
      : "",
    allowedFloorIds,
  };
}

export function canAccessForkmeshSkyOfficeFloor(access, floorId) {
  const floor = forkmeshSkyOfficeFloorById(floorId);
  return Boolean(
    floor &&
      normalizeForkmeshSkyOfficeAccess(access).allowedFloorIds.includes(
        floor.id,
      ),
  );
}

export function forkmeshSkyOfficeSurfaceContains(x, z, radius = 0) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  const island = Math.hypot(
    px - FORKMESH_SKY_OFFICE_CENTER[0],
    pz - FORKMESH_SKY_OFFICE_CENTER[2],
  );
  if (island <= FORKMESH_SKY_OFFICE_ISLAND_RADIUS - margin) return true;
  return (
    Math.abs(px - FORKMESH_SKY_OFFICE_CENTER[0]) <=
      FORKMESH_SKY_OFFICE_HYPERLOOP_WIDTH / 2 - margin &&
    pz <= FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z + margin &&
    pz >= FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z - margin
  );
}

function makeMaterial(THREE, color, options = {}) {
  return new THREE.MeshStandardMaterial({
    color,
    roughness: 0.55,
    metalness: 0.12,
    ...options,
  });
}

function triangleCount(root) {
  let total = 0;
  root?.traverse?.((child) => {
    if (!child.isMesh || !child.geometry) return;
    const position = child.geometry.attributes?.position;
    if (!position) return;
    const vertices = child.geometry.index
      ? child.geometry.index.count
      : position.count;
    total +=
      (vertices / 3) * (child.isInstancedMesh ? child.count : 1);
  });
  return Math.round(total);
}

function makeCanvasTexture(THREE, width, height, paint) {
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const context = canvas.getContext("2d");
  paint(context, width, height);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.needsUpdate = true;
  return texture;
}

function makePanel(THREE, title, subtitle, color = "#7df3ff", width = 15) {
  const textureFor = (nextTitle, nextSubtitle, nextColor) =>
    makeCanvasTexture(THREE, 1024, 256, (context) => {
      context.fillStyle = "#061714";
      context.fillRect(0, 0, 1024, 256);
      context.strokeStyle = nextColor;
      context.lineWidth = 12;
      context.strokeRect(8, 8, 1008, 240);
      context.textAlign = "center";
      context.textBaseline = "middle";
      context.fillStyle = "#efffff";
      context.font = '900 68px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(nextTitle, 512, 94, 940);
      context.fillStyle = nextColor;
      context.font = '700 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(nextSubtitle, 512, 177, 940);
    });
  const panel = new THREE.Mesh(
    new THREE.PlaneGeometry(width, width / 4),
    new THREE.MeshBasicMaterial({
      map: textureFor(title, subtitle, color),
      toneMapped: false,
    }),
  );
  // The light console updates its subtitle on every click. Keeping the repaint
  // next to panel construction prevents a stale mode label from claiming a
  // different setting than the tank is actually showing.
  panel.userData.setPanelText = (
    nextTitle = title,
    nextSubtitle = subtitle,
    nextColor = color,
  ) => {
    const previousTexture = panel.material.map;
    panel.material.map = textureFor(nextTitle, nextSubtitle, nextColor);
    panel.material.needsUpdate = true;
    previousTexture?.dispose?.();
  };
  return panel;
}

function makeSkyOfficeFloorTexture(THREE, style) {
  return makeCanvasTexture(THREE, 768, 768, (context, width, height) => {
    const center = width / 2;
    context.fillStyle = style.base;
    context.fillRect(0, 0, width, height);

    // Fine concentric inlays remain legible through glass without creating
    // dozens of separate meshes, and the sparse tile lines give each floor a
    // material identity when the camera moves close to the surface.
    context.strokeStyle = style.vein;
    context.globalAlpha = 0.32;
    context.lineWidth = 3;
    for (let radius = 72; radius < center; radius += 86) {
      context.beginPath();
      context.arc(center, center, radius, 0, Math.PI * 2);
      context.stroke();
    }
    context.globalAlpha = 0.2;
    context.lineWidth = 2;
    for (let offset = -center; offset <= center; offset += 96) {
      context.beginPath();
      context.moveTo(center + offset, 0);
      context.lineTo(center + offset * 0.68, height);
      context.stroke();
      context.beginPath();
      context.moveTo(0, center + offset);
      context.lineTo(width, center + offset * 0.68);
      context.stroke();
    }
    context.globalAlpha = 0.52;
    context.fillStyle = style.accent;
    context.beginPath();
    context.arc(center, center, 16, 0, Math.PI * 2);
    context.fill();
    context.globalAlpha = 1;
  });
}

function setInteractive(root, interactive, type, data = {}) {
  root.traverse((child) => {
    if (!child.isMesh) return;
    child.userData.interactive = type;
    Object.assign(child.userData, data);
    interactive.push(child);
  });
}

function addCouch(THREE, parent, x, z, yaw, index, interactive) {
  const couch = new THREE.Group();
  couch.name = `forkmesh-sky-office-lounge-couch-${index}`;
  couch.position.set(x, 0, z);
  couch.rotation.y = yaw;
  const upholstery = makeMaterial(THREE, index % 2 ? "#4e8b78" : "#397c91", {
    roughness: 0.72,
  });
  const frame = makeMaterial(THREE, "#142a28", {
    metalness: 0.5,
    roughness: 0.36,
  });
  const base = new THREE.Mesh(new THREE.BoxGeometry(9, 0.8, 3.8), frame);
  base.position.y = 0.5;
  const seat = new THREE.Mesh(new THREE.BoxGeometry(8.6, 0.5, 3.6), upholstery);
  seat.position.y = 1.18;
  const back = new THREE.Mesh(new THREE.BoxGeometry(8.6, 2.8, 0.52), upholstery);
  back.position.set(0, 2.45, -1.58);
  couch.add(base, seat, back);
  seat.userData.interactive = "forkmesh-sky-office-couch";
  seat.userData.forkmeshSkyOfficeCouch = index;
  seat.userData.worldSeat = true;
  seat.userData.worldSeatHeading = yaw + Math.PI;
  seat.userData.worldSeatActivity = "relaxing in the Forkmesh Office lounge";
  interactive.push(seat);
  parent.add(couch);
  return couch;
}

function addRoofSeat(THREE, parent, x, z, yaw, index, interactive) {
  const seat = new THREE.Group();
  seat.name = `forkmesh-sky-office-roof-seat-${index}`;
  seat.position.set(x, 0, z);
  seat.rotation.y = yaw;
  const frame = makeMaterial(THREE, "#183934", {
    metalness: 0.55,
    roughness: 0.34,
  });
  const cushion = makeMaterial(THREE, "#5ca68b", { roughness: 0.68 });
  const plank = new THREE.Mesh(new THREE.BoxGeometry(8, 0.35, 2.7), cushion);
  plank.position.y = 1.35;
  const back = new THREE.Mesh(new THREE.BoxGeometry(8, 2.4, 0.35), cushion);
  back.position.set(0, 2.5, -1.18);
  const pedestal = new THREE.Mesh(new THREE.BoxGeometry(6.8, 1.1, 2), frame);
  pedestal.position.y = 0.58;
  seat.add(plank, back, pedestal);
  plank.userData.interactive = "forkmesh-sky-office-roof-seat";
  plank.userData.forkmeshSkyOfficeRoofSeat = index;
  plank.userData.worldSeat = true;
  plank.userData.worldSeatHeading = yaw + Math.PI;
  plank.userData.worldSeatActivity = "sitting on the Forkmesh Office roof";
  interactive.push(plank);
  parent.add(seat);
  return seat;
}

// This is the same authored F/M sculpture used in the main Office lobby. The
// Sky Office intentionally uses ordinary Standard materials (rather than the
// main lobby's live cube-map capture) so its remote LOD never enables a costly
// reflection pass, but the silhouette, letter reliefs, and branch cut-outs are
// the exact same dimensions.
function createSkyOfficeForkmeshCube(THREE, interactive) {
  const cube = new THREE.Group();
  cube.name = "forkmesh-sky-office-spinning-fm-cube";
  cube.userData.forkmeshSkyOfficeCubeReplica = "forkmesh-reflective-fm-cube";

  const chrome = makeMaterial(THREE, "#dbe4ef", {
    metalness: 0.94,
    roughness: 0.055,
    emissive: "#0b1417",
    emissiveIntensity: 0.14,
  });
  const darkChrome = makeMaterial(THREE, "#05070b", {
    metalness: 1,
    roughness: 0.06,
  });
  const logoInnerFace = makeMaterial(THREE, "#030807", {
    metalness: 0.18,
    roughness: 0.72,
  });
  const logoPanelChrome = chrome.clone();
  logoPanelChrome.color.set("#f0f5ff");
  logoPanelChrome.metalness = 0.8;
  logoPanelChrome.roughness = 0.07;
  logoPanelChrome.emissive.set("#111c20");
  logoPanelChrome.emissiveIntensity = 0.16;

  const chromeMark = new THREE.Group();
  chromeMark.name = "forkmesh-reflective-fm-cube-fixed-tilt";
  cube.add(chromeMark);
  const logoHalfSize = 2.92;
  const logoLowerCorner = new THREE.Vector3(-1, -1, -1).normalize();
  chromeMark.quaternion.setFromUnitVectors(
    logoLowerCorner,
    new THREE.Vector3(0, -1, 0),
  );

  const logoPiece = (
    parent,
    width,
    height,
    depth,
    x,
    y,
    z,
    rotationZ = 0,
  ) => {
    const piece = new THREE.Mesh(
      new THREE.BoxGeometry(width, height, depth),
      [
        logoInnerFace,
        logoInnerFace,
        logoInnerFace,
        logoInnerFace,
        chrome,
        logoInnerFace,
      ],
    );
    piece.position.set(x, y, z);
    piece.rotation.z = rotationZ;
    parent.add(piece);
    return piece;
  };
  const logoLetterStroke = 1.04;

  const addFLogoFace = (z, rotationY) => {
    const face = new THREE.Group();
    face.name = `forkmesh-reflective-f-face-${z > 0 ? "front" : "back"}`;
    face.position.z = z;
    face.rotation.y = rotationY;
    logoPiece(face, logoLetterStroke, 4.9, 0.48, -1.64, -0.04, 0);
    logoPiece(face, 4.35, logoLetterStroke, 0.48, 0.12, 2.0, 0);
    logoPiece(face, 3.45, logoLetterStroke, 0.48, -0.33, 0.22, 0);
    chromeMark.add(face);
  };
  addFLogoFace(2.68, 0);
  addFLogoFace(-2.68, Math.PI);

  const addMLogoFace = (x, rotationY) => {
    const face = new THREE.Group();
    face.name = `forkmesh-reflective-m-face-${x > 0 ? "right" : "left"}`;
    face.position.x = x;
    face.rotation.y = rotationY;
    logoPiece(face, logoLetterStroke, 4.9, 0.48, -1.72, -0.04, 0);
    logoPiece(face, logoLetterStroke, 4.9, 0.48, 1.72, -0.04, 0);
    const addDiagonal = (fromX, fromY, toX, toY) => {
      const dx = toX - fromX;
      const dy = toY - fromY;
      logoPiece(
        face,
        logoLetterStroke,
        Math.hypot(dx, dy),
        0.48,
        (fromX + toX) / 2,
        (fromY + toY) / 2,
        0,
        Math.atan2(-dx, dy),
      );
    };
    addDiagonal(-1.72, 2.0, 0, 0.08);
    addDiagonal(0, 0.08, 1.72, 2.0);
    chromeMark.add(face);
  };
  addMLogoFace(2.68, Math.PI / 2);
  addMLogoFace(-2.68, -Math.PI / 2);

  const logoPanelShape = new THREE.Shape();
  const panelSize = 5.72;
  const panelHalf = panelSize / 2;
  const panelRadius = 0.34;
  logoPanelShape.moveTo(-panelHalf + panelRadius, -panelHalf);
  logoPanelShape.lineTo(panelHalf - panelRadius, -panelHalf);
  logoPanelShape.quadraticCurveTo(
    panelHalf,
    -panelHalf,
    panelHalf,
    -panelHalf + panelRadius,
  );
  logoPanelShape.lineTo(panelHalf, panelHalf - panelRadius);
  logoPanelShape.quadraticCurveTo(
    panelHalf,
    panelHalf,
    panelHalf - panelRadius,
    panelHalf,
  );
  logoPanelShape.lineTo(-panelHalf + panelRadius, panelHalf);
  logoPanelShape.quadraticCurveTo(
    -panelHalf,
    panelHalf,
    -panelHalf,
    panelHalf - panelRadius,
  );
  logoPanelShape.lineTo(-panelHalf, -panelHalf + panelRadius);
  logoPanelShape.quadraticCurveTo(
    -panelHalf,
    -panelHalf,
    -panelHalf + panelRadius,
    -panelHalf,
  );
  logoPanelShape.closePath();
  const topNodes = [
    [-1.8, -1.3],
    [0, -1.8],
    [1.7, -0.4],
    [0.2, 1.7],
  ];
  const topBranches = [
    [[-1.8, -1.3], [0, -1.8]],
    [[0, -1.8], [1.7, -0.4]],
    [[0, -1.8], [0.2, 1.7]],
  ];
  const logoNodeCutoutRadius = 0.5;
  topNodes.forEach(([x, z]) => {
    const hole = new THREE.Path();
    hole.absarc(x, z, logoNodeCutoutRadius, 0, Math.PI * 2, false);
    logoPanelShape.holes.push(hole);
  });
  topBranches.forEach(([[x1, z1], [x2, z2]]) => {
    const dx = x2 - x1;
    const dz = z2 - z1;
    const length = Math.hypot(dx, dz);
    const ux = dx / length;
    const uz = dz / length;
    const halfWidth = 0.16;
    const nx = -uz * halfWidth;
    const nz = ux * halfWidth;
    const trim = logoNodeCutoutRadius + 0.018;
    const sx = x1 + ux * trim;
    const sz = z1 + uz * trim;
    const ex = x2 - ux * trim;
    const ez = z2 - uz * trim;
    const hole = new THREE.Path();
    hole.moveTo(sx + nx, sz + nz);
    hole.lineTo(ex + nx, ez + nz);
    hole.lineTo(ex - nx, ez - nz);
    hole.lineTo(sx - nx, sz - nz);
    hole.closePath();
    logoPanelShape.holes.push(hole);
  });
  const logoPanelGeometry = new THREE.ExtrudeGeometry(logoPanelShape, {
    depth: 0.36,
    steps: 1,
    curveSegments: 16,
    bevelEnabled: true,
    bevelSegments: 2,
    bevelSize: 0.08,
    bevelThickness: 0.08,
  });
  logoPanelGeometry.translate(0, 0, -0.18);
  logoPanelGeometry.rotateX(Math.PI / 2);
  for (const faceY of [-2.68, 2.68]) {
    const panel = new THREE.Mesh(logoPanelGeometry, logoPanelChrome);
    panel.name = `forkmesh-reflective-fm-panel-${faceY > 0 ? "top" : "bottom"}`;
    panel.position.y = faceY;
    panel.userData.logoThroughCutouts = logoPanelShape.holes.length;
    chromeMark.add(panel);
  }
  const logoContactPoint = new THREE.Mesh(
    new THREE.OctahedronGeometry(0.2, 0),
    darkChrome,
  );
  logoContactPoint.name = "forkmesh-reflective-fm-cube-contact-point";
  logoContactPoint.position.set(-logoHalfSize, -logoHalfSize, -logoHalfSize);
  chromeMark.add(logoContactPoint);

  setInteractive(cube, interactive, "forkmesh-sky-office-cube", {
    forkmeshSkyOfficeCubeReplica: "forkmesh-reflective-fm-cube",
  });
  return cube;
}

function interactiveSourceRoots(existingRoot) {
  const keywords = [
    "task-board",
    "initiatives-board",
    "marketing-roster",
    "marketing-table",
    "attendance-calendar",
    "engineering-debug",
    "system-capacity",
    "link-kiosk",
    "feedback-kiosk",
    "task-bid-kiosk",
    "link-rewards",
    "greeting-board",
    "attendance",
    "clock-in",
    "rooftop-laptop",
    "meeting-board",
  ];
  const roots = [];
  const seen = new Set();
  const inspect = (candidate) => {
    const name = String(candidate?.name || "").toLowerCase();
    if (!name || seen.has(candidate) || !keywords.some((key) => name.includes(key))) {
      return;
    }
    seen.add(candidate);
    roots.push(candidate);
  };
  (existingRoot?.children || []).forEach((child) => {
    inspect(child);
    if (child?.userData?.officeFloorGroup === true) {
      (child.children || []).forEach(inspect);
    }
  });
  return roots.slice(0, 28);
}

function createForkmeshSkyOfficeFull(
  THREE,
  { interactive = [], animated = [] } = {},
) {
  const group = new THREE.Group();
  group.name = "forkmesh-sky-office-destination";
  group.userData.landmark = "forkmesh-sky-office";

  const campus = new THREE.Group();
  campus.name = "forkmesh-sky-office-campus";
  campus.position.set(...FORKMESH_SKY_OFFICE_CENTER);
  group.add(campus);

  // The island belongs to the same LOD as the tower. Previously its 64-sided
  // cylinder and 1,024-triangle rim stayed live from Town even when the tower
  // itself had switched to a silhouette.
  const high = new THREE.Group();
  high.name = "forkmesh-sky-office-high-fidelity";
  const low = new THREE.Group();
  low.name = "forkmesh-sky-office-low-poly";
  const lod = new THREE.LOD();
  lod.name = "forkmesh-sky-office-distance-lod";
  lod.addLevel(high, 0, 0.08);
  lod.addLevel(low, 260, 0.12);
  campus.add(lod);
  group.userData.forkmeshSkyOfficeLod = lod;

  const island = new THREE.Mesh(
    new THREE.CylinderGeometry(
      FORKMESH_SKY_OFFICE_ISLAND_RADIUS,
      FORKMESH_SKY_OFFICE_ISLAND_RADIUS + 5,
      1.8,
      64,
    ),
    makeMaterial(THREE, "#356d58", { roughness: 0.96 }),
  );
  island.name = "forkmesh-sky-office-round-island";
  island.position.y = -0.82;
  island.receiveShadow = true;
  high.add(island);
  const islandRim = new THREE.Mesh(
    new THREE.TorusGeometry(FORKMESH_SKY_OFFICE_ISLAND_RADIUS, 0.72, 8, 64),
    makeMaterial(THREE, "#75efff", {
      emissive: "#158da9",
      emissiveIntensity: 1.1,
      metalness: 0.48,
      roughness: 0.24,
    }),
  );
  islandRim.rotation.x = Math.PI / 2;
  islandRim.position.y = 0.08;
  high.add(islandRim);

  const lowIsland = new THREE.Mesh(
    new THREE.CylinderGeometry(
      FORKMESH_SKY_OFFICE_ISLAND_RADIUS,
      FORKMESH_SKY_OFFICE_ISLAND_RADIUS + 4,
      1.4,
      6,
    ),
    makeMaterial(THREE, "#356d58", { roughness: 1 }),
  );
  lowIsland.name = "forkmesh-sky-office-low-island";
  lowIsland.position.y = -0.7;
  low.add(lowIsland);

  const structure = makeMaterial(THREE, "#173d35", {
    metalness: 0.72,
    roughness: 0.24,
  });
  const darkStructure = makeMaterial(THREE, "#071713", {
    metalness: 0.6,
    roughness: 0.34,
  });
  const glow = makeMaterial(THREE, "#80f5ff", {
    emissive: "#1b9fbd",
    emissiveIntensity: 1.35,
    metalness: 0.3,
    roughness: 0.2,
  });
  // Keep aquarium lighting separate from the building's wayfinding glow. A
  // console setting should tint the water and its fixtures, not unexpectedly
  // recolor the elevator collars or the entire office perimeter.
  const aquariumGlow = glow.clone();
  const aquariumLightRings = [];
  const floorStyles = Object.freeze({
    aquarium: Object.freeze({
      base: "#1d4e58",
      vein: "#8eefff",
      accent: "#f5cf74",
      roughness: 0.29,
      metalness: 0.52,
    }),
    lounge: Object.freeze({
      base: "#254a47",
      vein: "#b6fff0",
      accent: "#6de4c0",
      roughness: 0.24,
      metalness: 0.58,
    }),
    summit: Object.freeze({
      base: "#202e42",
      vein: "#a7cbff",
      accent: "#7df3ff",
      roughness: 0.21,
      metalness: 0.64,
    }),
    roof: Object.freeze({
      base: "#263b38",
      vein: "#8ac9bd",
      accent: "#9ef7c6",
      roughness: 0.42,
      metalness: 0.42,
    }),
  });
  // These translucent shells do not need screen-space refraction. Physical
  // transmission can force Three.js to render the scene into a separate buffer
  // before the normal pass, and the world-spanning route kept that path active
  // from Town. Phong glass keeps readable highlights and transparency without
  // adding this destination to the transmission pass.
  const clearGlass = new THREE.MeshPhongMaterial({
    color: "#d7ffff",
    specular: "#ffffff",
    shininess: 96,
    transparent: true,
    opacity: 0.2,
    side: THREE.DoubleSide,
    depthWrite: false,
  });
  const aquariumGlass = new THREE.MeshPhongMaterial({
    color: "#a9f3ff",
    specular: "#e8ffff",
    shininess: 82,
    transparent: true,
    opacity: 0.38,
    side: THREE.DoubleSide,
    depthWrite: false,
  });
  const aquariumWater = new THREE.MeshPhongMaterial({
    color: "#087fba",
    specular: "#80eaff",
    shininess: 54,
    transparent: true,
    opacity: 0.43,
    side: THREE.DoubleSide,
    depthWrite: false,
    emissive: "#064d76",
    emissiveIntensity: 0.3,
  });

  const addCylinder = (
    parent,
    name,
    radius,
    height,
    y,
    material,
    segments = 64,
    openEnded = true,
    thetaStart = 0,
    thetaLength = Math.PI * 2,
  ) => {
    const mesh = new THREE.Mesh(
      new THREE.CylinderGeometry(
        radius,
        radius,
        height,
        segments,
        1,
        openEnded,
        thetaStart,
        thetaLength,
      ),
      material,
    );
    mesh.name = name;
    mesh.position.y = y;
    parent.add(mesh);
    return mesh;
  };
  const addRing = (parent, name, radius, y, material, segments = 64) => {
    const ring = new THREE.Mesh(
      new THREE.TorusGeometry(radius, 0.28, 8, segments),
      material,
    );
    ring.name = name;
    ring.rotation.x = Math.PI / 2;
    ring.position.y = y;
    parent.add(ring);
    return ring;
  };
  const addSlab = (parent, name, radius, y, style = floorStyles.lounge) => {
    const slab = addCylinder(
      parent,
      name,
      radius,
      0.72,
      y + 0.36,
      darkStructure,
      64,
      false,
    );
    slab.receiveShadow = true;
    addRing(parent, `${name}-light`, radius, y + 0.58, glow);
    const finish = new THREE.Mesh(
      new THREE.CircleGeometry(Math.max(1, radius - 1.05), 48),
      makeMaterial(THREE, style.base, {
        map: makeSkyOfficeFloorTexture(THREE, style),
        roughness: style.roughness,
        metalness: style.metalness,
      }),
    );
    finish.name = `${name}-terrazzo-finish`;
    finish.rotation.x = -Math.PI / 2;
    finish.position.y = y + 0.735;
    finish.receiveShadow = true;
    parent.add(finish);
    const inlay = new THREE.Mesh(
      new THREE.RingGeometry(radius * 0.34, radius * 0.355, 48),
      makeMaterial(THREE, style.accent, {
        emissive: style.accent,
        emissiveIntensity: 0.34,
        roughness: 0.2,
        metalness: 0.7,
      }),
    );
    inlay.name = `${name}-brass-inlay`;
    inlay.rotation.x = -Math.PI / 2;
    inlay.position.y = y + 0.748;
    parent.add(inlay);
    return slab;
  };

  const radius = FORKMESH_SKY_OFFICE_RADIUS;
  const innerRadius = radius - FORKMESH_SKY_OFFICE_AQUARIUM_DEPTH;
  const doorwayHalfAngle = Math.asin(6 / innerRadius);
  const wallArc = Math.PI * 2 - doorwayHalfAngle * 2;
  addCylinder(
    high,
    "forkmesh-sky-office-aquarium-outer-glass",
    radius,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT / 2,
    aquariumGlass,
    64,
    true,
    doorwayHalfAngle,
    wallArc,
  );
  const innerGlass = addCylinder(
    high,
    "forkmesh-sky-office-aquarium-inner-glass",
    innerRadius,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT - 0.8,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT / 2,
    aquariumGlass,
    64,
    true,
    doorwayHalfAngle,
    wallArc,
  );
  innerGlass.userData.interactive = "forkmesh-sky-office-aquarium-tap";
  interactive.push(innerGlass);
  addCylinder(
    high,
    "forkmesh-sky-office-six-foot-water-wall",
    radius - FORKMESH_SKY_OFFICE_AQUARIUM_DEPTH / 2,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT - 1.1,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT / 2,
    aquariumWater,
    64,
    true,
    doorwayHalfAngle,
    wallArc,
  );
  const sand = new THREE.Mesh(
    new THREE.RingGeometry(innerRadius, radius, 64),
    makeMaterial(THREE, "#d8a461", { roughness: 0.94 }),
  );
  sand.name = "forkmesh-sky-office-aquarium-sand";
  sand.rotation.x = -Math.PI / 2;
  sand.position.y = 0.46;
  high.add(sand);
  for (const y of [0.35, FORKMESH_SKY_OFFICE_FLOOR_HEIGHT - 0.35]) {
    aquariumLightRings.push(
      addRing(
        high,
        `forkmesh-sky-office-aquarium-light-${y}`,
        radius - 0.2,
        y,
        aquariumGlow,
      ),
    );
    addRing(high, `forkmesh-sky-office-aquarium-frame-${y}`, innerRadius + 0.15, y, structure);
  }
  const aquariumLightRig = new THREE.PointLight("#7deeff", 2.5, 130, 1.75);
  aquariumLightRig.name = "forkmesh-sky-office-aquarium-light-rig";
  aquariumLightRig.position.set(0, 10.5, 0);
  aquariumLightRig.castShadow = false;
  high.add(aquariumLightRig);

  // The former stretched dodecahedra read as flying rocks. Each fish now has
  // a smooth body, a real tail, and two rounded side fins while keeping the
  // school in three instanced batches.
  const fishCount = 72;
  const fishSchool = new THREE.Group();
  fishSchool.name = "forkmesh-sky-office-aquarium-fish-school";
  const fishGeometry = new THREE.SphereGeometry(1, 12, 8);
  const fishMaterial = new THREE.MeshStandardMaterial({
    color: "#ffffff",
    roughness: 0.36,
    metalness: 0.08,
    vertexColors: true,
  });
  const fish = new THREE.InstancedMesh(fishGeometry, fishMaterial, fishCount);
  fish.name = "forkmesh-sky-office-aquarium-fish";
  const fishTailShape = new THREE.Shape();
  fishTailShape.moveTo(-0.9, 0);
  fishTailShape.lineTo(-1.62, 0.82);
  fishTailShape.lineTo(-1.48, 0);
  fishTailShape.lineTo(-1.62, -0.82);
  fishTailShape.closePath();
  const fishTail = new THREE.InstancedMesh(
    new THREE.ShapeGeometry(fishTailShape),
    new THREE.MeshStandardMaterial({
      color: "#ffffff",
      roughness: 0.29,
      metalness: 0.12,
      vertexColors: true,
      side: THREE.DoubleSide,
    }),
    fishCount,
  );
  fishTail.name = "forkmesh-sky-office-aquarium-fish-tails";
  const fishFins = new THREE.InstancedMesh(
    new THREE.SphereGeometry(1, 8, 6),
    new THREE.MeshStandardMaterial({
      color: "#ffffff",
      roughness: 0.38,
      metalness: 0.08,
      vertexColors: true,
    }),
    fishCount * 2,
  );
  fishFins.name = "forkmesh-sky-office-aquarium-fish-fins";
  const fishDummy = new THREE.Object3D();
  const fishFinDummy = new THREE.Object3D();
  const fishPartMatrix = new THREE.Matrix4();
  const fishStates = [];
  const fishColors = ["#ff8a3d", "#29a6ff", "#ffe15d", "#9f75ff", "#63e6be"];
  const writeFishPose = (state, index, angle, bob = 0) => {
    fishDummy.position.set(
      Math.sin(angle) * state.radius,
      state.y + bob,
      Math.cos(angle) * state.radius,
    );
    fishDummy.rotation.set(
      0,
      angle + (state.direction > 0 ? Math.PI / 2 : -Math.PI / 2),
      Math.sin(angle * 4 + index * 0.7) * 0.055,
    );
    fishDummy.scale.set(
      state.length * 1.55,
      state.length * 0.66,
      state.length * 0.48,
    );
    fishDummy.updateMatrix();
    fish.setMatrixAt(index, fishDummy.matrix);
    fishTail.setMatrixAt(index, fishDummy.matrix);
    [-1, 1].forEach((side, finIndex) => {
      fishFinDummy.position.set(0.14, 0.02, side * 0.76);
      fishFinDummy.rotation.set(side * 0.48, 0, 0);
      fishFinDummy.scale.set(0.66, 0.14, 0.52);
      fishFinDummy.updateMatrix();
      fishPartMatrix.multiplyMatrices(fishDummy.matrix, fishFinDummy.matrix);
      fishFins.setMatrixAt(index * 2 + finIndex, fishPartMatrix);
    });
  };
  for (let index = 0; index < fishCount; index += 1) {
    const angle = doorwayHalfAngle + ((index + 0.5) / fishCount) * wallArc;
    const fishRadius = innerRadius + 1.2 + (index % 4) * 1.05;
    const length = 0.72 + (index % 6) * 0.11;
    const baseSpeed = 0.000045 + (index % 7) * 0.000006;
    const state = {
      angle,
      radius: fishRadius,
      y: 2.1 + (index % 9) * 1.22,
      length,
      direction: index % 2 ? 1 : -1,
      baseSpeed,
      speed: baseSpeed,
      feedBoostUntil: -Infinity,
    };
    fishStates.push(state);
    writeFishPose(state, index, angle);
    const bodyColor = new THREE.Color(fishColors[index % fishColors.length]);
    const accentColor = bodyColor.clone().offsetHSL(0.035, 0.14, 0.1);
    fish.setColorAt(index, bodyColor);
    fishTail.setColorAt(index, accentColor);
    fishFins.setColorAt(index * 2, accentColor);
    fishFins.setColorAt(index * 2 + 1, accentColor);
  }
  fish.instanceMatrix.needsUpdate = true;
  fish.instanceColor.needsUpdate = true;
  fishTail.instanceMatrix.needsUpdate = true;
  fishTail.instanceColor.needsUpdate = true;
  fishFins.instanceMatrix.needsUpdate = true;
  fishFins.instanceColor.needsUpdate = true;
  fishSchool.add(fish, fishTail, fishFins);
  high.add(fishSchool);

  // Soft reef clusters replace the pointed cone spikes. The low-segment
  // spheres still read as stylized coral, but every silhouette is rounded.
  const coralCount = 48;
  const coral = new THREE.InstancedMesh(
    new THREE.SphereGeometry(0.84, 10, 7),
    new THREE.MeshStandardMaterial({
      color: "#ff7ca8",
      roughness: 0.66,
      vertexColors: true,
    }),
    coralCount,
  );
  coral.name = "forkmesh-sky-office-aquarium-coral";
  coral.userData.spikeFree = true;
  for (let index = 0; index < coralCount; index += 1) {
    const angle = doorwayHalfAngle + ((index + 0.5) / coralCount) * wallArc;
    const coralRadius = innerRadius + 2 + (index % 3) * 1.2;
    fishDummy.position.set(
      Math.sin(angle) * coralRadius,
      1.08 + (index % 3) * 0.12,
      Math.cos(angle) * coralRadius,
    );
    fishDummy.rotation.set(0, -angle, 0);
    fishDummy.scale.set(
      0.72 + (index % 4) * 0.1,
      1.18 + (index % 5) * 0.12,
      0.72 + ((index + 2) % 4) * 0.1,
    );
    fishDummy.updateMatrix();
    coral.setMatrixAt(index, fishDummy.matrix);
    coral.setColorAt(
      index,
      new THREE.Color(["#ff6f91", "#f6c45b", "#8c72ff", "#55dca1"][index % 4]),
    );
  }
  coral.instanceMatrix.needsUpdate = true;
  coral.instanceColor.needsUpdate = true;
  high.add(coral);

  // Food remains visible long enough for the school to gather around it: a
  // bright falling pellet column plus a pulsing surface marker makes the feed
  // action readable even through the aquarium glass.
  const fishFood = new THREE.Group();
  fishFood.name = "forkmesh-sky-office-aquarium-food";
  const fishFoodAngle = doorwayHalfAngle + 0.46;
  const fishFoodRadius = innerRadius + 2.45;
  const fishFoodOrigin = new THREE.Vector3(
    Math.sin(fishFoodAngle) * fishFoodRadius,
    14.2,
    Math.cos(fishFoodAngle) * fishFoodRadius,
  );
  const fishFoodPellets = new THREE.InstancedMesh(
    new THREE.SphereGeometry(0.2, 8, 6),
    makeMaterial(THREE, "#f7c96b", {
      emissive: "#d98e28",
      emissiveIntensity: 0.86,
      roughness: 0.36,
    }),
    42,
  );
  fishFoodPellets.name = "forkmesh-sky-office-aquarium-food-pellets";
  const fishFoodPulse = new THREE.Mesh(
    new THREE.TorusGeometry(1.28, 0.075, 6, 28),
    makeMaterial(THREE, "#ffe3a6", {
      emissive: "#f5b342",
      emissiveIntensity: 1.2,
      roughness: 0.2,
    }),
  );
  fishFoodPulse.name = "forkmesh-sky-office-aquarium-food-bloom";
  fishFoodPulse.rotation.x = Math.PI / 2;
  fishFoodPulse.position.set(fishFoodOrigin.x, 1.55, fishFoodOrigin.z);
  const fishFoodDummy = new THREE.Object3D();
  const fishFoodStates = [];
  for (let index = 0; index < 42; index += 1) {
    const state = {
      angleOffset: ((index % 7) - 3) * 0.012,
      yOffset: (index % 6) * 0.55,
      drift: 0.12 + (index % 5) * 0.05,
      phase: index * 0.74,
    };
    fishFoodStates.push(state);
    fishFoodDummy.position.set(
      Math.sin(fishFoodAngle + state.angleOffset) * fishFoodRadius,
      fishFoodOrigin.y + state.yOffset,
      Math.cos(fishFoodAngle + state.angleOffset) * fishFoodRadius,
    );
    fishFoodDummy.scale.setScalar(0.78 + (index % 3) * 0.18);
    fishFoodDummy.updateMatrix();
    fishFoodPellets.setMatrixAt(index, fishFoodDummy.matrix);
  }
  fishFoodPellets.instanceMatrix.needsUpdate = true;
  fishFood.add(fishFoodPellets, fishFoodPulse);
  fishFood.visible = false;
  high.add(fishFood);

  const floorGroups = new Map();
  const lobby = new THREE.Group();
  lobby.name = "forkmesh-sky-office-floor-aquarium-lobby";
  lobby.userData.forkmeshSkyOfficeFloorId = "aquarium-lobby";
  addSlab(
    lobby,
    "forkmesh-sky-office-lobby-slab",
    innerRadius - 0.6,
    0,
    floorStyles.aquarium,
  );
  high.add(lobby);
  floorGroups.set("aquarium-lobby", lobby);

  const lounge = new THREE.Group();
  lounge.name = "forkmesh-sky-office-floor-glass-lounge";
  lounge.position.y = FORKMESH_SKY_OFFICE_LOUNGE_Y;
  lounge.userData.forkmeshSkyOfficeFloorId = "glass-lounge";
  addSlab(
    lounge,
    "forkmesh-sky-office-lounge-slab",
    radius - 0.6,
    0,
    floorStyles.lounge,
  );
  addCylinder(
    lounge,
    "forkmesh-sky-office-lounge-glass",
    radius,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT / 2,
    clearGlass,
  );
  for (let index = 0; index < 24; index += 1) {
    const angle = (index / 24) * Math.PI * 2;
    const mullion = new THREE.Mesh(
      new THREE.BoxGeometry(0.25, 15.2, 0.25),
      structure,
    );
    mullion.position.set(
      Math.sin(angle) * (radius - 0.28),
      FORKMESH_SKY_OFFICE_FLOOR_HEIGHT / 2,
      Math.cos(angle) * (radius - 0.28),
    );
    mullion.rotation.y = angle;
    lounge.add(mullion);
  }
  addCouch(THREE, lounge, -35, 5, Math.PI / 3, 1, interactive);
  addCouch(THREE, lounge, 35, 5, -Math.PI / 3, 2, interactive);
  addCouch(THREE, lounge, -23, -35, Math.PI / 7, 3, interactive);
  addCouch(THREE, lounge, 23, -35, -Math.PI / 7, 4, interactive);
  high.add(lounge);
  floorGroups.set("glass-lounge", lounge);

  const summit = new THREE.Group();
  summit.name = "forkmesh-sky-office-floor-summit-office";
  summit.position.y = FORKMESH_SKY_OFFICE_SUMMIT_Y;
  summit.userData.forkmeshSkyOfficeFloorId = "summit-office";
  addSlab(
    summit,
    "forkmesh-sky-office-summit-slab",
    radius,
    0,
    floorStyles.summit,
  );
  addCylinder(
    summit,
    "forkmesh-sky-office-summit-glass",
    radius,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT,
    FORKMESH_SKY_OFFICE_FLOOR_HEIGHT / 2,
    clearGlass,
  );
  const summitSign = makePanel(
    THREE,
    "FORKMESH OFFICE",
    "ONE ORGANIZED SUMMIT · LIVE WIDGETS & BOARDS",
    "#79efff",
    34,
  );
  summitSign.name = "forkmesh-sky-office-summit-sign";
  summitSign.position.set(0, 11.2, radius - 0.5);
  summit.add(summitSign);
  const summitContent = new THREE.Group();
  summitContent.name = "forkmesh-sky-office-summit-widget-copies";
  summit.add(summitContent);
  high.add(summit);
  floorGroups.set("summit-office", summit);

  const roof = new THREE.Group();
  roof.name = "forkmesh-sky-office-floor-drone-roof";
  roof.position.y = FORKMESH_SKY_OFFICE_ROOF_Y;
  roof.userData.forkmeshSkyOfficeFloorId = "drone-roof";
  addSlab(
    roof,
    "forkmesh-sky-office-roof-slab",
    radius + 2.4,
    0,
    floorStyles.roof,
  );
  addCylinder(
    roof,
    "forkmesh-sky-office-roof-glass-rail",
    radius + 1.2,
    4.2,
    2.1,
    clearGlass,
  );
  addRoofSeat(THREE, roof, -42, 10, Math.PI / 3, 1, interactive);
  addRoofSeat(THREE, roof, 42, 10, -Math.PI / 3, 2, interactive);
  const dronePad = new THREE.Mesh(
    new THREE.CylinderGeometry(17, 17, 0.42, 48),
    makeMaterial(THREE, "#172a2c", {
      emissive: "#11566d",
      emissiveIntensity: 0.58,
      metalness: 0.64,
      roughness: 0.28,
    }),
  );
  dronePad.name = "forkmesh-sky-office-drone-landing-pad";
  dronePad.position.set(0, 0.85, -32);
  dronePad.userData.interactive = "forkmesh-sky-office-drone-pad";
  interactive.push(dronePad);
  roof.add(dronePad);
  const droneH = makePanel(THREE, "H", "DRONE LANDING PAD", "#7df3ff", 12);
  droneH.name = "forkmesh-sky-office-drone-pad-h";
  droneH.rotation.x = -Math.PI / 2;
  droneH.position.set(0, 1.08, -32);
  roof.add(droneH);
  high.add(roof);
  floorGroups.set("drone-roof", roof);

  // Entrance -> elevator -> spinning cube -> reception remains a single clear
  // sightline. Furniture never blocks the aquarium glass.
  const elevatorCenter = new THREE.Vector3(0, 0, FORKMESH_SKY_OFFICE_ELEVATOR_Z);
  const tube = addCylinder(
    high,
    "forkmesh-sky-office-glass-tube-elevator",
    5.75,
    FORKMESH_SKY_OFFICE_ROOF_Y,
    FORKMESH_SKY_OFFICE_ROOF_Y / 2,
    clearGlass,
    32,
  );
  tube.position.z = FORKMESH_SKY_OFFICE_ELEVATOR_Z;
  for (const [x, z] of [[-5, -5], [5, -5], [-5, 5], [5, 5]]) {
    const rail = new THREE.Mesh(
      new THREE.BoxGeometry(0.25, FORKMESH_SKY_OFFICE_ROOF_Y, 0.25),
      structure,
    );
    rail.position.set(
      x,
      FORKMESH_SKY_OFFICE_ROOF_Y / 2,
      FORKMESH_SKY_OFFICE_ELEVATOR_Z + z,
    );
    high.add(rail);
  }
  for (let y = 32; y <= FORKMESH_SKY_OFFICE_SUMMIT_Y; y += 32) {
    const collar = addRing(
      high,
      `forkmesh-sky-office-elevator-collar-${y}`,
      5.9,
      y,
      glow,
      32,
    );
    collar.position.z = FORKMESH_SKY_OFFICE_ELEVATOR_Z;
  }

  const elevatorCar = new THREE.Group();
  elevatorCar.name = "forkmesh-sky-office-glass-elevator-car";
  elevatorCar.position.copy(elevatorCenter);
  const carFloor = new THREE.Mesh(
    new THREE.CylinderGeometry(5.1, 5.1, 0.34, 32),
    darkStructure,
  );
  carFloor.position.y = 0.18;
  elevatorCar.add(carFloor);
  const carCeiling = carFloor.clone();
  carCeiling.position.y = 7.1;
  elevatorCar.add(carCeiling);
  const carGlass = addCylinder(
    elevatorCar,
    "forkmesh-sky-office-elevator-car-glass",
    5,
    6.8,
    3.65,
    clearGlass,
    24,
  );
  carGlass.userData.forkmeshSkyOfficeNonInteractive = true;
  const elevatorDoors = [-1, 1].map((side) => {
    const door = new THREE.Mesh(
      new THREE.BoxGeometry(4.55, 6.4, 0.16),
      aquariumGlass.clone(),
    );
    door.name = `forkmesh-sky-office-elevator-door-${side < 0 ? "left" : "right"}`;
    door.position.set(side * 2.32, 3.5, -4.92);
    elevatorCar.add(door);
    return door;
  });
  const panel = new THREE.Mesh(
    new THREE.BoxGeometry(5.4, 5.4, 0.42),
    darkStructure,
  );
  panel.name = "forkmesh-sky-office-elevator-panel";
  panel.position.set(-4.4, 3.4, 0);
  panel.rotation.y = Math.PI / 2;
  elevatorCar.add(panel);
  const elevatorButtons = [];
  FORKMESH_SKY_OFFICE_FLOORS.forEach((floor, index) => {
    const button = new THREE.Mesh(
      new THREE.BoxGeometry(1.55, 0.82, 0.3),
      makeMaterial(THREE, floor.public ? "#2d8063" : "#4c4145", {
        emissive: floor.public ? "#1f9c6a" : "#6e2737",
        emissiveIntensity: floor.public ? 0.9 : 0.22,
        metalness: 0.48,
        roughness: 0.32,
      }),
    );
    button.name = `forkmesh-sky-office-elevator-button-${floor.id}`;
    button.position.set(-4.66, 4.9 - index * 1.18, 0);
    button.rotation.y = Math.PI / 2;
    button.userData.interactive = "forkmesh-sky-office-elevator-button";
    button.userData.forkmeshSkyOfficeFloorId = floor.id;
    interactive.push(button);
    elevatorButtons.push(button);
    elevatorCar.add(button);
  });
  high.add(elevatorCar);

  const cubeMount = new THREE.Group();
  cubeMount.name = "forkmesh-sky-office-spinning-cube";
  cubeMount.position.set(0, 6.6, 28);
  const cube = createSkyOfficeForkmeshCube(THREE, interactive);
  cubeMount.add(cube);
  const cubeHalo = new THREE.Mesh(new THREE.TorusGeometry(6, 0.22, 10, 48), glow);
  cubeHalo.rotation.x = Math.PI / 2;
  cubeMount.add(cubeHalo);
  lobby.add(cubeMount);

  const reception = new THREE.Group();
  reception.name = "forkmesh-sky-office-reception";
  reception.position.set(0, 0, -24);
  const receptionDesk = new THREE.Mesh(
    new THREE.BoxGeometry(30, 1.7, 5),
    makeMaterial(THREE, "#5b493c", { metalness: 0.25, roughness: 0.55 }),
  );
  receptionDesk.position.y = 1.05;
  receptionDesk.userData.interactive = "forkmesh-sky-office-reception";
  interactive.push(receptionDesk);
  reception.add(receptionDesk);
  const receptionist = new THREE.Group();
  receptionist.name = "forkmesh-sky-office-receptionist";
  receptionist.position.set(0, 0, -27.1);
  const receptionistUniform = makeMaterial(THREE, "#295d71", {
    metalness: 0.32,
    roughness: 0.48,
  });
  const receptionistSkin = makeMaterial(THREE, "#d59b72", {
    roughness: 0.62,
  });
  const receptionistHair = makeMaterial(THREE, "#172830", {
    metalness: 0.32,
    roughness: 0.3,
  });
  const receptionistBody = new THREE.Mesh(
    new THREE.CylinderGeometry(1.32, 1.5, 3.15, 12),
    receptionistUniform,
  );
  receptionistBody.name = "forkmesh-sky-office-receptionist-uniform";
  receptionistBody.position.y = 3.25;
  const receptionistHead = new THREE.Mesh(
    new THREE.SphereGeometry(1.2, 14, 10),
    receptionistSkin,
  );
  receptionistHead.name = "forkmesh-sky-office-receptionist-head";
  receptionistHead.position.y = 5.75;
  const receptionistHairCap = new THREE.Mesh(
    new THREE.SphereGeometry(1.26, 14, 8, 0, Math.PI * 2, 0, 1.68),
    receptionistHair,
  );
  receptionistHairCap.name = "forkmesh-sky-office-receptionist-hair";
  receptionistHairCap.position.set(0, 6.08, -0.08);
  const receptionistArm = new THREE.Mesh(
    new THREE.CylinderGeometry(0.3, 0.36, 2.65, 10),
    receptionistUniform,
  );
  receptionistArm.name = "forkmesh-sky-office-receptionist-wave-arm";
  receptionistArm.position.set(1.55, 3.6, 0.05);
  receptionistArm.rotation.z = -0.3;
  const receptionistHand = new THREE.Mesh(
    new THREE.SphereGeometry(0.38, 10, 8),
    receptionistSkin,
  );
  receptionistHand.position.set(1.97, 2.36, 0.05);
  const receptionistBadge = makePanel(
    THREE,
    "NOVA",
    "RECEPTIONIST",
    "#9ef7c6",
    4.6,
  );
  receptionistBadge.name = "forkmesh-sky-office-receptionist-badge";
  receptionistBadge.position.set(0, 7.45, 0.12);
  receptionist.add(
    receptionistBody,
    receptionistHead,
    receptionistHairCap,
    receptionistArm,
    receptionistHand,
    receptionistBadge,
  );
  setInteractive(
    receptionist,
    interactive,
    "forkmesh-sky-office-receptionist",
  );
  reception.add(receptionist);
  const receptionSign = makePanel(
    THREE,
    "RECEPTION",
    "WELCOME TO FORKMESH OFFICE",
    "#9ef7c6",
    20,
  );
  receptionSign.position.set(0, 9.15, -2.55);
  reception.add(receptionSign);
  lobby.add(reception);

  const aquariumConsole = new THREE.Group();
  aquariumConsole.name = "forkmesh-sky-office-aquarium-controls";
  aquariumConsole.position.set(52, 1.1, 18);
  aquariumConsole.rotation.y = -Math.PI / 2;
  const aquariumConsoleBody = new THREE.Mesh(
    new THREE.BoxGeometry(7.8, 3.8, 2),
    darkStructure,
  );
  aquariumConsoleBody.position.y = 1.9;
  aquariumConsole.add(aquariumConsoleBody);
  let aquariumLightControl = null;
  [
    ["FEED", "forkmesh-sky-office-aquarium-feed", -2.35, "#f7c96b"],
    ["TAP", "forkmesh-sky-office-aquarium-tap", 0, "#79efff"],
    ["LIGHT", "forkmesh-sky-office-aquarium-light", 2.35, "#9ef7c6"],
  ].forEach(([label, type, x, color]) => {
    const subtitle =
      type === "forkmesh-sky-office-aquarium-light"
        ? FORKMESH_SKY_OFFICE_LIGHT_SETTINGS[0].label
        : "AQUARIUM";
    const control = makePanel(THREE, label, subtitle, color, 2.05);
    control.position.set(x, 3.15, 1.05);
    control.userData.interactive = type;
    if (type === "forkmesh-sky-office-aquarium-light") {
      aquariumLightControl = control;
    }
    interactive.push(control);
    aquariumConsole.add(control);
  });
  lobby.add(aquariumConsole);

  const entranceDoors = new THREE.Group();
  entranceDoors.name = "forkmesh-sky-office-aquarium-entrance-doors";
  entranceDoors.position.z = FORKMESH_SKY_OFFICE_FRONT_Z + 0.05;
  const entranceDoorWidth = 4.9;
  const entranceDoorPanels = [-1, 1].map((side) => {
    const door = new THREE.Mesh(
      new THREE.BoxGeometry(entranceDoorWidth, 8, 0.2),
      aquariumGlass.clone(),
    );
    door.name = `forkmesh-sky-office-entrance-door-${side < 0 ? "left" : "right"}`;
    door.position.set(side * 2.5, 4.15, 0);
    entranceDoors.add(door);
    return door;
  });
  high.add(entranceDoors);
  const entranceArch = new THREE.Mesh(
    new THREE.TorusGeometry(6.4, 0.42, 10, 48, Math.PI),
    glow,
  );
  entranceArch.position.set(0, 8.1, FORKMESH_SKY_OFFICE_FRONT_Z + 0.22);
  entranceArch.rotation.z = Math.PI;
  high.add(entranceArch);
  const entrySign = makePanel(
    THREE,
    "FORKMESH OFFICE",
    "AQUARIUM ENTRY · ELEVATOR STRAIGHT AHEAD",
    "#79efff",
    28,
  );
  entrySign.position.set(0, 13.4, FORKMESH_SKY_OFFICE_FRONT_Z + 0.34);
  high.add(entrySign);

  // Minimal far silhouette: no fish, transparent layers, furniture, text
  // canvases, or interactive meshes.
  const lowBase = addCylinder(
    low,
    "forkmesh-sky-office-low-aquarium",
    radius,
    32,
    6,
    makeMaterial(THREE, "#167ea0", {
      emissive: "#0b526f",
      emissiveIntensity: 0.42,
      roughness: 0.5,
    }),
    16,
    false,
  );
  lowBase.userData.forkmeshSkyOfficeLowPoly = true;
  addCylinder(
    low,
    "forkmesh-sky-office-low-tube",
    5.7,
    FORKMESH_SKY_OFFICE_ROOF_Y,
    FORKMESH_SKY_OFFICE_ROOF_Y / 2,
    makeMaterial(THREE, "#4c8e99", { roughness: 0.44 }),
    6,
    false,
  ).position.z = FORKMESH_SKY_OFFICE_ELEVATOR_Z;
  addCylinder(
    low,
    "forkmesh-sky-office-low-summit",
    radius,
    18,
    FORKMESH_SKY_OFFICE_SUMMIT_Y + 9,
    makeMaterial(THREE, "#24566b", {
      emissive: "#10394b",
      emissiveIntensity: 0.4,
    }),
    6,
    false,
  );
  addCylinder(
    low,
    "forkmesh-sky-office-low-roof",
    radius + 3,
    2,
    FORKMESH_SKY_OFFICE_ROOF_Y + 1,
    darkStructure,
    6,
    false,
  );

  // Twin transparent tubes begin on the actual mainland and finish at the
  // remote island. Cars always wait at opposite ends, like bullets in their
  // magazines, so travel is available in either direction.
  const tubeLength = Math.abs(
    FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z -
      FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z,
  );
  const tubeCenterZ =
    (FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z +
      FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z) /
    2;
  const hyperloop = new THREE.Group();
  hyperloop.name = "forkmesh-sky-office-twin-hyperloop";
  // The express spans the whole route and can stay in the camera frustum from
  // Town. Keep it unlit and translucent so it never requests Three.js's
  // transmission pre-pass while it is only serving as a route silhouette.
  const hyperloopGlass = new THREE.MeshBasicMaterial({
    color: "#bff8ff",
    transparent: true,
    opacity: 0.25,
    side: THREE.DoubleSide,
    depthWrite: false,
    toneMapped: false,
  });
  const laneOffsets = [-3.35, 3.35];
  const hyperloopCollarFractions = Object.freeze([0, 0.004, 0.996, 1]);
  const collarGeometry = new THREE.TorusGeometry(3.1, 0.16, 3, 6);
  laneOffsets.forEach((offset, lane) => {
    const tubeMesh = new THREE.Mesh(
      new THREE.CylinderGeometry(3.05, 3.05, tubeLength, 6, 1, true),
      hyperloopGlass,
    );
    tubeMesh.name = `forkmesh-sky-office-hyperloop-tube-${lane + 1}`;
    tubeMesh.rotation.x = Math.PI / 2;
    tubeMesh.position.set(
      FORKMESH_SKY_OFFICE_CENTER[0] + offset,
      4.2,
      tubeCenterZ,
    );
    hyperloop.add(tubeMesh);
    const rail = new THREE.Mesh(
      new THREE.BoxGeometry(1.3, 0.24, tubeLength),
      darkStructure,
    );
    rail.position.set(
      FORKMESH_SKY_OFFICE_CENTER[0] + offset,
      1.35,
      tubeCenterZ,
    );
    hyperloop.add(rail);
    // Four six-sided instances replace twenty-six individually tessellated
    // collars. At this distance intermediate rings were sub-pixel noise; the
    // paired station collars retain the readable launch/arrival silhouette.
    const collars = new THREE.InstancedMesh(
      collarGeometry,
      glow,
      hyperloopCollarFractions.length,
    );
    collars.name = `forkmesh-sky-office-hyperloop-collars-${lane + 1}`;
    const collarTransform = new THREE.Object3D();
    hyperloopCollarFractions.forEach((fraction, index) => {
      collarTransform.position.set(
        FORKMESH_SKY_OFFICE_CENTER[0] + offset,
        4.2,
        THREE.MathUtils.lerp(
          FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z,
          FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z,
          fraction,
        ),
      );
      collarTransform.rotation.set(Math.PI / 2, 0, 0);
      collarTransform.updateMatrix();
      collars.setMatrixAt(index, collarTransform.matrix);
    });
    collars.instanceMatrix.needsUpdate = true;
    hyperloop.add(collars);
  });
  const serviceDeck = new THREE.Mesh(
    new THREE.BoxGeometry(
      FORKMESH_SKY_OFFICE_HYPERLOOP_WIDTH,
      0.3,
      tubeLength,
    ),
    makeMaterial(THREE, "#315e50", { roughness: 0.88 }),
  );
  serviceDeck.name = "forkmesh-sky-office-hyperloop-service-deck";
  serviceDeck.position.set(
    FORKMESH_SKY_OFFICE_CENTER[0],
    0.22,
    tubeCenterZ,
  );
  hyperloop.add(serviceDeck);
  group.add(hyperloop);

  const makeStation = (z, title, yaw) => {
    const station = new THREE.Group();
    station.name = `forkmesh-sky-office-hyperloop-station-${title.toLowerCase()}`;
    station.position.set(FORKMESH_SKY_OFFICE_CENTER[0], 0, z);
    const canopy = new THREE.Mesh(
      new THREE.BoxGeometry(18, 0.6, 10),
      darkStructure,
    );
    canopy.position.y = 8.2;
    station.add(canopy);
    const sign = makePanel(THREE, title, "TWIN-TUBE EXPRESS", "#79efff", 16);
    sign.position.set(0, 6.2, yaw ? 5.1 : -5.1);
    sign.rotation.y = yaw;
    station.add(sign);
    group.add(station);
    return station;
  };
  const hyperloopStations = [
    makeStation(FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z, "MAINLAND", 0),
    makeStation(
      FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z,
      "FORKMESH OFFICE",
      Math.PI,
    ),
  ];

  const hyperloopCars = [];
  const makeCar = (index, fromZ, toZ, destination) => {
    const car = new THREE.Group();
    car.name = `forkmesh-sky-office-bullet-car-${index + 1}`;
    const shell = new THREE.Mesh(
      new THREE.CapsuleGeometry(2.35, 6.8, 2, 6),
      makeMaterial(THREE, index ? "#8be8ff" : "#9ef7c6", {
        metalness: 0.72,
        roughness: 0.2,
        emissive: index ? "#155d73" : "#176143",
        emissiveIntensity: 0.52,
      }),
    );
    shell.rotation.x = Math.PI / 2;
    car.add(shell);
    const windowMesh = new THREE.Mesh(
      new THREE.CapsuleGeometry(1.55, 3.8, 2, 6),
      hyperloopGlass,
    );
    windowMesh.rotation.x = Math.PI / 2;
    car.add(windowMesh);
    car.position.set(
      FORKMESH_SKY_OFFICE_CENTER[0] + laneOffsets[index],
      4.05,
      fromZ,
    );
    setInteractive(car, interactive, "forkmesh-sky-office-hyperloop-car", {
      forkmeshSkyOfficeHyperloopCar: index,
    });
    group.add(car);
    const state = {
      car,
      fromZ,
      toZ,
      destination,
      available: true,
      resetAt: 0,
    };
    hyperloopCars.push(state);
    return state;
  };
  makeCar(
    0,
    FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z + 7,
    FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z - 7,
    "forkmesh-office",
  );
  makeCar(
    1,
    FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z - 7,
    FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z + 7,
    "mainland",
  );

  let access = normalizeForkmeshSkyOfficeAccess();
  let currentFloorId = "aquarium-lobby";
  let elevatorRide = null;
  let hyperloopRide = null;
  let inside = false;
  let automaticDispatchComplete = false;
  let entranceDoorOpen = false;
  let aquariumLightSettingIndex = 0;
  let aquariumLightSetting = FORKMESH_SKY_OFFICE_LIGHT_SETTINGS[
    aquariumLightSettingIndex
  ];
  let aquariumFeedStartedAt = -Infinity;
  let receptionistWaveUntil = -Infinity;
  let cubeSpinMultiplier = 1;
  let soundEmitter = () => {};
  let lastAquariumWaterAt = -Infinity;
  let lastAquariumBubblesAt = -Infinity;
  let lastAquariumFishAt = -Infinity;
  let lastCubeHumAt = -Infinity;
  let lastStationAt = -Infinity;
  let lastSummitAt = -Infinity;
  let lastRoofAt = -Infinity;
  let lastFootstepAt = -Infinity;
  let lastElevatorMotorAt = -Infinity;
  let lastWhooshAt = -Infinity;
  let lastCollar = -1;
  let populated = false;
  let previousPlayerPositionReady = false;
  const previousPlayerPosition = new THREE.Vector3();
  const aquariumRgbColor = new THREE.Color();

  const applyAquariumLightColor = (color, setting) => {
    aquariumGlow.color.copy(color);
    aquariumGlow.emissive.copy(color);
    aquariumGlow.emissiveIntensity = setting.id === "moonlight" ? 0.74 : 1.22;
    aquariumLightRig.color.copy(color);
    aquariumLightRig.intensity = setting.intensity;
    aquariumLightRings.forEach((ring) => {
      ring.userData.forkmeshSkyOfficeLightSetting = setting.id;
    });
  };

  const applyAquariumLightSetting = (setting) => {
    aquariumWater.color.set(setting.waterColor);
    aquariumWater.emissive.set(setting.waterEmissive);
    aquariumWater.emissiveIntensity = setting.waterIntensity;
    applyAquariumLightColor(new THREE.Color(setting.color), setting);
    aquariumLightControl?.userData?.setPanelText?.(
      "LIGHT",
      setting.label,
      setting.color,
    );
  };
  applyAquariumLightSetting(aquariumLightSetting);

  const emit = (stage, detail = {}) => {
    if (!FORKMESH_SKY_OFFICE_SOUND_EVENTS.includes(stage)) return false;
    soundEmitter(stage, detail);
    return true;
  };

  function setAccess(payload = {}) {
    access = normalizeForkmeshSkyOfficeAccess(payload);
    elevatorButtons.forEach((button) => {
      const allowed = canAccessForkmeshSkyOfficeFloor(
        access,
        button.userData.forkmeshSkyOfficeFloorId,
      );
      button.userData.forkmeshSkyOfficeAllowed = allowed;
      button.material.color.set(allowed ? "#2d8063" : "#4c4145");
      button.material.emissive?.set?.(allowed ? "#1f9c6a" : "#6e2737");
      button.material.emissiveIntensity = allowed ? 0.9 : 0.22;
    });
    return { ...access, allowedFloorIds: [...access.allowedFloorIds] };
  }

  function setSoundEmitter(handler) {
    soundEmitter = typeof handler === "function" ? handler : () => {};
  }

  function floorElevation(floorId) {
    return forkmeshSkyOfficeFloorById(floorId)?.elevation || 0;
  }

  function playerLocal(player, target = new THREE.Vector3()) {
    target.copy(player.position);
    target.x -= FORKMESH_SKY_OFFICE_CENTER[0];
    target.y -= FORKMESH_SKY_OFFICE_CENTER[1];
    target.z -= FORKMESH_SKY_OFFICE_CENTER[2];
    return target;
  }

  function playerInCabin(player) {
    const local = playerLocal(player);
    return (
      Math.abs(local.x) <= 4.25 &&
      Math.abs(local.z - FORKMESH_SKY_OFFICE_ELEVATOR_Z) <= 4.1
    );
  }

  function beginElevator(floorId, player, setFloorY = () => {}) {
    const floor = forkmeshSkyOfficeFloorById(floorId);
    const allowed = canAccessForkmeshSkyOfficeFloor(access, floor?.id);
    emit("sky-office-elevator-button", {
      floor: floor?.id || "",
      allowed,
    });
    if (!floor || !allowed || elevatorRide || hyperloopRide) {
      if (floor && !allowed) {
        emit("sky-office-elevator-denied", { floor: floor.id });
      }
      return false;
    }
    if (floor.id === currentFloorId) {
      emit("sky-office-elevator-door-open", { floor: floor.id });
      return true;
    }
    const fromY = player.position.y;
    const toY = floor.elevation + 0.38;
    const distance = Math.abs(toY - fromY);
    const duration = Math.min(8000, Math.max(1800, 1800 + distance * 18));
    elevatorRide = {
      floorId: floor.id,
      fromY,
      toY,
      startedAt: performance.now(),
      duration,
      started: false,
      braking: false,
    };
    lastCollar = Math.floor(Math.min(fromY, toY) / 32);
    emit("sky-office-elevator-door-close", {
      from: currentFloorId,
      to: floor.id,
    });
    setFloorY(fromY);
    return true;
  }

  function beginHyperloop(index, player) {
    if (hyperloopRide || elevatorRide) return false;
    const state = hyperloopCars[index];
    if (!state?.available) return false;
    const distance = Math.hypot(
      state.car.position.x - player.position.x,
      state.car.position.z - player.position.z,
    );
    if (distance > 10) return false;
    state.available = false;
    hyperloopRide = {
      index,
      startedAt: performance.now(),
      sealed: false,
      charged: false,
      launched: false,
      braking: false,
    };
    player.position.set(state.car.position.x, 4.05, state.car.position.z);
    player.rotation.y = state.toZ < state.fromZ ? 0 : Math.PI;
    emit("sky-office-hyperloop-load", {
      destination: state.destination,
      duration: 3000,
    });
    return true;
  }

  function interact(
    object,
    { player, setFloorY = () => {}, setSpace = () => {} } = {},
  ) {
    if (!object || !player) return false;
    const type = String(object.userData?.interactive || "");
    if (!type.startsWith("forkmesh-sky-office-")) return false;
    if (type === "forkmesh-sky-office-hyperloop-car") {
      return beginHyperloop(
        Number(object.userData.forkmeshSkyOfficeHyperloopCar),
        player,
      );
    }
    if (type === "forkmesh-sky-office-elevator-button") {
      return beginElevator(
        object.userData.forkmeshSkyOfficeFloorId,
        player,
        setFloorY,
      );
    }
    if (type === "forkmesh-sky-office-aquarium-feed") {
      const startedAt = performance.now();
      aquariumFeedStartedAt = startedAt;
      fishFood.visible = true;
      fishFood.userData.forkmeshSkyOfficeFeedUntil = startedAt + 9000;
      fishStates.forEach((state) => {
        state.feedBoostUntil = startedAt + 9000;
        state.speed = state.baseSpeed * 1.72;
      });
      emit("sky-office-aquarium-feed", {
        visibleFood: true,
        pelletCount: fishFoodStates.length,
      });
      return true;
    }
    if (type === "forkmesh-sky-office-aquarium-tap") {
      fishStates.forEach((state) => {
        state.direction *= -1;
      });
      emit("sky-office-aquarium-tap", {});
      return true;
    }
    if (type === "forkmesh-sky-office-aquarium-light") {
      aquariumLightSettingIndex =
        (aquariumLightSettingIndex + 1) %
        FORKMESH_SKY_OFFICE_LIGHT_SETTINGS.length;
      aquariumLightSetting =
        FORKMESH_SKY_OFFICE_LIGHT_SETTINGS[aquariumLightSettingIndex];
      applyAquariumLightSetting(aquariumLightSetting);
      emit("sky-office-aquarium-light", {
        enabled: true,
        setting: aquariumLightSetting.id,
      });
      return true;
    }
    if (type === "forkmesh-sky-office-cube") {
      cubeSpinMultiplier = cubeSpinMultiplier === 1 ? 2.5 : 1;
      emit("sky-office-cube-touch", { speed: cubeSpinMultiplier });
      return true;
    }
    if (
      type === "forkmesh-sky-office-reception" ||
      type === "forkmesh-sky-office-receptionist"
    ) {
      receptionistWaveUntil = performance.now() + 3200;
      emit("sky-office-reception-chime", {});
      return true;
    }
    if (
      type === "forkmesh-sky-office-couch" ||
      type === "forkmesh-sky-office-roof-seat"
    ) {
      emit("sky-office-lounge-seat", { floor: currentFloorId });
      return true;
    }
    if (type === "forkmesh-sky-office-widget-copy") {
      emit("sky-office-widget", {
        widget: String(object.userData.forkmeshSkyOfficeWidget || "widget"),
      });
      return true;
    }
    if (type === "forkmesh-sky-office-drone-pad") {
      emit("sky-office-drone-pad", {});
      return true;
    }
    setSpace(`pkg-forkmesh-sky-office-${currentFloorId}`);
    return false;
  }

  function interactNearest(player) {
    if (!player || hyperloopRide || elevatorRide) return false;
    let nearest = -1;
    let distance = 10;
    hyperloopCars.forEach((state, index) => {
      if (!state.available) return;
      const next = Math.hypot(
        state.car.position.x - player.position.x,
        state.car.position.z - player.position.z,
      );
      if (next <= distance) {
        nearest = index;
        distance = next;
      }
    });
    return nearest >= 0 ? beginHyperloop(nearest, player) : false;
  }

  function constrainPlayer(previous, player) {
    if (!inside || elevatorRide || hyperloopRide || !player) return false;
    const local = playerLocal(player);
    const limit =
      currentFloorId === "drone-roof" ? radius + 0.5 : innerRadius - 0.8;
    const distance = Math.hypot(local.x, local.z);
    const doorway =
      currentFloorId === "aquarium-lobby" &&
      Math.abs(local.x) <= 5.1 &&
      local.z >= innerRadius - 1 &&
      local.z <= FORKMESH_SKY_OFFICE_FRONT_Z + 3;
    if (distance <= limit || doorway) return false;
    player.position.x = previous.x;
    player.position.z = previous.z;
    return true;
  }

  function update(
    {
      time = performance.now(),
      delta = 0.016,
      player,
      setFloorY = () => {},
      setSpace = () => {},
    } = {},
  ) {
    if (!player) return { riding: false, inside, floorId: currentFloorId };
    const horizontalMovement = previousPlayerPositionReady
      ? Math.hypot(
          player.position.x - previousPlayerPosition.x,
          player.position.z - previousPlayerPosition.z,
        )
      : 0;
    previousPlayerPosition.copy(player.position);
    previousPlayerPositionReady = true;
    const local = playerLocal(player);
    const horizontalDistance = Math.hypot(local.x, local.z);
    const entranceNear =
      Math.abs(local.x) <= 9 &&
      Math.abs(local.z - FORKMESH_SKY_OFFICE_FRONT_Z) <= 9 &&
      player.position.y < 10;
    if (entranceNear !== entranceDoorOpen) {
      entranceDoorOpen = entranceNear;
      emit(
        entranceDoorOpen
          ? "sky-office-door-open"
          : "sky-office-door-close",
        {},
      );
    }
    const doorOpenAmount = entranceDoorOpen ? 1 : 0;
    entranceDoorPanels[0].position.x = -2.5 - doorOpenAmount * 3;
    entranceDoorPanels[1].position.x = 2.5 + doorOpenAmount * 3;

    const nowInside =
      player.position.y < 10 &&
      horizontalDistance <= innerRadius - 0.5;
    if (nowInside && !inside) {
      inside = true;
      currentFloorId = "aquarium-lobby";
      automaticDispatchComplete = false;
      setFloorY(0.38);
      setSpace("pkg-forkmesh-sky-office-aquarium-lobby");
    } else if (
      inside &&
      currentFloorId === "aquarium-lobby" &&
      !elevatorRide &&
      horizontalDistance > radius + 3
    ) {
      inside = false;
      automaticDispatchComplete = false;
      setFloorY(0.38);
      setSpace("town-square");
    }

    if (
      inside &&
      !automaticDispatchComplete &&
      !elevatorRide &&
      playerInCabin(player)
    ) {
      automaticDispatchComplete = true;
      const destination = access.organizationRole
        ? "summit-office"
        : "glass-lounge";
      beginElevator(destination, player, setFloorY);
    }

    if (elevatorRide) {
      const ride = elevatorRide;
      const elapsed = Math.max(0, time - ride.startedAt);
      const launchDelay = 620;
      if (elapsed >= 180 && !ride.started) {
        ride.started = true;
        emit("sky-office-elevator-start", {
          from: currentFloorId,
          to: ride.floorId,
          duration: ride.duration,
        });
      }
      const progress = Math.max(
        0,
        Math.min(1, (elapsed - launchDelay) / Math.max(1, ride.duration)),
      );
      const eased =
        progress < 0.5
          ? 4 * progress ** 3
          : 1 - (-2 * progress + 2) ** 3 / 2;
      const y = ride.fromY + (ride.toY - ride.fromY) * eased;
      player.position.set(
        FORKMESH_SKY_OFFICE_CENTER[0],
        y,
        FORKMESH_SKY_OFFICE_CENTER[2] + FORKMESH_SKY_OFFICE_ELEVATOR_Z,
      );
      elevatorCar.position.y = y - 0.38;
      setFloorY(y);
      const doorsClosed = Math.min(1, elapsed / 450);
      elevatorDoors[0].position.x = -2.32 + doorsClosed * 2.05;
      elevatorDoors[1].position.x = 2.32 - doorsClosed * 2.05;
      if (progress > 0 && time - lastElevatorMotorAt >= 1500) {
        lastElevatorMotorAt = time;
        emit("sky-office-elevator-motor", {
          progress,
          direction: ride.toY >= ride.fromY ? "up" : "down",
        });
      }
      const collar = Math.floor(y / 32);
      if (progress > 0 && progress < 1 && collar !== lastCollar) {
        lastCollar = collar;
        emit("sky-office-elevator-collar", { collar, elevation: y });
      }
      if (progress >= 0.86 && !ride.braking) {
        ride.braking = true;
        emit("sky-office-elevator-brake", { floor: ride.floorId });
      }
      if (progress >= 1) {
        currentFloorId = ride.floorId;
        setFloorY(ride.toY);
        setSpace(`pkg-forkmesh-sky-office-${ride.floorId}`);
        elevatorCar.position.y = floorElevation(ride.floorId);
        elevatorDoors[0].position.x = -2.32;
        elevatorDoors[1].position.x = 2.32;
        elevatorRide = null;
        emit("sky-office-elevator-arrive", { floor: currentFloorId });
        emit("sky-office-elevator-door-open", { floor: currentFloorId });
      }
    }

    if (hyperloopRide) {
      const ride = hyperloopRide;
      const state = hyperloopCars[ride.index];
      const elapsed = Math.max(0, time - ride.startedAt);
      if (elapsed >= 220 && !ride.sealed) {
        ride.sealed = true;
        emit("sky-office-hyperloop-seal", { destination: state.destination });
      }
      if (elapsed >= 520 && !ride.charged) {
        ride.charged = true;
        emit("sky-office-hyperloop-charge", { destination: state.destination });
      }
      const loadMs = 780;
      const travelMs = 3000;
      if (elapsed < loadMs) {
        state.car.position.z = state.fromZ + Math.sin(elapsed * 0.025) * 0.1;
      } else {
        if (!ride.launched) {
          ride.launched = true;
          emit("sky-office-hyperloop-launch", {
            destination: state.destination,
            duration: travelMs,
          });
        }
        const progress = Math.max(
          0,
          Math.min(1, (elapsed - loadMs) / travelMs),
        );
        const eased =
          progress < 0.5
            ? 2 * progress * progress
            : 1 - ((-2 * progress + 2) ** 2) / 2;
        state.car.position.z = THREE.MathUtils.lerp(state.fromZ, state.toZ, eased);
        if (progress > 0.08 && progress < 0.9 && time - lastWhooshAt >= 620) {
          lastWhooshAt = time;
          emit("sky-office-hyperloop-whoosh", {
            destination: state.destination,
            progress,
          });
        }
        if (progress >= 0.82 && !ride.braking) {
          ride.braking = true;
          emit("sky-office-hyperloop-brake", { destination: state.destination });
        }
        if (progress >= 1) {
          hyperloopRide = null;
          state.resetAt = time + 1600;
          player.position.set(
            state.car.position.x,
            0.38,
            state.car.position.z + (state.destination === "forkmesh-office" ? 12 : -12),
          );
          player.rotation.y = state.destination === "forkmesh-office" ? 0 : Math.PI;
          setFloorY(0.38);
          setSpace(
            state.destination === "forkmesh-office"
              ? "pkg-forkmesh-sky-office-campus"
              : "town-square",
          );
          emit("sky-office-hyperloop-arrive", { destination: state.destination });
          emit("sky-office-hyperloop-unseal", { destination: state.destination });
        }
      }
      if (hyperloopRide) {
        player.position.set(state.car.position.x, 4.05, state.car.position.z);
        player.rotation.y = state.toZ < state.fromZ ? 0 : Math.PI;
      }
    }

    hyperloopCars.forEach((state) => {
      if (!state.resetAt || time < state.resetAt) return;
      state.resetAt = 0;
      state.available = true;
      state.car.position.z = state.fromZ;
    });

    const aquariumNear =
      horizontalDistance <= radius + 16 && player.position.y < 24;
    if (aquariumNear && time - lastAquariumWaterAt >= 6500) {
      lastAquariumWaterAt = time;
      emit("sky-office-aquarium-water", {});
    }
    if (aquariumNear && time - lastAquariumBubblesAt >= 4100) {
      lastAquariumBubblesAt = time;
      emit("sky-office-aquarium-bubbles", {});
    }
    if (aquariumNear && time - lastAquariumFishAt >= 8200) {
      lastAquariumFishAt = time;
      emit("sky-office-aquarium-fish", {});
    }
    const cubeDistance = Math.hypot(local.x, local.z - 28);
    if (
      inside &&
      currentFloorId === "aquarium-lobby" &&
      cubeDistance < 18 &&
      time - lastCubeHumAt >= 6000
    ) {
      lastCubeHumAt = time;
      emit("sky-office-cube-hum", {});
    }
    const stationDistance = Math.min(
      Math.abs(player.position.z - FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z),
      Math.abs(player.position.z - FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z),
    );
    if (
      Math.abs(player.position.x - FORKMESH_SKY_OFFICE_CENTER[0]) < 15 &&
      stationDistance < 18 &&
      time - lastStationAt >= 8000
    ) {
      lastStationAt = time;
      emit("sky-office-hyperloop-station", {});
    }
    if (inside && currentFloorId === "summit-office" && time - lastSummitAt >= 7000) {
      lastSummitAt = time;
      emit("sky-office-summit-electronics", {});
    }
    if (inside && currentFloorId === "drone-roof" && time - lastRoofAt >= 6500) {
      lastRoofAt = time;
      emit("sky-office-roof-wind", {});
    }
    const walkingFloorY = floorElevation(currentFloorId) + 0.38;
    if (
      inside &&
      !elevatorRide &&
      !hyperloopRide &&
      horizontalMovement >= 0.025 &&
      horizontalMovement <= 2.5 &&
      Math.abs(player.position.y - walkingFloorY) <= 0.55 &&
      time - lastFootstepAt >= 340
    ) {
      lastFootstepAt = time;
      emit("sky-office-footstep", { floor: currentFloorId });
    }

    return {
      riding: Boolean(elevatorRide || hyperloopRide),
      inside,
      floorId: currentFloorId,
    };
  }

  function populateFromExisting(existingRoot) {
    if (populated || !existingRoot) return 0;
    populated = true;
    existingRoot.updateMatrixWorld?.(true);
    const roots = interactiveSourceRoots(existingRoot);
    const columns = [-48, -16, 16, 48];
    const rows = [-56, -37, -18, 1, 20, 39, 58];
    roots.forEach((source, index) => {
      const copy = source.clone(true);
      copy.name = `${source.name || "office-widget"}-summit-copy`;
      copy.traverse((child) => {
        if (child.isLight || child.isCamera) child.visible = false;
        if (!child.isMesh) return;
        child.userData = {
          ...child.userData,
          interactive: "forkmesh-sky-office-widget-copy",
          forkmeshSkyOfficeWidget: source.name || "Office widget",
          forkmeshSkyOfficeCopy: true,
          officeFloorId: "",
        };
        interactive.push(child);
      });
      const wrapper = new THREE.Group();
      wrapper.name = `forkmesh-sky-office-organized-widget-${index + 1}`;
      wrapper.add(copy);
      wrapper.updateMatrixWorld(true);
      const box = new THREE.Box3().setFromObject(copy);
      const center = box.getCenter(new THREE.Vector3());
      const size = box.getSize(new THREE.Vector3());
      copy.position.sub(center);
      const scale = Math.min(
        1,
        13 / Math.max(0.1, size.x),
        8 / Math.max(0.1, size.y),
        8 / Math.max(0.1, size.z),
      );
      wrapper.scale.setScalar(scale);
      wrapper.position.set(
        columns[index % columns.length],
        1 + (size.y * scale) / 2,
        rows[Math.floor(index / columns.length) % rows.length],
      );
      wrapper.rotation.y = Math.atan2(-wrapper.position.x, -wrapper.position.z);
      summitContent.add(wrapper);
    });
    group.userData.forkmeshSkyOfficeCopiedWidgetCount = roots.length;
    return roots.length;
  }

  animated.push((time) => {
    if (!high.visible) return;
    const feedElapsed = time - aquariumFeedStartedAt;
    const foodActive = feedElapsed >= 0 && feedElapsed < 9000;
    fishFood.visible = foodActive;
    if (foodActive) {
      const fallProgress = Math.min(1, feedElapsed / 3600);
      fishFoodStates.forEach((state, index) => {
        const driftAngle =
          fishFoodAngle +
          state.angleOffset +
          Math.sin(time * 0.0013 + state.phase) * state.drift * 0.018;
        fishFoodDummy.position.set(
          Math.sin(driftAngle) * (fishFoodRadius + Math.sin(index) * 0.26),
          fishFoodOrigin.y + state.yOffset - fallProgress * (10.4 + (index % 4)),
          Math.cos(driftAngle) * (fishFoodRadius + Math.sin(index) * 0.26),
        );
        fishFoodDummy.scale.setScalar(
          0.76 + (index % 3) * 0.17 + Math.sin(time * 0.004 + index) * 0.08,
        );
        fishFoodDummy.updateMatrix();
        fishFoodPellets.setMatrixAt(index, fishFoodDummy.matrix);
      });
      fishFoodPellets.instanceMatrix.needsUpdate = true;
      const pulse = 1 + Math.sin(time * 0.006) * 0.24;
      fishFoodPulse.scale.setScalar(pulse);
      fishFoodPulse.rotation.z = time * 0.00048;
      fishFoodPulse.material.emissiveIntensity = 0.86 + pulse * 0.46;
    }

    if (aquariumLightSetting.id === "rgb-slow-fade") {
      // One complete RGB sweep takes just under a minute; it is deliberately
      // calm enough for an office, rather than a strobing arcade effect.
      aquariumRgbColor.setHSL(
        THREE.MathUtils.euclideanModulo(time * 0.000018, 1),
        0.78,
        0.62,
      );
      aquariumWater.color.copy(aquariumRgbColor);
      aquariumWater.emissive.copy(aquariumRgbColor).multiplyScalar(0.36);
      aquariumWater.emissiveIntensity = aquariumLightSetting.waterIntensity;
      applyAquariumLightColor(aquariumRgbColor, aquariumLightSetting);
    }

    fishStates.forEach((state, index) => {
      state.speed =
        time < state.feedBoostUntil ? state.baseSpeed * 1.72 : state.baseSpeed;
      const rawAngle = state.angle + time * state.speed * state.direction;
      const angle =
        doorwayHalfAngle +
        THREE.MathUtils.euclideanModulo(rawAngle - doorwayHalfAngle, wallArc);
      writeFishPose(
        state,
        index,
        angle,
        Math.sin(time * 0.0013 + index) * 0.34,
      );
    });
    fish.instanceMatrix.needsUpdate = true;
    fishTail.instanceMatrix.needsUpdate = true;
    fishFins.instanceMatrix.needsUpdate = true;
    cubeMount.rotation.y = time * 0.00022 * cubeSpinMultiplier;
    cubeHalo.rotation.z = -time * 0.00014;
    dronePad.material.emissiveIntensity = 0.5 + Math.sin(time * 0.004) * 0.22;
    const greeting =
      time < receptionistWaveUntil ? Math.sin(time * 0.014) * 0.72 : 0;
    receptionistHead.rotation.y = Math.sin(time * 0.0011) * 0.12 + greeting * 0.18;
    receptionistArm.rotation.z = -0.3 - greeting;
    receptionistHand.position.y = 2.36 - Math.sin(time * 0.014) * 0.16;
  });

  group.traverse((child) => {
    if (!child.isMesh) return;
    // The destination already has emissive edge lighting, while submitting
    // every furnishing, fish, control, and copied widget to the sun's shadow
    // pass added another hundred-plus draws on each shadow refresh. Keep only
    // the broad walking surfaces as receivers and leave this remote district
    // out of the global caster list.
    const broadSurface = /(island|slab|service-deck|landing-pad|floor)/.test(
      String(child.name || "").toLowerCase(),
    );
    child.castShadow = false;
    child.receiveShadow =
      broadSurface && child.material?.transparent !== true;
    child.userData.forkmeshSkyOffice = true;
  });

  const distantTriangleCount = [
    low,
    hyperloop,
    ...hyperloopStations,
    ...hyperloopCars.map((state) => state.car),
  ].reduce((total, root) => total + triangleCount(root), 0);
  group.userData.forkmeshSkyOfficeDistantTriangleCount =
    distantTriangleCount;
  group.userData.forkmeshSkyOfficeDistantTriangleBudget =
    FORKMESH_SKY_OFFICE_DISTANT_TRIANGLE_BUDGET;

  return {
    group,
    floorGroups,
    hyperloopCars,
    setAccess,
    setSoundEmitter,
    populateFromExisting,
    interact,
    interactNearest,
    constrainPlayer,
    update,
    isRiding: () => Boolean(elevatorRide || hyperloopRide),
    isInside: () => inside,
    currentFloorId: () => currentFloorId,
    access: () => ({ ...access, allowedFloorIds: [...access.allowedFloorIds] }),
  };
}

// Build only what Town needs to advertise and enter the remote destination.
// The furnished tower used to allocate more than a hundred meshes, nine
// canvases, and all copied Office widgets inside createWorldScene(), even for
// visitors who never approached its station. The shell keeps the horizon mark
// and one working express car resident; explicit travel materializes the full
// destination once.
function createForkmeshSkyOfficeShell(THREE, interactive) {
  const group = new THREE.Group();
  group.name = "forkmesh-sky-office-destination";
  group.userData.landmark = "forkmesh-sky-office";
  group.userData.forkmeshSkyOfficeDeferred = true;
  group.userData.forkmeshSkyOfficeLoaded = false;

  const campus = new THREE.Group();
  campus.name = "forkmesh-sky-office-campus";
  campus.position.set(...FORKMESH_SKY_OFFICE_CENTER);
  group.add(campus);

  const high = new THREE.Group();
  high.name = "forkmesh-sky-office-high-fidelity";
  const low = new THREE.Group();
  low.name = "forkmesh-sky-office-low-poly";
  const lod = new THREE.LOD();
  lod.name = "forkmesh-sky-office-distance-lod";
  lod.addLevel(high, 0, 0.08);
  lod.addLevel(low, 260, 0.12);
  campus.add(lod);
  group.userData.forkmeshSkyOfficeLod = lod;

  const islandMaterial = new THREE.MeshBasicMaterial({ color: "#356d58" });
  const towerMaterial = new THREE.MeshBasicMaterial({ color: "#167ea0" });
  const structureMaterial = new THREE.MeshBasicMaterial({ color: "#173d35" });
  const glowMaterial = new THREE.MeshBasicMaterial({
    color: "#80f5ff",
    toneMapped: false,
  });
  const addCylinder = (name, radius, height, y, material) => {
    const mesh = new THREE.Mesh(
      new THREE.CylinderGeometry(radius, radius, height, 6),
      material,
    );
    mesh.name = name;
    mesh.position.y = y;
    low.add(mesh);
    return mesh;
  };
  addCylinder(
    "forkmesh-sky-office-low-island",
    FORKMESH_SKY_OFFICE_ISLAND_RADIUS,
    1.4,
    -0.7,
    islandMaterial,
  );
  addCylinder(
    "forkmesh-sky-office-low-aquarium",
    FORKMESH_SKY_OFFICE_RADIUS,
    32,
    6,
    towerMaterial,
  );
  const lift = addCylinder(
    "forkmesh-sky-office-low-tube",
    5.7,
    FORKMESH_SKY_OFFICE_ROOF_Y,
    FORKMESH_SKY_OFFICE_ROOF_Y / 2,
    structureMaterial,
  );
  lift.position.z = FORKMESH_SKY_OFFICE_ELEVATOR_Z;
  addCylinder(
    "forkmesh-sky-office-low-summit",
    FORKMESH_SKY_OFFICE_RADIUS,
    18,
    FORKMESH_SKY_OFFICE_SUMMIT_Y + 9,
    towerMaterial,
  );
  addCylinder(
    "forkmesh-sky-office-low-roof",
    FORKMESH_SKY_OFFICE_RADIUS + 3,
    2,
    FORKMESH_SKY_OFFICE_ROOF_Y + 1,
    structureMaterial,
  );

  const route = new THREE.Group();
  route.name = "forkmesh-sky-office-twin-hyperloop";
  [-3.35, 3.35].forEach((offset, index) => {
    const guide = new THREE.Line(
      new THREE.BufferGeometry().setFromPoints([
        new THREE.Vector3(
          FORKMESH_SKY_OFFICE_CENTER[0] + offset,
          4.2,
          FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z,
        ),
        new THREE.Vector3(
          FORKMESH_SKY_OFFICE_CENTER[0] + offset,
          4.2,
          FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z,
        ),
      ]),
      new THREE.LineBasicMaterial({
        color: index ? "#79dfff" : "#9ef7c6",
        transparent: true,
        opacity: 0.52,
        toneMapped: false,
      }),
    );
    guide.name = `forkmesh-sky-office-hyperloop-guide-${index + 1}`;
    route.add(guide);
  });
  group.add(route);

  const station = new THREE.Group();
  station.name = "forkmesh-sky-office-hyperloop-station-mainland";
  station.position.set(
    FORKMESH_SKY_OFFICE_CENTER[0],
    0,
    FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z,
  );
  const platform = new THREE.Mesh(
    new THREE.BoxGeometry(18, 0.5, 12),
    structureMaterial,
  );
  platform.position.y = 0.25;
  const canopy = new THREE.Mesh(
    new THREE.BoxGeometry(18, 0.5, 10),
    structureMaterial,
  );
  canopy.position.y = 8.2;
  const stationMark = new THREE.Mesh(
    new THREE.BoxGeometry(13, 2.2, 0.18),
    glowMaterial,
  );
  stationMark.position.set(0, 6.1, -5.05);
  station.add(platform, canopy, stationMark);
  group.add(station);

  const car = new THREE.Mesh(
    new THREE.CapsuleGeometry(2.35, 6.8, 2, 6),
    new THREE.MeshStandardMaterial({
      color: "#9ef7c6",
      emissive: "#176143",
      emissiveIntensity: 0.52,
      metalness: 0.62,
      roughness: 0.28,
    }),
  );
  car.name = "forkmesh-sky-office-bullet-car-1";
  car.rotation.x = Math.PI / 2;
  car.position.set(
    FORKMESH_SKY_OFFICE_CENTER[0] - 3.35,
    4.05,
    FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z + 7,
  );
  car.userData.interactive = "forkmesh-sky-office-hyperloop-car";
  car.userData.forkmeshSkyOfficeHyperloopCar = 0;
  car.userData.forkmeshSkyOffice = true;
  interactive.push(car);
  group.add(car);

  group.traverse((child) => {
    if (!child.isMesh) return;
    child.castShadow = false;
    child.receiveShadow = false;
    child.userData.forkmeshSkyOffice = true;
  });
  group.userData.forkmeshSkyOfficeDistantTriangleCount = triangleCount(group);
  group.userData.forkmeshSkyOfficeDistantTriangleBudget =
    FORKMESH_SKY_OFFICE_DISTANT_TRIANGLE_BUDGET;

  return {
    group,
    floorGroups: new Map(),
    hyperloopCars: [
      {
        car,
        fromZ: FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z + 7,
        toZ: FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z - 7,
        destination: "forkmesh-office",
        available: true,
      },
    ],
    interactive: [car],
  };
}

function disposeForkmeshSkyOfficeShell(shell, interactive) {
  shell.interactive.forEach((object) => {
    let index = interactive.indexOf(object);
    while (index >= 0) {
      interactive.splice(index, 1);
      index = interactive.indexOf(object);
    }
  });
  const geometries = new Set();
  const materials = new Set();
  shell.group.traverse((child) => {
    if (child.geometry) geometries.add(child.geometry);
    const nextMaterials = Array.isArray(child.material)
      ? child.material
      : [child.material];
    nextMaterials.filter(Boolean).forEach((material) => materials.add(material));
  });
  geometries.forEach((geometry) => geometry.dispose());
  materials.forEach((material) => material.dispose());
  shell.group.clear();
}

export function createForkmeshSkyOffice(
  THREE,
  { interactive = [], animated = [] } = {},
) {
  const shell = createForkmeshSkyOfficeShell(THREE, interactive);
  const group = shell.group;
  let full = null;
  let sourceOffice = null;
  let populationScheduled = false;
  let cancelPopulation = () => {};
  let disposed = false;
  let accessState = normalizeForkmeshSkyOfficeAccess();
  let soundEmitter = () => {};

  const populateWhenIdle = () => {
    if (!full || !sourceOffice || populationScheduled) return;
    populationScheduled = true;
    const populate = () => {
      populationScheduled = false;
      cancelPopulation = () => {};
      if (disposed) return;
      if (!full || !sourceOffice) return;
      full.populateFromExisting(sourceOffice);
    };
    if (typeof globalThis.requestIdleCallback === "function") {
      const handle = globalThis.requestIdleCallback(populate, {
        timeout: 1500,
      });
      cancelPopulation = () => globalThis.cancelIdleCallback?.(handle);
    } else {
      const handle = globalThis.setTimeout(populate, 0);
      cancelPopulation = () => globalThis.clearTimeout(handle);
    }
  };

  const ensureFull = () => {
    if (full) return full;
    const next = createForkmeshSkyOfficeFull(THREE, {
      interactive,
      animated,
    });
    next.setAccess(accessState);
    next.setSoundEmitter(soundEmitter);

    // Only discard the working entry shell after the full destination exists.
    // If construction ever throws, Town retains a usable station instead of
    // leaving an empty landmark behind.
    disposeForkmeshSkyOfficeShell(shell, interactive);
    group.add(next.group);
    group.userData.forkmeshSkyOfficeDeferred = false;
    group.userData.forkmeshSkyOfficeLoaded = true;
    group.userData.forkmeshSkyOfficeLod =
      next.group.userData.forkmeshSkyOfficeLod;
    group.userData.forkmeshSkyOfficeDistantTriangleCount =
      next.group.userData.forkmeshSkyOfficeDistantTriangleCount;
    group.userData.forkmeshSkyOfficeDistantTriangleBudget =
      next.group.userData.forkmeshSkyOfficeDistantTriangleBudget;
    full = next;
    populateWhenIdle();
    return full;
  };

  const setAccess = (payload = {}) => {
    accessState = normalizeForkmeshSkyOfficeAccess(payload);
    return full
      ? full.setAccess(accessState)
      : { ...accessState, allowedFloorIds: [...accessState.allowedFloorIds] };
  };

  const setSoundEmitter = (handler) => {
    soundEmitter = typeof handler === "function" ? handler : () => {};
    full?.setSoundEmitter(soundEmitter);
  };

  const populateFromExisting = (existingRoot) => {
    sourceOffice = existingRoot || null;
    if (!full) return 0;
    cancelPopulation();
    populationScheduled = false;
    return full.populateFromExisting(sourceOffice);
  };

  const interact = (object, options = {}) => {
    const type = String(object?.userData?.interactive || "");
    if (!type.startsWith("forkmesh-sky-office-")) return false;
    return ensureFull().interact(object, options);
  };

  const interactNearest = (player) => {
    if (full) return full.interactNearest(player);
    if (!player) return false;
    const distance = Math.hypot(
      shell.hyperloopCars[0].car.position.x - player.position.x,
      shell.hyperloopCars[0].car.position.z - player.position.z,
    );
    if (distance > 10) return false;
    return ensureFull().interactNearest(player);
  };

  const update = (options = {}) => {
    if (!full) {
      const player = options?.player;
      const remoteDistance = player
        ? Math.hypot(
            player.position.x - FORKMESH_SKY_OFFICE_CENTER[0],
            player.position.z - FORKMESH_SKY_OFFICE_CENTER[2],
          )
        : Infinity;
      // Restore a saved destination directly, but leave ordinary Town frames
      // on the tiny shell until the visitor explicitly boards the express.
      if (remoteDistance <= FORKMESH_SKY_OFFICE_ISLAND_RADIUS + 160) {
        ensureFull();
      }
    }
    return full
      ? full.update(options)
      : { riding: false, inside: false, floorId: "aquarium-lobby" };
  };

  const dispose = () => {
    disposed = true;
    cancelPopulation();
    populationScheduled = false;
    sourceOffice = null;
  };

  return {
    group,
    get floorGroups() {
      return full?.floorGroups || shell.floorGroups;
    },
    get hyperloopCars() {
      return full?.hyperloopCars || shell.hyperloopCars;
    },
    setAccess,
    setSoundEmitter,
    populateFromExisting,
    interact,
    interactNearest,
    constrainPlayer: (previous, player) =>
      full?.constrainPlayer(previous, player) === true,
    update,
    isRiding: () => full?.isRiding() === true,
    isInside: () => full?.isInside() === true,
    currentFloorId: () => full?.currentFloorId() || "aquarium-lobby",
    access: () =>
      full?.access() || {
        ...accessState,
        allowedFloorIds: [...accessState.allowedFloorIds],
      },
    isLoaded: () => full !== null,
    dispose,
  };
}


// ---------------------------------------------------------------------------
// Marketplace package surface
//
// The World host reads `manifest`, `soundProfiles`, and `install()`. The
// instance returned by `install()` exposes the district contract the host
// loops over each frame: spaces, walk surface, player constraint, update,
// interaction, camera bounds, access, and dispose.

export const manifest = Object.freeze({
  id: "forkmesh-sky-office",
  version: "1.0.0",
  label: "ForkMesh Aquarium Sky Office",
  category: "3D",
  summary:
    "An opt-in aquarium office island with a glass lift, summit workspace, drone roof, and twin-tube hyperloop.",
});

// Space ids are namespaced "pkg-<package-id>-<floor>": the relay accepts the
// shape without knowing the package, and the host keys floors on them.
export const FORKMESH_SKY_OFFICE_SPACE_PREFIX = "pkg-forkmesh-sky-office-";
export const FORKMESH_SKY_OFFICE_WORLD_RADIUS = Math.ceil(
  Math.hypot(FORKMESH_SKY_OFFICE_CENTER[0], FORKMESH_SKY_OFFICE_CENTER[2]) +
    FORKMESH_SKY_OFFICE_ISLAND_RADIUS +
    32,
);
const spaceSpec = (floorY) =>
  Object.freeze({
    floorY,
    radius: FORKMESH_SKY_OFFICE_WORLD_RADIUS,
    ceiling: FORKMESH_SKY_OFFICE_ROOF_Y + 40,
    label: "Forkmesh Office",
    remoteHorizon: true,
  });
export const FORKMESH_SKY_OFFICE_SPACES = Object.freeze({
  [`${FORKMESH_SKY_OFFICE_SPACE_PREFIX}campus`]: spaceSpec(0.38),
  [`${FORKMESH_SKY_OFFICE_SPACE_PREFIX}aquarium-lobby`]: spaceSpec(0.38),
  [`${FORKMESH_SKY_OFFICE_SPACE_PREFIX}glass-lounge`]: spaceSpec(
    FORKMESH_SKY_OFFICE_LOUNGE_Y + 0.38,
  ),
  [`${FORKMESH_SKY_OFFICE_SPACE_PREFIX}summit-office`]: spaceSpec(
    FORKMESH_SKY_OFFICE_SUMMIT_Y + 0.38,
  ),
  [`${FORKMESH_SKY_OFFICE_SPACE_PREFIX}drone-roof`]: spaceSpec(
    FORKMESH_SKY_OFFICE_ROOF_Y + 0.38,
  ),
});

// One synthesized cue per sound event: [frequency, endFrequency, duration,
// volume, oscillator type, harmonic]. The host's audio context plays them so
// the package never owns an AudioContext of its own.
export const soundProfiles = Object.freeze({
  "sky-office-door-open": [196, 293.66, 0.34, 0.026, "triangle", 1.28],
  "sky-office-door-close": [293.66, 174.61, 0.28, 0.026, "triangle", 0.72],
  "sky-office-aquarium-water": [73.42, 82.41, 1.5, 0.009, "sine", 1.12],
  "sky-office-aquarium-bubbles": [523.25, 783.99, 0.22, 0.012, "sine", 1.5],
  "sky-office-aquarium-fish": [329.63, 392, 0.3, 0.008, "triangle", 1.18],
  "sky-office-aquarium-feed": [880, 587.33, 0.18, 0.025, "square", 0.67],
  "sky-office-aquarium-tap": [1244.51, 932.33, 0.12, 0.02, "sine", 0.75],
  "sky-office-aquarium-light": [440, 880, 0.36, 0.024, "sine", 2],
  "sky-office-elevator-button": [659.25, 783.99, 0.16, 0.028, "sine", 1.19],
  "sky-office-elevator-denied": [233.08, 155.56, 0.42, 0.032, "square", 0.67],
  "sky-office-elevator-door-close": [246.94, 164.81, 0.38, 0.022, "triangle", 0.67],
  "sky-office-elevator-start": [82.41, 110, 0.58, 0.025, "sawtooth", 1.34],
  "sky-office-elevator-motor": [65.41, 98, 0.72, 0.013, "sine", 1.18],
  "sky-office-elevator-collar": [392, 523.25, 0.11, 0.015, "triangle", 1.33],
  "sky-office-elevator-brake": [146.83, 92.5, 0.48, 0.024, "sawtooth", 0.63],
  "sky-office-elevator-arrive": [659.25, 987.77, 0.46, 0.036, "sine", 1.5],
  "sky-office-elevator-door-open": [174.61, 261.63, 0.36, 0.022, "triangle", 1.5],
  "sky-office-hyperloop-station": [220, 329.63, 0.54, 0.013, "sine", 1.5],
  "sky-office-hyperloop-load": [130.81, 196, 0.42, 0.025, "triangle", 1.5],
  "sky-office-hyperloop-seal": [196, 98, 0.32, 0.027, "square", 0.5],
  "sky-office-hyperloop-charge": [55, 164.81, 0.62, 0.03, "sawtooth", 3],
  "sky-office-hyperloop-launch": [73.42, 220, 0.82, 0.035, "sawtooth", 3],
  "sky-office-hyperloop-whoosh": [98, 146.83, 0.44, 0.018, "triangle", 1.5],
  "sky-office-hyperloop-brake": [174.61, 58.27, 0.72, 0.032, "sawtooth", 0.33],
  "sky-office-hyperloop-arrive": [523.25, 1046.5, 0.48, 0.04, "sine", 2],
  "sky-office-hyperloop-unseal": [164.81, 329.63, 0.34, 0.025, "triangle", 2],
  "sky-office-cube-hum": [110, 164.81, 0.9, 0.012, "sine", 1.5],
  "sky-office-cube-touch": [329.63, 987.77, 0.44, 0.032, "triangle", 3],
  "sky-office-reception-chime": [587.33, 880, 0.5, 0.034, "sine", 1.5],
  "sky-office-lounge-seat": [164.81, 130.81, 0.22, 0.018, "triangle", 0.79],
  "sky-office-footstep": [110, 73.42, 0.1, 0.014, "triangle", 1.06],
  "sky-office-widget": [440, 659.25, 0.24, 0.02, "square", 1.5],
  "sky-office-summit-electronics": [261.63, 392, 0.28, 0.009, "square", 1.5],
  "sky-office-roof-wind": [55, 73.42, 1.25, 0.008, "sine", 1.18],
  "sky-office-drone-pad": [98, 392, 0.68, 0.032, "sawtooth", 4],
});

// The island is part of the outdoor World scene, so it cannot use the
// original Office's local-space camera envelope. Keep the orbit eye inside
// the occupied circular floor (or the moving car) instead of letting a
// zoom-out gesture pull the visitor through the glass.
function skyOfficeCameraDistanceLimit(office, target, direction, requestedDistance, player) {
  const requested = Math.max(0, Number(requestedDistance) || 0);
  if (!office.isInside() || !target || !direction || !player || requested <= 0) {
    return requested;
  }
  let boundaryDistance = Infinity;
  if (office.isRiding()) {
    // Both the glass elevator and the hyperloop car have a compact cabin.
    // Anchor the bounds to the live avatar position so they move with either
    // vehicle rather than leaving a camera behind at the departure point.
    const bounds = [
      [target.x, direction.x, player.position.x - 3.9, player.position.x + 3.9],
      [target.y, direction.y, player.position.y + 0.48, player.position.y + 6.9],
      [target.z, direction.z, player.position.z - 3.45, player.position.z + 3.45],
    ];
    bounds.forEach(([coordinate, component, min, max]) => {
      if (Math.abs(component) < 1e-6) return;
      const edge = component > 0 ? max : min;
      const distance = (edge - coordinate) / component;
      if (distance >= 0) boundaryDistance = Math.min(boundaryDistance, distance);
    });
  } else {
    const floorId = office.currentFloorId();
    const floorBase =
      FORKMESH_SKY_OFFICE_SPACES[`${FORKMESH_SKY_OFFICE_SPACE_PREFIX}${floorId}`]
        ?.floorY ?? 0.38;
    const floorRadius =
      floorId === "aquarium-lobby"
        ? FORKMESH_SKY_OFFICE_RADIUS - FORKMESH_SKY_OFFICE_AQUARIUM_DEPTH - 0.8
        : floorId === "drone-roof"
          ? FORKMESH_SKY_OFFICE_RADIUS + 0.35
          : FORKMESH_SKY_OFFICE_RADIUS - 0.8;
    const floorCeiling =
      floorId === "drone-roof"
        ? floorBase + 7.4
        : floorBase + FORKMESH_SKY_OFFICE_FLOOR_HEIGHT - 0.58;
    const verticalComponent = Number(direction.y) || 0;
    if (verticalComponent > 1e-6) {
      const distance = (floorCeiling - target.y) / verticalComponent;
      if (distance >= 0) boundaryDistance = Math.min(boundaryDistance, distance);
    } else if (verticalComponent < -1e-6) {
      const distance = (floorBase + 0.3 - target.y) / verticalComponent;
      if (distance >= 0) boundaryDistance = Math.min(boundaryDistance, distance);
    }
    const localX = target.x - FORKMESH_SKY_OFFICE_CENTER[0];
    const localZ = target.z - FORKMESH_SKY_OFFICE_CENTER[2];
    const horizontalX = Number(direction.x) || 0;
    const horizontalZ = Number(direction.z) || 0;
    const horizontalLengthSq = horizontalX ** 2 + horizontalZ ** 2;
    if (horizontalLengthSq > 1e-8) {
      const halfB = localX * horizontalX + localZ * horizontalZ;
      const c = localX ** 2 + localZ ** 2 - floorRadius ** 2;
      const discriminant = halfB ** 2 - horizontalLengthSq * c;
      if (discriminant >= 0) {
        const distance = (-halfB + Math.sqrt(discriminant)) / horizontalLengthSq;
        if (distance >= 0) boundaryDistance = Math.min(boundaryDistance, distance);
      }
    }
  }
  if (!Number.isFinite(boundaryDistance)) return requested;
  return Math.min(requested, Math.max(0, boundaryDistance - 0.24));
}

// Approaching either hyperloop station (or boarding a car) asks the host to
// hydrate the visitor's organization role once; leaving without riding
// releases the latch so a later approach can ask again.
function stationNear(player) {
  return (
    Math.abs(player.position.x - FORKMESH_SKY_OFFICE_CENTER[0]) < 24 &&
    Math.min(
      Math.abs(player.position.z - FORKMESH_SKY_OFFICE_HYPERLOOP_START_Z),
      Math.abs(player.position.z - FORKMESH_SKY_OFFICE_HYPERLOOP_END_Z),
    ) < 24
  );
}

export function install(host) {
  if (!host?.THREE) {
    throw new Error("forkmesh-sky-office requires the World district host");
  }
  const office = createForkmeshSkyOffice(host.THREE, {
    interactive: host.interactive || [],
    animated: host.animated || [],
  });
  office.setSoundEmitter((stage, detail) => host.emitSound?.(stage, detail));
  let disposed = false;
  return {
    group: office.group,
    spaces: FORKMESH_SKY_OFFICE_SPACES,
    enclosure: Object.freeze({
      center: FORKMESH_SKY_OFFICE_CENTER,
      radius: FORKMESH_SKY_OFFICE_ISLAND_RADIUS,
    }),
    walkSurfaceContains: forkmeshSkyOfficeSurfaceContains,
    constrainPlayer: (previous, player) => office.constrainPlayer(previous, player),
    setAccess: (payload) => office.setAccess(payload),
    onWorldReady: ({ officeInterior } = {}) =>
      office.populateFromExisting(officeInterior),
    interact: (object, context = {}) => {
      const type = String(object?.userData?.interactive || "");
      if (type === "forkmesh-sky-office-hyperloop-car") context.requestAccess?.();
      return office.interact(object, context);
    },
    interactNearest: (player, context = {}) => {
      const boarded = office.interactNearest(player) === true;
      if (boarded) context.requestAccess?.();
      return boarded;
    },
    update: (frame) => {
      const player = frame?.player;
      if (player && stationNear(player)) frame.requestAccess?.();
      else if (!office.isRiding()) frame.releaseAccess?.();
      return office.update(frame);
    },
    cameraDistanceLimit: (target, direction, requested, player) =>
      skyOfficeCameraDistanceLimit(office, target, direction, requested, player),
    state: () => ({
      inside: office.isInside(),
      riding: office.isRiding(),
      floorId: office.currentFloorId(),
      access: office.access(),
    }),
    isLoaded: () => office.isLoaded(),
    dispose: () => {
      if (disposed) return;
      disposed = true;
      office.dispose();
      host.disposeObject3D?.(office.group);
    },
  };
}
