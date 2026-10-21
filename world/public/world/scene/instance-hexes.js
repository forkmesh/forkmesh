import { makeLabelSprite } from "./labels.js";
import {
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  DESERT_PLOT_SURFACE_Y,
  INSTANCE_HEX_SLOTS,
  WORLD_MESH_FLOWER_HEXES,
  WORLD_ROAD_CORNER_SEGMENTS,
  WORLD_ROAD_JUNCTION_REACH,
  WORLD_ROAD_SURFACE_Y,
  WORLD_MESH_HEX_SHAPE,
  WORLD_ROAD_WIDTH,
  worldMeshHexesFor,
} from "./layout.js";
import { makeSharedMaterial } from "./materials.js";
import { hexCorners, roundedHexagonOutline } from "./movement.js";
import { setShadows } from "./object-utils.js";
import {
  DESERT_SAND_TILE,
  desertSandTexture,
  roundedHexagonGeometry,
} from "./town.js";
import * as resources from "./resources.js";
import { neighbourFootprint, setNeighbourFootprints } from "./neighbour-footprints.js";
import { loadNeighbourScene, neighbourIconURL } from "../world-neighbour-scene.js";

export const INSTANCE_LOGO_PATH =
  /^(?:https:\/\/[a-z0-9.-]+(?::\d{1,5})?)?\/api\/start\/instances\/[0-9a-f]{32}\/logo\?v=\d+$/;
const LOGO_TEXTURE_SIZE = 256;
const GROUND_LOGO_SIZE = 150;
const LANDMARK_INSET = CONTINUOUS_CITY_RADIUS * 0.55;
const MESH_ROAD_DROP = 0.02;
const CIRCUIT_TEXTURE_SIZE = 256;
const CIRCUIT_REPEAT = 5;

export function instanceHexArrival(slot) {
  const tile = instanceHexSlot(slot);
  if (!tile) return null;
  const reach = LANDMARK_INSET + 18;
  return {
    x: tile.x - tile.normalX * reach,
    z: tile.z - tile.normalZ * reach,
    heading: Math.atan2(tile.normalX, tile.normalZ),
    space: "town-square",
  };
}

export function instanceHexSlot(slot) {
  const index = Number(slot);
  return Number.isInteger(index) ? INSTANCE_HEX_SLOTS[index] || null : null;
}

function roadRingsGeometry(THREE, rings) {
  const outer = roundedHexagonOutline(
    CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH,
    CONTINUOUS_CITY_CORNER_RADIUS + WORLD_ROAD_WIDTH,
    WORLD_ROAD_CORNER_SEGMENTS,
  );
  const inner = roundedHexagonOutline(
    CONTINUOUS_CITY_RADIUS,
    CONTINUOUS_CITY_CORNER_RADIUS,
    WORLD_ROAD_CORNER_SEGMENTS,
  );
  const positions = [];
  const normals = [];
  const indices = [];
  const count = outer.length;
  const fanSegments = 12;
  rings.forEach(({ cx, cy, corners }) => {
    const base = positions.length / 3;
    for (let index = 0; index < count; index += 1) {
      positions.push(cx + inner[index][0], cy + inner[index][1], 0);
      positions.push(cx + outer[index][0], cy + outer[index][1], 0);
      normals.push(0, 0, 1, 0, 0, 1);
      const next = (index + 1) % count;
      indices.push(base + index * 2, base + index * 2 + 1, base + next * 2 + 1);
      indices.push(base + index * 2, base + next * 2 + 1, base + next * 2);
    }
    corners.forEach((corner) => {
      const fan = positions.length / 3;
      positions.push(corner.x, corner.y, 0);
      normals.push(0, 0, 1);
      for (let step = 0; step < fanSegments; step += 1) {
        const angle = (step * Math.PI * 2) / fanSegments;
        positions.push(
          corner.x + Math.cos(angle) * WORLD_ROAD_JUNCTION_REACH,
          corner.y + Math.sin(angle) * WORLD_ROAD_JUNCTION_REACH,
          0,
        );
        normals.push(0, 0, 1);
        indices.push(fan, fan + 1 + step, fan + 1 + ((step + 1) % fanSegments));
      }
    });
  });
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.Float32BufferAttribute(positions, 3));
  geometry.setAttribute("normal", new THREE.Float32BufferAttribute(normals, 3));
  geometry.setIndex(indices);
  geometry.computeBoundingSphere();
  return geometry;
}

export function hexRoadRingGeometry(THREE, corners = []) {
  return roadRingsGeometry(THREE, [{ cx: 0, cy: 0, corners }]);
}

function flowerRoadGeometry(THREE, hexes = WORLD_MESH_FLOWER_HEXES) {
  return roadRingsGeometry(
    THREE,
    hexes.map((hex) => ({
      cx: hex.x,
      cy: -hex.z,
      corners: hexCorners(hex).map((point) => ({ x: point.x, y: -point.z })),
    })),
  );
}

function initialTexture(THREE, name) {
  const canvas = document.createElement("canvas");
  canvas.width = LOGO_TEXTURE_SIZE;
  canvas.height = LOGO_TEXTURE_SIZE;
  const context = canvas.getContext("2d");
  context.fillStyle = "#ececef";
  context.fillRect(0, 0, LOGO_TEXTURE_SIZE, LOGO_TEXTURE_SIZE);
  context.fillStyle = "#111113";
  context.font = `700 ${LOGO_TEXTURE_SIZE * 0.55}px system-ui, sans-serif`;
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillText(
    String(name || "?").trim().charAt(0).toUpperCase() || "?",
    LOGO_TEXTURE_SIZE / 2,
    LOGO_TEXTURE_SIZE / 2 + LOGO_TEXTURE_SIZE * 0.04,
  );
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

export function loadInstanceLogo(THREE, url, apply) {
  if (!INSTANCE_LOGO_PATH.test(String(url || ""))) return;
  const image = new Image();
  image.decoding = "async";
  if (/^https:/.test(String(url))) image.crossOrigin = "anonymous";
  image.onload = () => {
    const width = image.naturalWidth || LOGO_TEXTURE_SIZE;
    const height = image.naturalHeight || LOGO_TEXTURE_SIZE;
    const scale = Math.min(LOGO_TEXTURE_SIZE / width, LOGO_TEXTURE_SIZE / height);
    const canvas = document.createElement("canvas");
    canvas.width = LOGO_TEXTURE_SIZE;
    canvas.height = LOGO_TEXTURE_SIZE;
    canvas.getContext("2d").drawImage(
      image,
      (LOGO_TEXTURE_SIZE - width * scale) / 2,
      (LOGO_TEXTURE_SIZE - height * scale) / 2,
      width * scale,
      height * scale,
    );
    const texture = new THREE.CanvasTexture(canvas);
    texture.colorSpace = THREE.SRGBColorSpace;
    apply(texture);
  };
  image.src = url;
}

export function createInstanceHex(THREE, instance, interactive, home = { x: 0, z: 0 }) {
  const centerX = Number(instance?.x);
  const centerZ = Number(instance?.z);
  if (!Number.isFinite(centerX) || !Number.isFinite(centerZ)) return null;
  const group = new THREE.Group();
  group.name = `forkmesh-instance-hex-${instance.id}`;
  group.position.set(centerX, 0, centerZ);
  const towardX = Number(home.x) - centerX;
  const towardZ = Number(home.z) - centerZ;
  const toward = Math.hypot(towardX, towardZ) || 1;
  const facingX = towardX / toward;
  const facingZ = towardZ / toward;

  const ground = new THREE.Mesh(
    resources.geometry(THREE, "instance-hex-ground", () =>
      roundedHexagonGeometry(
        THREE,
        CONTINUOUS_CITY_RADIUS,
        CONTINUOUS_CITY_CORNER_RADIUS,
        WORLD_ROAD_CORNER_SEGMENTS,
      ),
    ),
    makeSharedMaterial(THREE, "#9fc584", { roughness: 1, metalness: 0 }),
  );
  ground.name = "forkmesh-instance-hex-ground";
  ground.rotation.x = -Math.PI / 2;
  ground.position.y = DESERT_PLOT_SURFACE_Y;
  ground.receiveShadow = true;
  ground.userData.ground = true;
  group.add(ground);

  const single = instance.shape === WORLD_MESH_HEX_SHAPE;
  if (!single) {
    const sand = new THREE.Mesh(
      resources.geometry(THREE, "world-mesh-flower-sand", () =>
        roundedHexagonGeometry(
          THREE,
          CONTINUOUS_CITY_RADIUS,
          CONTINUOUS_CITY_CORNER_RADIUS,
          WORLD_ROAD_CORNER_SEGMENTS,
          {
            centers: WORLD_MESH_FLOWER_HEXES.slice(1).map((hex) => [hex.x, -hex.z]),
            uvScale: 1 / DESERT_SAND_TILE,
          },
        ),
      ),
      resources.material(THREE, {
        color: "#f2dfb8",
        map: desertSandTexture(THREE),
        roughness: 1,
        metalness: 0,
      }, "MeshStandardMaterial"),
    );
    sand.name = "forkmesh-instance-hex-sand";
    sand.rotation.x = -Math.PI / 2;
    sand.position.y = DESERT_PLOT_SURFACE_Y;
    sand.receiveShadow = true;
    sand.userData.ground = true;
    group.add(sand);
  }

  const road = new THREE.Mesh(
    resources.geometry(THREE, single ? "world-mesh-hex-road" : "world-mesh-flower-road", () =>
      flowerRoadGeometry(THREE, worldMeshHexesFor(instance.shape))),
    makeSharedMaterial(THREE, "#2b3433", { roughness: 0.92, metalness: 0 }),
  );
  road.name = "forkmesh-instance-hex-road";
  road.rotation.x = -Math.PI / 2;
  road.position.y = WORLD_ROAD_SURFACE_Y - MESH_ROAD_DROP;
  road.receiveShadow = true;
  road.userData.ground = true;
  group.add(road);

  const pick = (object) => {
    object.userData.interactive = "instance-hexagon";
    object.userData.url = instance.world;
    object.userData.label = instance.name;
    object.userData.host = instance.host;
    object.userData.worldUrl = instance.worldUrl || "";
    object.userData.arrivalSlot = Number.isInteger(instance.arrivalSlot)
      ? instance.arrivalSlot
      : instance.slot;
    interactive.push(object);
  };

  const faceMaterial = resources.uniqueMaterial(THREE, {
    map: initialTexture(THREE, instance.name),
    toneMapped: false,
  }, "MeshBasicMaterial");
  const groundLogoMaterial = resources.uniqueMaterial(THREE, {
    transparent: true,
    opacity: 0.85,
    depthWrite: false,
    polygonOffset: true,
    polygonOffsetFactor: -2,
    polygonOffsetUnits: -2,
    toneMapped: false,
  }, "MeshBasicMaterial");
  const groundLogo = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", GROUND_LOGO_SIZE, GROUND_LOGO_SIZE),
    groundLogoMaterial,
  );
  groundLogo.name = "forkmesh-instance-hex-logo";
  groundLogo.rotation.x = -Math.PI / 2;
  groundLogo.position.y = DESERT_PLOT_SURFACE_Y + 0.05;
  groundLogo.visible = false;
  group.add(groundLogo);

  const landmark = new THREE.Group();
  landmark.name = "forkmesh-instance-hex-landmark";
  landmark.position.set(
    facingX * LANDMARK_INSET,
    DESERT_PLOT_SURFACE_Y,
    facingZ * LANDMARK_INSET,
  );
  landmark.rotation.y = Math.atan2(facingX, facingZ);
  const stone = makeSharedMaterial(THREE, "#d8d0bf", { roughness: 0.9 });
  const plinth = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 7, 8, 1.2, 6),
    stone,
  );
  plinth.position.y = 0.6;
  landmark.add(plinth);
  const obelisk = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 1.1, 2, 15, 4),
    stone,
  );
  obelisk.position.set(0, 8.7, -2);
  obelisk.rotation.y = Math.PI / 4;
  pick(obelisk);
  landmark.add(obelisk);
  const board = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 9, 9, 0.5),
    makeSharedMaterial(THREE, "#3a3f3e", { roughness: 0.8 }),
  );
  board.position.set(0, 6.4, 1.6);
  pick(board);
  landmark.add(board);
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 8.4, 8.4),
    faceMaterial,
  );
  face.name = "forkmesh-instance-hex-face";
  face.position.set(0, 6.4, 1.86);
  pick(face);
  landmark.add(face);
  setShadows(landmark, true, false);
  group.add(landmark);

  const label = makeLabelSprite(
    THREE,
    String(instance.name || "ForkMesh instance").slice(0, 22),
    instance.worldUrl
      ? `portal · ${String(instance.host || "")}`.slice(0, 32)
      : String(instance.host || "").slice(0, 32),
    "#9ef7c6",
  );
  label.scale.set(15, 5, 1);
  label.position.set(landmark.position.x, 21, landmark.position.z);
  pick(label);
  group.add(label);

  if (single && instance.scene) {
    void loadNeighbourScene(instance.scene).then((scene) => {
      if (!scene || !group.parent?.parent) return;
      ground.material = neighbourGroundMaterial(THREE, scene.ground);
      groundLogo.visible = false;
      groundLogo.userData.sceneHidden = true;
      setNeighbourFootprints(instance.id, neighbourSceneFootprints(scene).map((item) =>
        neighbourFootprint({ ...item, x: centerX + item.x, z: centerZ + item.z })));
      group.add(buildNeighbourScene(THREE, scene, {
        icon: (url) => neighbourIconURL(instance.id, url),
        door: (object, url, label) => {
          if (!url) return;
          object.userData.interactive = "neighbour-door";
          object.userData.link = { url, label };
          interactive.push(object);
        },
        bug: (sprite, entry) => {
          sprite.userData.interactive = "neighbour-bug";
          sprite.userData.bug = { ...entry };
          interactive.push(sprite);
        },
        pick: (object, url, label) => {
          if (!url) return;
          object.userData.interactive = "instance-hexagon";
          object.userData.url = url;
          object.userData.label = label;
          object.userData.host = new URL(url).hostname;
          object.userData.worldUrl = "";
          interactive.push(object);
        },
      }));
    });
  }

  loadInstanceLogo(THREE, instance.logo, (texture) => {
    if (!group.parent) {
      texture.dispose();
      return;
    }
    faceMaterial.map?.dispose?.();
    faceMaterial.map = texture;
    faceMaterial.needsUpdate = true;
    groundLogoMaterial.map = texture;
    groundLogoMaterial.needsUpdate = true;
    groundLogo.visible = !groundLogo.userData.sceneHidden;
  });
  return group;
}

function circuitCanvas(THREE, color, accent, glow) {
  const size = CIRCUIT_TEXTURE_SIZE;
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const context = canvas.getContext("2d");
  context.fillStyle = glow ? "#000000" : color;
  context.fillRect(0, 0, size, size);
  context.strokeStyle = glow ? "#ffffff" : accent;
  context.fillStyle = context.strokeStyle;
  context.globalAlpha = glow ? 0.6 : 0.22;
  context.lineWidth = 2;
  context.lineCap = "round";
  let seed = 7;
  const next = () => {
    seed = (seed * 16807) % 2147483647;
    return seed / 2147483647;
  };
  const grid = size / 8;
  for (let index = 0; index < 10; index += 1) {
    let x = Math.floor(next() * 8) * grid;
    let y = Math.floor(next() * 8) * grid;
    context.beginPath();
    context.moveTo(x, y);
    for (let step = 0; step < 4; step += 1) {
      const run = (1 + Math.floor(next() * 2)) * grid;
      if (step % 2 === 0) x = (x + (next() < 0.5 ? run : -run) + size) % size;
      else y = (y + (next() < 0.5 ? run : -run) + size) % size;
      context.lineTo(x, y);
    }
    context.stroke();
    context.beginPath();
    context.arc(x, y, 4, 0, Math.PI * 2);
    context.fill();
  }
  const texture = new THREE.CanvasTexture(canvas);
  texture.wrapS = THREE.RepeatWrapping;
  texture.wrapT = THREE.RepeatWrapping;
  texture.repeat.set(CIRCUIT_REPEAT, CIRCUIT_REPEAT);
  if (!glow) texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

export function neighbourGroundMaterial(THREE, ground) {
  if (ground.pattern !== "circuit") {
    return makeSharedMaterial(THREE, ground.color, { roughness: 1, metalness: 0 });
  }
  return resources.uniqueMaterial(THREE, {
    color: "#ffffff",
    map: circuitCanvas(THREE, ground.color, ground.accent, false),
    emissive: ground.accent,
    emissiveMap: circuitCanvas(THREE, ground.color, ground.accent, true),
    emissiveIntensity: 0.2,
    roughness: 1,
    metalness: 0,
  }, "MeshStandardMaterial");
}

function iconTexture(THREE, label, url, ring) {
  const size = 96;
  const canvas = document.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const context = canvas.getContext("2d");
  const paint = (image) => {
    context.clearRect(0, 0, size, size);
    context.fillStyle = image ? "#ffffff" : "#33415c";
    context.strokeStyle = ring;
    context.lineWidth = 6;
    context.beginPath();
    if (context.roundRect) context.roundRect(6, 6, size - 12, size - 12, 18);
    else context.rect(6, 6, size - 12, size - 12);
    context.fill();
    context.stroke();
    if (image) {
      context.drawImage(image, 22, 22, size - 44, size - 44);
    } else {
      context.fillStyle = "#ffffff";
      context.font = `800 ${size * 0.45}px system-ui, sans-serif`;
      context.textAlign = "center";
      context.textBaseline = "middle";
      context.fillText(String(label || "?").charAt(0).toUpperCase(), size / 2, size / 2 + 3);
    }
  };
  paint(null);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  if (url) {
    const image = new Image();
    image.decoding = "async";
    image.crossOrigin = "anonymous";
    image.referrerPolicy = "no-referrer";
    image.onload = () => {
      paint(image);
      texture.needsUpdate = true;
    };
    image.src = url;
  }
  return texture;
}

function sceneHouse(THREE, item, pick) {
  const house = new THREE.Group();
  const body = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 14, 9, 12),
    makeSharedMaterial(THREE, "#2f363c", { roughness: 0.8 }),
  );
  body.position.y = 4.5;
  const roof = new THREE.Mesh(
    resources.primitive(THREE, "ConeGeometry", 11, 6, 4),
    makeSharedMaterial(THREE, item.color, { roughness: 0.6 }),
  );
  roof.position.y = 12;
  roof.rotation.y = Math.PI / 4;
  const door = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 3.4, 5.4),
    makeSharedMaterial(THREE, item.color, { emissive: item.color, emissiveIntensity: 0.7 }),
  );
  door.position.set(0, 2.7, 6.02);
  const glass = makeSharedMaterial(THREE, "#ffe9a8", { emissive: "#ffe9a8", emissiveIntensity: 0.9 });
  const windows = [-4.6, 4.6].map((x) => {
    const pane = new THREE.Mesh(resources.primitive(THREE, "PlaneGeometry", 2.6, 2.2), glass);
    pane.position.set(x, 5, 6.02);
    return pane;
  });
  const label = makeLabelSprite(THREE, item.label.slice(0, 22),
    item.url ? new URL(item.url).hostname.slice(0, 32) : "", item.color);
  label.scale.set(15, 5, 1);
  label.position.y = 19;
  house.add(body, roof, door, ...windows, label);
  [body, roof, door, label].forEach((object) => pick(object, item.url, item.label));
  return house;
}

function sceneTerrarium(THREE, item, accent, icon, bug) {
  const tank = new THREE.Group();
  const width = 26;
  const depth = 16;
  const height = 8;
  const soil = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", width, 0.2, depth),
    makeSharedMaterial(THREE, "#3d3126", { roughness: 1 }),
  );
  soil.position.y = 0.1;
  const glassMaterial = makeSharedMaterial(THREE, "#bfe8ff", {
    transparent: true,
    opacity: 0.16,
    roughness: 0.05,
    depthWrite: false,
  });
  const doorWidth = TERRARIUM_DOOR_WIDTH;
  const doorHeight = 5.8;
  const side = (width - doorWidth) / 2;
  const pane = (sx, sy, sz, x, y, z) => {
    const mesh = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", sx, sy, sz), glassMaterial);
    mesh.position.set(x, y, z);
    mesh.renderOrder = 5;
    return mesh;
  };
  tank.add(
    soil,
    pane(width, height, 0.12, 0, height / 2, -depth / 2),
    pane(0.12, height, depth, -width / 2, height / 2, 0),
    pane(0.12, height, depth, width / 2, height / 2, 0),
    pane(side, height, 0.12, -(doorWidth + side) / 2, height / 2, depth / 2),
    pane(side, height, 0.12, (doorWidth + side) / 2, height / 2, depth / 2),
    pane(doorWidth, height - doorHeight, 0.12, 0, doorHeight + (height - doorHeight) / 2, depth / 2),
    pane(width, 0.12, depth, 0, height, 0),
  );
  const frame = makeSharedMaterial(THREE, accent, { emissive: accent, emissiveIntensity: 0.25 });
  [-1, 1].forEach((sx) => {
    const post = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 0.4, doorHeight, 0.45), frame);
    post.position.set((sx * (doorWidth + 0.4)) / 2, doorHeight / 2, depth / 2);
    tank.add(post);
  });
  const lintel = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", doorWidth + 0.8, 0.4, 0.45), frame);
  lintel.position.set(0, doorHeight, depth / 2);
  tank.add(lintel);
  item.bugs.forEach((entry, index) => {
    const column = index % 8;
    const row = Math.floor(index / 8);
    const sprite = new THREE.Sprite(resources.uniqueMaterial(THREE, {
      map: iconTexture(THREE, entry.label, entry.icon ? icon(entry.icon) : "", accent),
      transparent: true,
      depthWrite: false,
    }, "SpriteMaterial"));
    sprite.scale.set(2.2, 2.2, 1);
    sprite.position.set(-width / 2 + 2.5 + column * 3, 1.6 + (index % 3) * 0.5, -depth / 2 + 2.5 + row * 3.2);
    sprite.renderOrder = 6;
    bug(sprite, entry);
    tank.add(sprite);
  });
  if (item.label) {
    const label = makeLabelSprite(THREE, item.label.slice(0, 22), `${item.bugs.length} bugs`, accent);
    label.scale.set(15, 5, 1);
    label.position.y = height + 7;
    tank.add(label);
  }
  return tank;
}

export const NEIGHBOUR_HOUSE = Object.freeze({ width: 14, depth: 12, door: 3.4 });
export const NEIGHBOUR_TERRARIUM = Object.freeze({ width: 26, depth: 16 });
const TERRARIUM_DOOR_WIDTH = 4.4;

export function neighbourSceneFootprints(scene) {
  return (scene?.objects || []).flatMap((item) => {
    const placed = { x: item.x, z: item.z, rotation: item.facing };
    if (item.type === "house") {
      return [{
        ...placed, ...NEIGHBOUR_HOUSE, hollow: false,
        link: item.url ? { url: item.url, label: item.label } : null,
      }];
    }
    if (item.type === "terrarium") {
      return [{ ...placed, ...NEIGHBOUR_TERRARIUM, hollow: true, door: TERRARIUM_DOOR_WIDTH }];
    }
    return [];
  });
}

export function buildNeighbourScene(THREE, scene, { icon = () => "", pick = () => {}, bug = null, door = null } = {}) {
  const group = new THREE.Group();
  group.name = "forkmesh-instance-hex-scene";
  const accent = scene.ground.accent;
  const bugPick = bug || ((sprite, entry) => pick(sprite, entry.url, entry.label));
  scene.objects.forEach((item) => {
    let object = null;
    if (item.type === "house") object = sceneHouse(THREE, item, door || pick);
    else if (item.type === "terrarium") object = sceneTerrarium(THREE, item, accent, icon, bugPick);
    else if (item.type === "sign") {
      object = makeLabelSprite(THREE, item.title.slice(0, 22), item.text.slice(0, 32), accent);
      object.scale.set(16, 5.3, 1);
      object.position.y = 8;
    } else if (item.type === "plaza") {
      object = new THREE.Mesh(
        resources.primitive(THREE, "CircleGeometry", item.radius, 48),
        makeSharedMaterial(THREE, item.color, { roughness: 0.9 }),
      );
      object.rotation.x = -Math.PI / 2;
      object.position.y = 0.06;
    } else if (item.type === "lamp") {
      object = new THREE.Group();
      const pole = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.14, 0.18, 5, 6),
        makeSharedMaterial(THREE, "#3a4045", { roughness: 0.6 }),
      );
      pole.position.y = 2.5;
      const bulb = new THREE.Mesh(
        resources.primitive(THREE, "SphereGeometry", 0.45, 10, 8),
        makeSharedMaterial(THREE, item.color, { emissive: item.color, emissiveIntensity: 1.2 }),
      );
      bulb.position.y = 5.2;
      object.add(pole, bulb);
    } else if (item.type === "tree") {
      object = new THREE.Group();
      const crown = new THREE.Mesh(
        resources.primitive(THREE, "ConeGeometry", 2.2, 5, 7),
        makeSharedMaterial(THREE, "#2f5e46", { roughness: 0.9 }),
      );
      crown.position.y = 5.2;
      object.add(crown);
      object.scale.setScalar(item.scale);
    }
    if (!object) return;
    const holder = object.isSprite ? new THREE.Group().add(object) : object;
    holder.position.x += item.x;
    holder.position.z += item.z;
    holder.rotation.y = item.facing;
    group.add(holder);
  });
  setShadows(group, true, false);
  return group;
}
