import { hexRoadRingGeometry } from "./instance-hexes.js";
import { makeLabelSprite } from "./labels.js";
import {
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  DESERT_PLOT_SURFACE_Y,
  WORLD_BIKE_LANE_APOTHEM,
  WORLD_ROAD_CORNER_SEGMENTS,
  WORLD_ROAD_SURFACE_Y,
  WORLD_HEX_TILES,
  WORLD_ROAD_WIDTH,
  deterministicFraction,
} from "./layout.js";
import { makeMaterial, makeSharedMaterial } from "./materials.js";
import {
  roundedHexagonOutline,
  setTerritoryTiles,
  territoryWalkActive,
} from "./movement.js";
import { setShadows } from "./object-utils.js";
import { OFFICE_AVATAR_RADIUS } from "../world-office-tower.js";
import { createBugSwarm, mergeParts } from "./territory-bugs.js";
import {
  TERRITORY_CENTER,
  TERRITORY_WALL_THICKNESS,
  constrainToFootprints,
  crossedInto,
  footprintDoorDistance,
  footprintWorld,
  forkmeshArrivalSpawn,
  forkmeshShiftAround,
  territoryPetalTile,
  forkmeshTilesAround,
  portalBorderLocal,
  territoryArrivalSpawn,
  hexRoadContains,
  territoryFootprint,
} from "./territory-geometry.js";
import { roundedHexagonGeometry } from "./town.js";
import { TERRITORY_NEIGHBOUR_ELEMENTS, TERRITORY_PLAZA } from "../world-territory.js";
import * as resources from "./resources.js";

export const TERRITORY_PLAZA_RADIUS = 26;
export const TERRITORY_TERRARIUM = Object.freeze({ width: 26, depth: 16, height: 8 });
const SIGN_WIDTH = 1024;
const SIGN_HEIGHT = 512;
const CROSSING_RESET_MS = 4000;
const TREE_COUNT = 34;

function loadImage(url, apply) {
  const image = new Image();
  image.decoding = "async";
  image.onload = () => apply(image);
  image.src = url;
}

function imageTexture(THREE, image, size, background = "") {
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const context = canvas.getContext("2d");
  if (background) {
    context.fillStyle = background;
    context.fillRect(0, 0, size, size);
  }
  const width = image.naturalWidth || size;
  const height = image.naturalHeight || size;
  const scale = Math.min(size / width, size / height) * 0.86;
  context.drawImage(
    image,
    (size - width * scale) / 2,
    (size - height * scale) / 2,
    width * scale,
    height * scale,
  );
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

function hexBandGeometry(THREE, inner, outer) {
  const scale = CONTINUOUS_CITY_CORNER_RADIUS - CONTINUOUS_CITY_RADIUS;
  const inside = roundedHexagonOutline(inner, inner + scale, WORLD_ROAD_CORNER_SEGMENTS);
  const outside = roundedHexagonOutline(outer, outer + scale, WORLD_ROAD_CORNER_SEGMENTS);
  const positions = [];
  const indices = [];
  inside.forEach((point, index) => {
    positions.push(point[0], point[1], 0, outside[index][0], outside[index][1], 0);
    const next = (index + 1) % inside.length;
    indices.push(index * 2, index * 2 + 1, next * 2 + 1, index * 2, next * 2 + 1, next * 2);
  });
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.Float32BufferAttribute(positions, 3));
  geometry.setIndex(indices);
  geometry.computeVertexNormals();
  return geometry;
}

const CIRCUIT_TEXTURE = 512;
const CIRCUIT_TILE = 44;

function circuitCanvases(palette) {
  const base = document.createElement("canvas");
  const glow = document.createElement("canvas");
  base.width = base.height = glow.width = glow.height = CIRCUIT_TEXTURE;
  const paint = base.getContext("2d");
  const light = glow.getContext("2d");
  paint.fillStyle = palette.ground;
  paint.fillRect(0, 0, CIRCUIT_TEXTURE, CIRCUIT_TEXTURE);
  light.fillStyle = "#000000";
  light.fillRect(0, 0, CIRCUIT_TEXTURE, CIRCUIT_TEXTURE);
  const cell = CIRCUIT_TEXTURE / 16;
  paint.lineWidth = 1;
  for (let line = 0; line <= 16; line += 1) {
    const major = line % 4 === 0;
    paint.strokeStyle = major ? "rgba(150, 170, 185, 0.16)" : "rgba(150, 170, 185, 0.07)";
    paint.beginPath();
    paint.moveTo(line * cell + 0.5, 0);
    paint.lineTo(line * cell + 0.5, CIRCUIT_TEXTURE);
    paint.moveTo(0, line * cell + 0.5);
    paint.lineTo(CIRCUIT_TEXTURE, line * cell + 0.5);
    paint.stroke();
  }
  for (let trace = 0; trace < 14; trace += 1) {
    const random = (key) => deterministicFraction(`circuit:${trace}:${key}`);
    let x = Math.floor(random("x") * 16) * cell;
    let y = Math.floor(random("y") * 16) * cell;
    const points = [[x, y]];
    let horizontal = random("h") < 0.5;
    for (let leg = 0; leg < 3; leg += 1) {
      const run = (1 + Math.floor(random(`r${leg}`) * 4)) * cell * (random(`s${leg}`) < 0.5 ? -1 : 1);
      if (horizontal) x += run;
      else y += run;
      points.push([x, y]);
      const bend = cell * (random(`b${leg}`) < 0.5 ? -1 : 1);
      x += bend;
      y += bend;
      points.push([x, y]);
      horizontal = !horizontal;
    }
    const red = trace % 5 === 0;
    [[paint, red ? "rgba(231, 76, 60, 0.22)" : "rgba(62, 230, 196, 0.14)", 2],
      [light, red ? "rgba(231, 76, 60, 0.32)" : "rgba(62, 230, 196, 0.28)", 1.5]].forEach(([context, color, width]) => {
      [-CIRCUIT_TEXTURE, 0, CIRCUIT_TEXTURE].forEach((ox) => {
        [-CIRCUIT_TEXTURE, 0, CIRCUIT_TEXTURE].forEach((oy) => {
          context.strokeStyle = color;
          context.fillStyle = color;
          context.lineWidth = width;
          context.beginPath();
          points.forEach(([px, py], index) => {
            if (index) context.lineTo(px + ox, py + oy);
            else context.moveTo(px + ox, py + oy);
          });
          context.stroke();
          [points[0], points[points.length - 1]].forEach(([px, py]) => {
            context.beginPath();
            context.arc(px + ox, py + oy, 3.5, 0, Math.PI * 2);
            context.fill();
          });
        });
      });
    });
  }
  return { base, glow };
}

function circuitGroundMaterial(THREE, palette) {
  const { base, glow } = circuitCanvases(palette);
  const repeat = (CONTINUOUS_CITY_RADIUS * 2) / CIRCUIT_TILE;
  const texture = (canvas, srgb) => {
    const map = new THREE.CanvasTexture(canvas);
    if (srgb) map.colorSpace = THREE.SRGBColorSpace;
    map.wrapS = map.wrapT = THREE.RepeatWrapping;
    map.repeat.set(repeat, repeat);
    map.anisotropy = 8;
    return map;
  };
  return resources.uniqueMaterial(THREE, {
    color: "#ffffff",
    map: texture(base, true),
    emissive: "#ffffff",
    emissiveMap: texture(glow, true),
    emissiveIntensity: 0.35,
    roughness: 0.92,
    metalness: 0.05,
  }, "MeshStandardMaterial");
}

function ground(THREE, name, apothem, corner, color, y, material = null) {
  const mesh = new THREE.Mesh(
    resources.geometry(THREE, `territory-ground:${apothem}:${corner}`, () =>
      roundedHexagonGeometry(THREE, apothem, corner, WORLD_ROAD_CORNER_SEGMENTS),
    ),
    material || makeSharedMaterial(THREE, color, { roughness: 1, metalness: 0 }),
  );
  mesh.name = name;
  mesh.rotation.x = -Math.PI / 2;
  mesh.position.y = y;
  mesh.receiveShadow = true;
  mesh.userData.ground = true;
  return mesh;
}

function paintSign(context, territory, logo) {
  const { palette } = territory;
  context.clearRect(0, 0, SIGN_WIDTH, SIGN_HEIGHT);
  context.fillStyle = palette.sign;
  context.fillRect(0, 0, SIGN_WIDTH, SIGN_HEIGHT);
  context.strokeStyle = palette.accent;
  context.lineWidth = 14;
  context.strokeRect(7, 7, SIGN_WIDTH - 14, SIGN_HEIGHT - 14);
  const logoSize = 280;
  if (logo) {
    const width = logo.naturalWidth || logoSize;
    const height = logo.naturalHeight || logoSize;
    const scale = Math.min(logoSize / width, logoSize / height);
    context.drawImage(
      logo,
      60 + (logoSize - width * scale) / 2,
      110 + (logoSize - height * scale) / 2,
      width * scale,
      height * scale,
    );
  } else {
    context.fillStyle = palette.accent;
    context.fillRect(60, 110, logoSize, logoSize);
  }
  context.textAlign = "left";
  context.textBaseline = "middle";
  context.fillStyle = palette.signText;
  let size = 92;
  context.font = `800 ${size}px system-ui, sans-serif`;
  while (size > 40 && context.measureText(territory.name).width > 560) {
    size -= 4;
    context.font = `800 ${size}px system-ui, sans-serif`;
  }
  context.fillText(territory.name, 380, 210);
  context.fillStyle = palette.accent;
  context.font = '500 40px ui-monospace, "ForkMesh Mono", monospace';
  context.fillText(
    territory.style === "security" ? "> secure by design_" : "Welcome to our World",
    380,
    300,
  );
}

function makeSign(THREE, territory) {
  const group = new THREE.Group();
  group.name = "territory-sign";
  const canvas = document.createElement("canvas");
  canvas.width = SIGN_WIDTH;
  canvas.height = SIGN_HEIGHT;
  const context = canvas.getContext("2d");
  paintSign(context, territory, null);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 16, 8),
    resources.uniqueMaterial(THREE, { map: texture, toneMapped: false }, "MeshBasicMaterial"),
  );
  face.name = "territory-sign-face";
  face.position.set(0, 7, 0.32);
  group.add(face);
  const board = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 16.8, 8.8, 0.5),
    makeSharedMaterial(THREE, territory.palette.trim, { roughness: 0.7 }),
  );
  board.position.set(0, 7, 0);
  group.add(board);
  [-6.5, 6.5].forEach((x) => {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.7, 3, 0.7),
      makeSharedMaterial(THREE, territory.palette.trim, { roughness: 0.7 }),
    );
    post.position.set(x, 1.5, 0);
    group.add(post);
  });
  loadImage(territory.logo, (image) => {
    paintSign(context, territory, image);
    texture.needsUpdate = true;
  });
  return group;
}

function makeBench(THREE, palette) {
  const group = new THREE.Group();
  const seat = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 4, 0.3, 1.2),
    makeSharedMaterial(THREE, palette.trim, { roughness: 0.8 }),
  );
  seat.position.y = 0.9;
  group.add(seat);
  [-1.6, 1.6].forEach((x) => {
    const leg = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.25, 0.9, 1),
      makeSharedMaterial(THREE, palette.road, { roughness: 0.8 }),
    );
    leg.position.set(x, 0.45, 0);
    group.add(leg);
  });
  return group;
}

function makeLamp(THREE, palette) {
  const group = new THREE.Group();
  const pole = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.14, 0.18, 5, 6),
    makeSharedMaterial(THREE, palette.road, { roughness: 0.6, metalness: 0.4 }),
  );
  pole.position.y = 2.5;
  group.add(pole);
  const bulb = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.45, 10, 8),
    makeSharedMaterial(THREE, palette.lamp, {
      emissive: palette.lamp,
      emissiveIntensity: 1.4,
    }),
  );
  bulb.position.y = 5.2;
  group.add(bulb);
  return group;
}

function makeTree(THREE, palette, scale) {
  const group = new THREE.Group();
  const trunk = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.35, 0.5, 3, 6),
    makeSharedMaterial(THREE, palette.trunk, { roughness: 0.9 }),
  );
  trunk.position.y = 1.5;
  group.add(trunk);
  const crown = new THREE.Mesh(
    resources.primitive(THREE, "ConeGeometry", 2.2, 5, 7),
    makeSharedMaterial(THREE, palette.foliage, { roughness: 0.9 }),
  );
  crown.position.y = 5.2;
  group.add(crown);
  group.scale.setScalar(scale);
  return group;
}

function makePadlock(THREE, palette, scale = 1) {
  const group = new THREE.Group();
  group.name = "territory-padlock";
  const body = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 3, 2.6, 1.2),
    makeSharedMaterial(THREE, palette.accent, { roughness: 0.4, metalness: 0.5 }),
  );
  body.position.y = 1.3;
  group.add(body);
  const shackle = new THREE.Mesh(
    resources.primitive(THREE, "TorusGeometry", 1, 0.22, 8, 16, Math.PI),
    makeSharedMaterial(THREE, "#c9ced2", { roughness: 0.3, metalness: 0.8 }),
  );
  shackle.position.y = 2.6;
  group.add(shackle);
  const keyhole = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.22, 0.22, 0.1, 10),
    makeSharedMaterial(THREE, palette.sign, { roughness: 0.8 }),
  );
  keyhole.rotation.x = Math.PI / 2;
  keyhole.position.set(0, 1.4, 0.62);
  group.add(keyhole);
  group.scale.setScalar(scale);
  return group;
}

function makeShield(THREE, palette) {
  const shape = new THREE.Shape();
  shape.moveTo(0, 2.6);
  shape.quadraticCurveTo(1.2, 2.2, 2, 2.4);
  shape.lineTo(2, 0.6);
  shape.quadraticCurveTo(1.8, -1.6, 0, -2.6);
  shape.quadraticCurveTo(-1.8, -1.6, -2, 0.6);
  shape.lineTo(-2, 2.4);
  shape.quadraticCurveTo(-1.2, 2.2, 0, 2.6);
  const mesh = new THREE.Mesh(
    resources.geometry(THREE, "territory-shield", () =>
      new THREE.ExtrudeGeometry(shape, { depth: 0.4, bevelEnabled: false }),
    ),
    makeSharedMaterial(THREE, palette.accent, { roughness: 0.35, metalness: 0.45 }),
  );
  mesh.name = "territory-shield";
  return mesh;
}

export const TERRITORY_HOUSE = Object.freeze({ width: 14, depth: 12, height: 9, door: 3.4 });
const TERRARIUM_DOOR = Object.freeze({ width: 4.4, height: 5.8 });
const DOOR_GLOW = "#3ee6c4";

function houseDoorParts(THREE, color, accent) {
  const { width, door } = TERRITORY_HOUSE;
  const front = TERRITORY_HOUSE.depth / 2;
  const frame = "#15191c";
  return mergeParts(THREE, [
    ...[-1, 1].map((side) =>
      colouredBox(THREE, [0.4, 6.2, 0.5], frame, [side * (door / 2 + 0.2), 3.1, front + 0.05])),
    colouredBox(THREE, [door + 0.8, 0.45, 0.5], frame, [0, 6.05, front + 0.05]),
    colouredBox(THREE, [door + 2.4, 0.06, 2.4], accent, [0, 0.03, front + 1.3]),
    colouredBox(THREE, [width + 0.2, 0.25, 0.25], color, [0, 9, front + 0.02]),
  ]);
}

function makeHouse(THREE, object, palette, interactive) {
  const group = new THREE.Group();
  group.name = "territory-house";
  const color = object.color || palette.accent;
  const { width, depth, height, door } = TERRITORY_HOUSE;
  const body = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", width, height, depth),
    makeSharedMaterial(THREE, palette.style === "security" ? "#2f363c" : "#f1e9da", {
      roughness: 0.8,
    }),
  );
  body.position.y = height / 2;
  group.add(body);
  const roof = new THREE.Mesh(
    resources.primitive(THREE, "ConeGeometry", 11, 6, 4),
    makeSharedMaterial(THREE, color, { roughness: 0.6 }),
  );
  roof.position.y = height + 3;
  roof.rotation.y = Math.PI / 4;
  group.add(roof);
  const doorway = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", door, 5.4),
    makeSharedMaterial(THREE, color, {
      emissive: color,
      emissiveIntensity: 0.75,
    }),
  );
  doorway.name = "territory-house-door";
  doorway.position.set(0, 2.7, depth / 2 + 0.02);
  group.add(doorway);
  const trim = new THREE.Mesh(
    resources.geometry(THREE, `territory-house-trim:${color}`, () =>
      houseDoorParts(THREE, color, DOOR_GLOW)),
    resources.material(THREE, {
      color: "#ffffff", vertexColors: true, roughness: 0.6, metalness: 0.2,
      emissive: "#0b3b33", emissiveIntensity: 0.6,
    }, "MeshStandardMaterial"),
  );
  group.add(trim);
  [-4.4, 4.4].forEach((x) => {
    const window = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 2.6, 2.2),
      makeSharedMaterial(THREE, palette.lamp, {
        emissive: palette.lamp,
        emissiveIntensity: 0.6,
      }),
    );
    window.position.set(x, 5.4, depth / 2 + 0.02);
    group.add(window);
  });
  const label = makeLabelSprite(THREE, object.label.slice(0, 22), object.host.slice(0, 32), color);
  label.scale.set(15, 5, 1);
  label.position.y = 19;
  group.add(label);
  const hint = makeLabelSprite(THREE, "Walk in", "or press E", DOOR_GLOW);
  hint.scale.set(4.8, 1.6, 1);
  hint.position.set(0, 7.6, depth / 2 + 0.6);
  group.add(hint);
  [body, roof, doorway, trim, label, hint].forEach((part) => {
    part.userData.interactive = "territory-link";
    part.userData.url = object.url;
    part.userData.label = object.label;
    interactive.push(part);
  });
  setShadows(group, true, false);
  return group;
}

function colouredBox(THREE, [x, y, z], color, position, rotation = [0, 0, 0]) {
  return colouredPart(THREE, new THREE.BoxGeometry(x, y, z), color, position, rotation);
}

function colouredPart(THREE, geometry, color, position, rotation = [0, 0, 0], scale = [1, 1, 1]) {
  const flat = geometry.toNonIndexed();
  geometry.dispose();
  flat.applyMatrix4(new THREE.Matrix4().compose(
    new THREE.Vector3(...position),
    new THREE.Quaternion().setFromEuler(new THREE.Euler(...rotation)),
    new THREE.Vector3(...scale),
  ));
  const tint = new THREE.Color(color);
  const count = flat.attributes.position.count;
  const colors = new Float32Array(count * 3);
  for (let index = 0; index < count; index += 1) colors.set([tint.r, tint.g, tint.b], index * 3);
  const out = {
    position: flat.attributes.position.array,
    normal: flat.attributes.normal.array,
    color: colors,
  };
  flat.dispose();
  return out;
}

function terrariumGlass(THREE) {
  const { width, depth, height } = TERRITORY_TERRARIUM;
  const t = TERRITORY_WALL_THICKNESS;
  const door = TERRARIUM_DOOR;
  const side = (width - door.width) / 2;
  const glass = "#ffffff";
  return mergeParts(THREE, [
    colouredBox(THREE, [width, height, t * 0.4], glass, [0, height / 2, -depth / 2]),
    colouredBox(THREE, [t * 0.4, height, depth], glass, [-width / 2, height / 2, 0]),
    colouredBox(THREE, [t * 0.4, height, depth], glass, [width / 2, height / 2, 0]),
    colouredBox(THREE, [side, height, t * 0.4], glass, [-(door.width + side) / 2, height / 2, depth / 2]),
    colouredBox(THREE, [side, height, t * 0.4], glass, [(door.width + side) / 2, height / 2, depth / 2]),
    colouredBox(THREE, [door.width, height - door.height, t * 0.4], glass,
      [0, door.height + (height - door.height) / 2, depth / 2]),
    colouredBox(THREE, [width, t * 0.4, depth], glass, [0, height, 0]),
    colouredBox(THREE, [door.width - 0.2, door.height - 0.2, t * 0.4], glass,
      [door.width / 2 + 0.1, door.height / 2, depth / 2 + (door.width - 0.2) / 2 + 0.2],
      [0, Math.PI / 2, 0]),
  ]);
}

function terrariumFrame(THREE, accent) {
  const { width, depth, height } = TERRITORY_TERRARIUM;
  const door = TERRARIUM_DOOR;
  const metal = "#20262b";
  const post = 0.32;
  const parts = [];
  [-1, 1].forEach((sx) => {
    [-1, 1].forEach((sz) => {
      parts.push(colouredBox(THREE, [post, height, post], metal, [(sx * width) / 2, height / 2, (sz * depth) / 2]));
    });
  });
  [0, height].forEach((y) => {
    [-1, 1].forEach((sz) => {
      if (y === 0 && sz > 0) {
        const side = (width - door.width) / 2;
        [-1, 1].forEach((sx) => parts.push(colouredBox(THREE, [side, post, post], metal,
          [sx * (door.width + side) / 2, y + post / 2, (sz * depth) / 2])));
        return;
      }
      parts.push(colouredBox(THREE, [width, post, post], metal, [0, y + (y ? 0 : post / 2), (sz * depth) / 2]));
    });
    [-1, 1].forEach((sx) => {
      parts.push(colouredBox(THREE, [post, post, depth], metal, [(sx * width) / 2, y + (y ? 0 : post / 2), 0]));
    });
  });
  [-1, 1].forEach((sx) => {
    parts.push(colouredBox(THREE, [0.4, door.height + 0.2, 0.45], accent,
      [(sx * (door.width + 0.4)) / 2, (door.height + 0.2) / 2, depth / 2]));
  });
  parts.push(colouredBox(THREE, [door.width + 0.8, 0.4, 0.45], accent, [0, door.height + 0.2, depth / 2]));
  parts.push(colouredBox(THREE, [0.14, door.height - 0.2, 0.14], metal,
    [door.width / 2 + 0.1, door.height / 2, depth / 2 + door.width - 0.1]));
  return mergeParts(THREE, parts);
}

function terrariumDecor(THREE) {
  const { width, depth } = TERRITORY_TERRARIUM;
  const parts = [
    colouredBox(THREE, [width + 1, 0.12, depth + 1], "#2a3036", [0, 0.06, 0]),
    colouredBox(THREE, [width - 0.4, 0.06, depth - 0.4], "#3d3126", [0, 0.15, 0]),
  ];
  [[-8, -4, 1.2], [6, -5, 0.9], [10, 3, 0.7], [-11, 4, 0.6]].forEach(([x, z, size]) => {
    parts.push(colouredPart(THREE, new THREE.DodecahedronGeometry(size, 0), "#6f7476",
      [x, 0.15 + size * 0.45, z], [0.3, x, 0.2]));
  });
  parts.push(colouredPart(THREE, new THREE.CylinderGeometry(0.45, 0.55, 6, 8), "#5b4330",
    [-2, 0.55, -5], [0, 0.4, Math.PI / 2]));
  [[-6, 1], [3, -2], [8, -1], [-9, -1], [1, 4.5]].forEach(([x, z], index) => {
    const tall = 0.9 + (index % 3) * 0.45;
    parts.push(colouredPart(THREE, new THREE.ConeGeometry(0.35, tall, 6), index % 2 ? "#3f7d4e" : "#5d9c59",
      [x, 0.18 + tall / 2, z]));
    parts.push(colouredPart(THREE, new THREE.ConeGeometry(0.28, tall * 0.7, 6), "#2f6b3f",
      [x + 0.4, 0.18 + tall * 0.35, z + 0.3], [0, 0, -0.25]));
  });
  return mergeParts(THREE, parts);
}

export function terrariumFootprint(x, z, rotation) {
  const { width, depth } = TERRITORY_TERRARIUM;
  return territoryFootprint({ x, z, rotation, width, depth, hollow: true, door: TERRARIUM_DOOR.width });
}

export function houseFootprint(object, x, z, rotation) {
  const { width, depth, door } = TERRITORY_HOUSE;
  return territoryFootprint({
    x, z, rotation, width, depth, door,
    link: { url: object.url, label: object.label },
  });
}

function makeTerrarium(THREE, territory, interactive, animated) {
  const { palette, bugs: config } = territory;
  const { width, depth, height } = TERRITORY_TERRARIUM;
  const group = new THREE.Group();
  group.name = "territory-terrarium";
  const vertexMaterial = resources.material(THREE, {
    color: "#ffffff", vertexColors: true, roughness: 0.75, metalness: 0.25,
  }, "MeshStandardMaterial");
  const decor = new THREE.Mesh(
    resources.geometry(THREE, "territory-terrarium-decor", () => terrariumDecor(THREE)),
    vertexMaterial,
  );
  decor.receiveShadow = true;
  decor.userData.ground = true;
  group.add(decor);
  const frame = new THREE.Mesh(
    resources.geometry(THREE, `territory-terrarium-frame:${palette.accent}`, () =>
      terrariumFrame(THREE, palette.accent)),
    vertexMaterial,
  );
  group.add(frame);
  const glass = new THREE.Mesh(
    resources.geometry(THREE, "territory-terrarium-glass", () => terrariumGlass(THREE)),
    makeMaterial(THREE, "#bfe8ff", {
      transparent: true,
      opacity: 0.17,
      roughness: 0.05,
      metalness: 0.1,
    }),
  );
  glass.material.depthWrite = false;
  glass.name = "territory-terrarium-glass";
  glass.renderOrder = 5;
  group.add(glass);
  const threshold = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", TERRARIUM_DOOR.width, 0.04, 1.6),
    makeSharedMaterial(THREE, DOOR_GLOW, { emissive: DOOR_GLOW, emissiveIntensity: 0.9 }),
  );
  threshold.name = "territory-terrarium-threshold";
  threshold.position.set(0, 0.14, depth / 2 + 0.6);
  group.add(threshold);
  const doorSign = makeLabelSprite(THREE, "Way in", "mind the bugs", DOOR_GLOW);
  doorSign.scale.set(4.8, 1.6, 1);
  doorSign.position.set(0, TERRARIUM_DOOR.height + 1.6, depth / 2 + 0.4);
  group.add(doorSign);
  const label = makeLabelSprite(THREE, config.title.slice(0, 22), "click a bug", palette.accent);
  label.scale.set(15, 5, 1);
  label.position.y = height + 7;
  group.add(label);
  setShadows(frame, true, false);

  const swarm = createBugSwarm(THREE, {
    bounds: { x: width / 2 - 1.4, z: depth / 2 - 1.4 },
    ring: palette.accent,
    height,
    interactive,
  });
  group.add(swarm.group);
  let lastTime = 0;

  function update(list = []) {
    const count = swarm.update((Array.isArray(list) ? list : []).slice(0, config.limit));
    const previous = label.material;
    label.material = makeLabelSprite(
      THREE,
      config.title.slice(0, 22),
      count ? `${count} bugs · click one` : "no bugs yet",
      palette.accent,
    ).material;
    previous.map?.dispose?.();
    previous.dispose?.();
  }

  animated.push((time) => {
    const delta = lastTime ? Math.min(0.1, Math.max(0, (time - lastTime) / 1000)) : 0;
    lastTime = time;
    if (!delta || !group.parent) return;
    swarm.step(delta);
  });

  return { group, update };
}

function makeForkmeshNeighbours(THREE, territory, interactive) {
  const group = new THREE.Group();
  group.name = "territory-forkmesh-neighbours";
  const tiles = forkmeshTilesAround(territory.place);
  const border = portalBorderLocal(territory.place);
  if (border) {
    const length = Math.hypot(border.x, border.z) || 1;
    const normalX = border.x / length;
    const normalZ = border.z / length;
    const gate = new THREE.Group();
    gate.name = "territory-border-gate";
    gate.position.set(TERRITORY_CENTER.x + border.x, 0, TERRITORY_CENTER.z + border.z);
    gate.rotation.y = Math.atan2(normalX, normalZ);
    const postMaterial = makeSharedMaterial(THREE, territory.palette.accent, { roughness: 0.5 });
    [-9, 9].forEach((x) => {
      const post = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 0.8, 10, 0.8),
        postMaterial,
      );
      post.position.set(x, 5, 0);
      gate.add(post);
    });
    const banner = makeLabelSprite(THREE, "ForkMesh World", "walk across the road", "#9ef7c6");
    banner.scale.set(18, 6, 1);
    banner.position.set(0, 13, 0);
    gate.add(banner);
    const obelisk = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 1.1, 2, 12, 4),
      makeSharedMaterial(THREE, "#d8d0bf", { roughness: 0.9 }),
    );
    obelisk.position.set(16, 6, -22);
    obelisk.rotation.y = Math.PI / 4;
    gate.add(obelisk);
    const board = makeLabelSprite(THREE, "Portal", "ForkMesh World", "#9ef7c6");
    board.scale.set(9, 3, 1);
    board.position.set(16, 14, -22);
    gate.add(board);
    setShadows(gate, true, false);
    group.add(gate);
  }
  return { group, tiles };
}

export function buildInstanceTerritory({ THREE, territory, interactive, animated }) {
  const { palette } = territory;
  const root = new THREE.Group();
  root.name = "instance-territory";
  const home = new THREE.Group();
  home.name = "territory-home";
  home.position.set(TERRITORY_CENTER.x, 0, TERRITORY_CENTER.z);
  home.add(
    ground(
      THREE,
      "territory-ground",
      CONTINUOUS_CITY_RADIUS,
      CONTINUOUS_CITY_CORNER_RADIUS,
      palette.ground,
      DESERT_PLOT_SURFACE_Y,
      territory.style === "security" ? circuitGroundMaterial(THREE, palette) : null,
    ),
  );
  const reach = (WORLD_BIKE_LANE_APOTHEM * 2) / Math.sqrt(3);
  const corners = [0, 1, 2, 3, 4, 5].map((corner) => {
    const angle = Math.PI / 6 + (corner * Math.PI) / 3;
    return { x: Math.cos(angle) * reach, y: -Math.sin(angle) * reach };
  });
  const road = new THREE.Mesh(
    resources.geometry(THREE, "territory-road", () => hexRoadRingGeometry(THREE, corners)),
    makeSharedMaterial(THREE, palette.road, { roughness: 0.92, metalness: 0 }),
  );
  road.name = "territory-road";
  road.rotation.x = -Math.PI / 2;
  road.position.y = WORLD_ROAD_SURFACE_Y + 0.01;
  road.receiveShadow = true;
  road.userData.ground = true;
  home.add(road);
  const lane = new THREE.Mesh(
    resources.geometry(THREE, "territory-road-line", () => hexBandGeometry(THREE,
      CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH / 2 - 0.3,
      CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH / 2 + 0.3)),
    makeSharedMaterial(THREE, palette.trim, { roughness: 0.8, emissive: palette.accent, emissiveIntensity: 0.35 }),
  );
  lane.name = "territory-road-line";
  lane.rotation.x = -Math.PI / 2;
  lane.position.y = WORLD_ROAD_SURFACE_Y + 0.03;
  home.add(lane);
  root.add(home);

  const plaza = new THREE.Group();
  plaza.name = "territory-plaza";
  plaza.position.set(TERRITORY_PLAZA.x, 0, TERRITORY_PLAZA.z);
  const plazaFloor = new THREE.Mesh(
    resources.primitive(THREE, "CircleGeometry", TERRITORY_PLAZA_RADIUS, 48),
    makeSharedMaterial(THREE, palette.plaza, { roughness: 0.9 }),
  );
  plazaFloor.rotation.x = -Math.PI / 2;
  plazaFloor.position.y = DESERT_PLOT_SURFACE_Y + 0.04;
  plazaFloor.receiveShadow = true;
  plazaFloor.userData.ground = true;
  plaza.add(plazaFloor);
  const rim = new THREE.Mesh(
    resources.primitive(THREE, "RingGeometry", TERRITORY_PLAZA_RADIUS - 0.8, TERRITORY_PLAZA_RADIUS, 64),
    makeSharedMaterial(THREE, palette.accent, { roughness: 0.6, emissive: palette.accent, emissiveIntensity: 0.25 }),
  );
  rim.rotation.x = -Math.PI / 2;
  rim.position.y = DESERT_PLOT_SURFACE_Y + 0.06;
  plaza.add(rim);
  const groundLogoMaterial = resources.uniqueMaterial(THREE, {
    transparent: true,
    opacity: 0.9,
    depthWrite: false,
    polygonOffset: true,
    polygonOffsetFactor: -2,
    polygonOffsetUnits: -2,
    toneMapped: false,
  }, "MeshBasicMaterial");
  const groundLogo = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 14, 14),
    groundLogoMaterial,
  );
  groundLogo.rotation.x = -Math.PI / 2;
  groundLogo.position.y = DESERT_PLOT_SURFACE_Y + 0.08;
  groundLogo.visible = false;
  plaza.add(groundLogo);
  loadImage(territory.logo, (image) => {
    groundLogoMaterial.map = imageTexture(THREE, image, 256);
    groundLogoMaterial.needsUpdate = true;
    groundLogo.visible = true;
  });
  const sign = makeSign(THREE, territory);
  sign.position.set(0, 0, -TERRITORY_PLAZA_RADIUS + 4);
  plaza.add(sign);
  [[-11, 9, 0.5], [11, 9, -0.5], [-15, -2, 1.2], [15, -2, -1.2]].forEach(([x, z, turn]) => {
    const bench = makeBench(THREE, palette);
    bench.position.set(x, 0, z);
    bench.rotation.y = turn;
    plaza.add(bench);
  });
  [[-20, -14], [20, -14], [-20, 14], [20, 14]].forEach(([x, z]) => {
    const lamp = makeLamp(THREE, palette);
    lamp.position.set(x, 0, z);
    plaza.add(lamp);
  });
  if (territory.style === "security") {
    [[-30, -12, 1.1], [30, -12, 1.1], [-24, 22, 0.8]].forEach(([x, z, scale]) => {
      const lock = makePadlock(THREE, palette, scale);
      lock.position.set(x, 0, z);
      lock.rotation.y = Math.atan2(-x, -z);
      plaza.add(lock);
    });
    const shield = makeShield(THREE, palette);
    shield.position.set(0, 14.2, -TERRITORY_PLAZA_RADIUS + 4);
    shield.scale.setScalar(1.1);
    plaza.add(shield);
  }
  setShadows(plaza, true, false);
  root.add(plaza);

  const scenery = new THREE.Group();
  scenery.name = "territory-scenery";
  for (let index = 0; index < TREE_COUNT; index += 1) {
    const angle = deterministicFraction(`territory-tree-a:${index}`) * Math.PI * 2;
    const distance = 70 + deterministicFraction(`territory-tree-d:${index}`) * 250;
    const x = TERRITORY_CENTER.x + Math.cos(angle) * distance;
    const z = TERRITORY_CENTER.z + Math.sin(angle) * distance;
    if (Math.hypot(x, z - 130) < 50 || Math.hypot(x - 130, z) < 50) continue;
    const tree = makeTree(THREE, palette, 0.9 + deterministicFraction(`territory-tree-s:${index}`) * 0.8);
    tree.position.set(x, 0, z);
    scenery.add(tree);
  }
  setShadows(scenery, true, false);
  root.add(scenery);

  const footprints = [];
  territory.objects.forEach((object) => {
    const house = makeHouse(THREE, object, palette, interactive);
    const x = TERRITORY_PLAZA.x + object.x;
    const z = TERRITORY_PLAZA.z + object.z;
    const rotation = Math.atan2(-object.x, -object.z);
    house.position.set(x, 0, z);
    house.rotation.y = rotation;
    root.add(house);
    footprints.push(houseFootprint(object, x, z, rotation));
  });

  let terrarium = null;
  if (territory.bugs) {
    terrarium = makeTerrarium(THREE, territory, interactive, animated);
    const x = TERRITORY_PLAZA.x + territory.bugs.x;
    const z = TERRITORY_PLAZA.z + territory.bugs.z;
    const rotation = Math.atan2(-territory.bugs.x, -territory.bugs.z);
    terrarium.group.position.set(x, 0, z);
    terrarium.group.rotation.y = rotation;
    root.add(terrarium.group);
    const tank = terrariumFootprint(x, z, rotation);
    footprints.push(tank);
    const leaf = footprintWorld(
      tank,
      TERRARIUM_DOOR.width / 2 + 0.1,
      TERRITORY_TERRARIUM.depth / 2 + TERRARIUM_DOOR.width / 2,
    );
    footprints.push(territoryFootprint({
      x: leaf.x, z: leaf.z, rotation, width: TERRITORY_WALL_THICKNESS, depth: TERRARIUM_DOOR.width,
    }));
  }

  const neighbours = makeForkmeshNeighbours(THREE, territory, interactive);
  root.add(neighbours.group);
  return {
    root,
    walkTiles: [TERRITORY_CENTER, ...neighbours.tiles],
    forkmeshTiles: neighbours.tiles,
    crossings: [],
    footprints,
    updateBugs: (list) => terrarium?.update(list),
  };
}

const DOOR_REACH = 5;
const DOOR_HINT_REACH = 14;
const DOOR_HINT_MS = 30_000;

export function territoryPlace(territory) {
  return territory?.petal >= 0 ? territoryPetalTile(territory.petal) : territory?.slot;
}

export function functions({
  THREE, ctx, fns, territory, onTerritoryCross,
  onTerritoryLinkSelect = () => {}, onTerritoryNotice = () => {},
}) {
  if (territory && territory.place === undefined) territory.place = territoryPlace(territory);
  let crossings = [];
  let footprints = [];
  let built = null;
  let crossedAt = 0;
  let doorAt = 0;
  const hintedAt = new Map();

  const shifted = new WeakSet();
  let shift = null;
  let sweptAt = -Infinity;

  function showForkmeshWorld(now = performance.now()) {
    if (!territory || !shift || now - sweptAt < 1000) return;
    sweptAt = now;
    let moved = false;
    TERRITORY_NEIGHBOUR_ELEMENTS.forEach((id) => {
      ctx.worldElements.get(id)?.roots.forEach((root) => {
        if (shifted.has(root)) return;
        shifted.add(root);
        root.position.x += shift.x;
        root.position.z += shift.z;
        root.updateMatrix();
        root.updateMatrixWorld(true);
        root.traverse((part) => {
          const index = ctx.interactive.indexOf(part);
          if (index >= 0) ctx.interactive.splice(index, 1);
        });
        moved = true;
      });
    });
    if (moved) {
      ctx.groundSurfaces = null;
      ctx.desertPlots?.userData?.replacePetals?.(
        territory.petal >= 0 ? [territory.petal] : [],
      );
    }
  }

  function installInstanceTerritory() {
    if (!territory || built) return built;
    built = buildInstanceTerritory({
      THREE,
      territory,
      interactive: ctx.interactive,
      animated: ctx.animated,
    });
    ctx.world.add(built.root);
    fns.registerWorldElement("instance-territory", `${territory.name} territory`, "Districts", built.root);
    setTerritoryTiles(built.walkTiles);
    crossings = built.crossings;
    footprints = built.footprints;
    shift = forkmeshShiftAround(territory.place);
    showForkmeshWorld();
    ctx.animated.push((time) => showForkmeshWorld(time));
    ctx.groundSurfaces = null;
    return built;
  }

  function territoryOwnerAt(x, z) {
    if (!built) return "";
    for (const tile of built.forkmeshTiles) {
      if (hexRoadContains(x, z, tile, WORLD_ROAD_WIDTH / 2)) return "forkmesh";
    }
    return "";
  }

  function setForkmeshCrossings(list = []) {
    if (territory) return;
    crossings = Array.isArray(list) ? list : [];
  }

  function checkTerritoryCrossing(previous) {
    if (!crossings.length || !previous) return false;
    const now = performance.now();
    if (crossedAt && now - crossedAt < CROSSING_RESET_MS) return false;
    const tile = crossedInto(previous, ctx.player.position, crossings);
    if (!tile) return false;
    crossedAt = now;
    onTerritoryCross({
      ...tile.target,
      local: {
        x: ctx.player.position.x - tile.originX,
        z: ctx.player.position.z - tile.originZ,
      },
    });
    return true;
  }

  function enterDoor(footprint, via) {
    const now = performance.now();
    if (!footprint?.link || (doorAt && now - doorAt < CROSSING_RESET_MS)) return false;
    doorAt = now;
    fns.cancelDash?.();
    onTerritoryLinkSelect({ ...footprint.link, via });
    return true;
  }

  function constrainTerritoryObstacles(previous) {
    if (!footprints.length || !previous || ctx.superJumping) return false;
    const position = ctx.player.position;
    const result = constrainToFootprints(footprints, previous, position, OFFICE_AVATAR_RADIUS);
    if (result.blocked) {
      position.x = result.x;
      position.z = result.z;
      if (!result.door) fns.cancelDash?.();
    }
    if (result.door) enterDoor(result.door, "walk");
    hintNearbyDoor(position);
    return result.blocked;
  }

  function nearestDoor(position, reach) {
    let best = null;
    let bestDistance = reach;
    footprints.forEach((footprint) => {
      if (!footprint.link) return;
      const distance = footprintDoorDistance(footprint, position.x, position.z);
      if (distance >= 0 && distance < bestDistance) {
        bestDistance = distance;
        best = footprint;
      }
    });
    return best;
  }

  function hintNearbyDoor(position) {
    const door = nearestDoor(position, DOOR_HINT_REACH);
    if (!door) return;
    const now = performance.now();
    if (now - (hintedAt.get(door) || -Infinity) < DOOR_HINT_MS) return;
    hintedAt.set(door, now);
    onTerritoryNotice(`${door.link.label}: walk through the door, tap it, or press E to go in`);
  }

  function enterNearbyTerritoryDoor() {
    if (!footprints.length) return false;
    return enterDoor(nearestDoor(ctx.player.position, DOOR_REACH), "key");
  }

  function territoryArrival({ slot = -1, pos = null, portal = false } = {}) {
    if (territory) {
      const local = portal ? portalBorderLocal(territory.place) : pos;
      return local ? territoryArrivalSpawn(local) : null;
    }
    return pos ? forkmeshArrivalSpawn(slot, pos) : null;
  }

  function updateTerritoryBugs(list = []) {
    built?.updateBugs(list);
  }

  return {
    installInstanceTerritory,
    setForkmeshCrossings,
    checkTerritoryCrossing,
    constrainTerritoryObstacles,
    enterNearbyTerritoryDoor,
    territoryArrival,
    updateTerritoryBugs,
    territoryOwnerAt,
    territoryForkmeshShift: () => (shift
      ? {
        ...shift,
        townX: WORLD_HEX_TILES[0].x + shift.x,
        townZ: WORLD_HEX_TILES[0].z + shift.z,
      }
      : null),
    territoryActive: () => territoryWalkActive(),
  };
}
