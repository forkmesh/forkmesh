import {
  CONTINUOUS_CITY_CENTER_Z,
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  WORLD_BIKE_LANE_CENTER_X,
  WORLD_HEX_TILES,
  WORLD_ROAD_WIDTH,
} from "./layout.js";
import { roundedHexagonContains } from "./movement.js";

export const TERRITORY_CENTER = Object.freeze({
  x: WORLD_HEX_TILES[0].x,
  z: WORLD_HEX_TILES[0].z,
});
export const TERRITORY_ARRIVAL_DEPTH = 14;
export const TERRITORY_EDGE_REACH = 150;
const HEX_PITCH = CONTINUOUS_CITY_RADIUS * 2 + WORLD_ROAD_WIDTH;
const EDGE_NORMALS = Object.freeze(
  [0, 1, 2, 3, 4, 5].map((side) =>
    Object.freeze({
      x: Math.cos((side * Math.PI) / 3),
      z: Math.sin((side * Math.PI) / 3),
    }),
  ),
);

// Where ForkMesh's World draws a directory slot's single hexagon. Slots 0-4
// are petals of ForkMesh's town: slot n takes desert petal n + 1 (at 60°·(n+1);
// petal 0 holds the beach) and shares an edge with the town. The rest keep
// the outer ring. This must match NEIGHBOUR_HEX_PETALS in ForkMesh's
// scene/layout.js so both Worlds agree on where the border is.
export const TERRITORY_PETALS = Object.freeze([1, 2, 3, 4, 5]);
export const TERRITORY_SLOTS = Object.freeze(
  [2, 4, 6, 8, 10, 11, 0, 2, 4, 6, 8, 10].map((step, slot) => {
    const angle = (step * Math.PI) / 6;
    const petal = slot < TERRITORY_PETALS.length;
    const reach = petal ? HEX_PITCH : HEX_PITCH * (step % 2 ? Math.sqrt(3) : 2);
    return Object.freeze({
      x: WORLD_BIKE_LANE_CENTER_X + Math.cos(angle) * reach,
      z: CONTINUOUS_CITY_CENTER_Z + Math.sin(angle) * reach,
      normalX: Math.cos(angle),
      normalZ: Math.sin(angle),
    });
  }),
);

// Where the hub says this World stands: the desert petal it publishes, else
// the directory slot's place.
export function territoryPetalTile(petal) {
  const index = Number(petal);
  if (!Number.isInteger(index) || index < 0 || index > 5) return null;
  const tile = WORLD_HEX_TILES[index + 1];
  return Object.freeze({ x: tile.x, z: tile.z, normalX: tile.normalX, normalZ: tile.normalZ });
}

export function territorySlotTile(slot) {
  if (slot && typeof slot === "object") {
    return Number.isFinite(slot.x) && Number.isFinite(slot.z) ? slot : null;
  }
  const index = Number(slot);
  return Number.isInteger(index) ? TERRITORY_SLOTS[index] || null : null;
}

// How far ForkMesh's own World (town at its usual place) moves to sit beside
// this World, which is drawn at the centre.
export function forkmeshShiftAround(slot) {
  const home = territorySlotTile(slot);
  if (!home) return null;
  return { x: TERRITORY_CENTER.x - home.x, z: TERRITORY_CENTER.z - home.z };
}

export function forkmeshTilesAround(slot) {
  const home = territorySlotTile(slot);
  if (!home) return [];
  // A petal slot stands where one of the town's desert petals would be; that
  // petal is the instance itself, so it is not drawn as ForkMesh's.
  return WORLD_HEX_TILES.flatMap((tile, index) => {
    const dx = tile.x - home.x;
    const dz = tile.z - home.z;
    const distance = Math.hypot(dx, dz);
    if (distance < 1) return [];
    return [{
      index,
      center: index === 0,
      x: TERRITORY_CENTER.x + dx,
      z: TERRITORY_CENTER.z + dz,
      adjacent: Math.abs(distance - HEX_PITCH) < 1,
      normalX: dx / distance,
      normalZ: dz / distance,
    }];
  });
}

export function forkmeshOfficeAround(slot, office = { x: 0, z: -207 }) {
  const home = territorySlotTile(slot);
  if (!home) return null;
  return {
    x: TERRITORY_CENTER.x + Number(office.x) - home.x,
    z: TERRITORY_CENTER.z + Number(office.z) - home.z,
  };
}

export function hexLandContains(x, z, tile, margin = 0) {
  return roundedHexagonContains(
    Number(x),
    Number(z),
    tile.x,
    tile.z,
    CONTINUOUS_CITY_RADIUS,
    CONTINUOUS_CITY_CORNER_RADIUS,
    margin,
  );
}

export function hexRoadContains(x, z, tile, margin = 0) {
  return roundedHexagonContains(
    Number(x),
    Number(z),
    tile.x,
    tile.z,
    CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH,
    CONTINUOUS_CITY_CORNER_RADIUS + WORLD_ROAD_WIDTH,
    margin,
  );
}

export function crossedInto(previous, current, tiles = []) {
  if (!previous || !current) return null;
  for (let index = 0; index < tiles.length; index += 1) {
    const tile = tiles[index];
    if (
      hexLandContains(current.x, current.z, tile) &&
      !hexLandContains(previous.x, previous.z, tile)
    ) {
      return tile;
    }
  }
  return null;
}

export function nearestEdgeNormal(localX, localZ) {
  let best = EDGE_NORMALS[0];
  let bestDot = -Infinity;
  EDGE_NORMALS.forEach((normal) => {
    const dot = localX * normal.x + localZ * normal.z;
    if (dot > bestDot) {
      bestDot = dot;
      best = normal;
    }
  });
  return best;
}

export function headingToward(dx, dz) {
  return Math.atan2(-dx, -dz);
}

export function edgeArrival(local, side = "inside") {
  const x = Number(local?.x);
  const z = Number(local?.z);
  if (!Number.isFinite(x) || !Number.isFinite(z)) return null;
  const normal = nearestEdgeNormal(x, z);
  const along = Math.max(
    -TERRITORY_EDGE_REACH,
    Math.min(TERRITORY_EDGE_REACH, -x * normal.z + z * normal.x),
  );
  const depth =
    side === "inside"
      ? CONTINUOUS_CITY_RADIUS - TERRITORY_ARRIVAL_DEPTH
      : CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH + TERRITORY_ARRIVAL_DEPTH;
  const facing = side === "inside" ? -1 : 1;
  return {
    x: normal.x * depth - normal.z * along,
    z: normal.z * depth + normal.x * along,
    heading: headingToward(normal.x * facing, normal.z * facing),
    normalX: normal.x,
    normalZ: normal.z,
  };
}

export function territoryArrivalSpawn(pos) {
  const spot = edgeArrival(pos, "inside");
  if (!spot) return null;
  return {
    x: TERRITORY_CENTER.x + spot.x,
    z: TERRITORY_CENTER.z + spot.z,
    heading: spot.heading,
    faceCamera: true,
    space: "town-square",
  };
}

export function forkmeshArrivalSpawn(slot, pos) {
  const tile = territorySlotTile(slot);
  const spot = tile ? edgeArrival(pos, "outside") : null;
  if (!spot) return null;
  return {
    x: tile.x + spot.x,
    z: tile.z + spot.z,
    heading: spot.heading,
    faceCamera: true,
    space: "town-square",
  };
}

export function portalBorderLocal(slot) {
  const home = territorySlotTile(slot);
  if (!home) return null;
  const tiles = forkmeshTilesAround(slot).filter((tile) => tile.adjacent);
  if (!tiles.length) return null;
  const normalX = tiles.reduce((sum, tile) => sum + tile.normalX, 0);
  const normalZ = tiles.reduce((sum, tile) => sum + tile.normalZ, 0);
  const length = Math.hypot(normalX, normalZ) || 1;
  const normal = nearestEdgeNormal(normalX / length, normalZ / length);
  return {
    x: normal.x * (CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH / 2),
    z: normal.z * (CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH / 2),
  };
}

// Territory buildings collide like the town office: a footprint is a box in
// the plot's own frame (its +z face looks at the plaza), either solid (a
// house) or hollow with glass walls (the terrarium). Each one has a door on
// its +z face. A hollow box lets walkers through its door; a solid one turns a
// walk into its door into a "door" result the caller can act on.
export const TERRITORY_WALL_THICKNESS = 0.3;
const COLLISION_STEP = 0.35;

export function territoryFootprint({
  x = 0, z = 0, rotation = 0, width = 1, depth = 1,
  hollow = false, door = 0, link = null,
} = {}) {
  return {
    x: Number(x) || 0,
    z: Number(z) || 0,
    rotation: Number(rotation) || 0,
    cos: Math.cos(Number(rotation) || 0),
    sin: Math.sin(Number(rotation) || 0),
    halfWidth: Math.max(0.5, Number(width) || 1) / 2,
    halfDepth: Math.max(0.5, Number(depth) || 1) / 2,
    hollow: Boolean(hollow),
    doorHalf: Math.max(0, Number(door) || 0) / 2,
    link,
  };
}

// three.js turns a child's local +z towards (sin r, cos r); this undoes it.
export function footprintLocal(footprint, x, z) {
  const dx = x - footprint.x;
  const dz = z - footprint.z;
  return {
    x: dx * footprint.cos - dz * footprint.sin,
    z: dx * footprint.sin + dz * footprint.cos,
  };
}

export function footprintWorld(footprint, localX, localZ) {
  return {
    x: footprint.x + localX * footprint.cos + localZ * footprint.sin,
    z: footprint.z - localX * footprint.sin + localZ * footprint.cos,
  };
}

function boxHit(localX, localZ, minX, maxX, minZ, maxZ, radius) {
  return (
    localX > minX - radius && localX < maxX + radius &&
    localZ > minZ - radius && localZ < maxZ + radius
  );
}

export function footprintBlocks(footprint, localX, localZ, radius = 0) {
  const w = footprint.halfWidth;
  const d = footprint.halfDepth;
  if (!footprint.hollow) return boxHit(localX, localZ, -w, w, -d, d, radius);
  const t = TERRITORY_WALL_THICKNESS / 2;
  const door = footprint.doorHalf;
  return (
    boxHit(localX, localZ, -w, w, -d - t, -d + t, radius) ||
    boxHit(localX, localZ, -w - t, -w + t, -d, d, radius) ||
    boxHit(localX, localZ, w - t, w + t, -d, d, radius) ||
    boxHit(localX, localZ, -w, -door, d - t, d + t, radius) ||
    boxHit(localX, localZ, door, w, d - t, d + t, radius)
  );
}

function pathBlocked(footprint, from, to, radius) {
  const length = Math.hypot(to.x - from.x, to.z - from.z);
  const steps = Math.max(1, Math.ceil(length / COLLISION_STEP));
  for (let step = 1; step <= steps; step += 1) {
    const t = step / steps;
    const x = from.x + (to.x - from.x) * t;
    const z = from.z + (to.z - from.z) * t;
    if (footprintBlocks(footprint, x, z, radius)) return { x, z };
  }
  return null;
}

// Moves a walker from `previous` to `current` past every footprint. A blocked
// move slides along the wall it met (keeping the free axis of the building's
// own frame); a walker already inside a wall is let out rather than pinned.
export function constrainToFootprints(footprints, previous, current, radius = 0) {
  let x = Number(current?.x);
  let z = Number(current?.z);
  let door = null;
  let blocked = false;
  if (!Number.isFinite(x) || !Number.isFinite(z) || !previous) {
    return { x, z, blocked, door };
  }
  for (const footprint of footprints || []) {
    if (!footprint) continue;
    const from = footprintLocal(footprint, previous.x, previous.z);
    if (Math.max(Math.abs(from.x) - footprint.halfWidth,
      Math.abs(from.z) - footprint.halfDepth) > 40) continue;
    const to = footprintLocal(footprint, x, z);
    if (footprintBlocks(footprint, from.x, from.z, radius)) continue;
    const hit = pathBlocked(footprint, from, to, radius);
    if (!hit) continue;
    blocked = true;
    if (
      !footprint.hollow &&
      footprint.doorHalf > 0 &&
      Math.abs(hit.x) <= footprint.doorHalf &&
      from.z >= footprint.halfDepth + radius - 1e-6 &&
      to.z < from.z
    ) {
      door = footprint;
    }
    let local = { x: to.x, z: from.z };
    if (pathBlocked(footprint, from, local, radius)) {
      local = { x: from.x, z: to.z };
      if (pathBlocked(footprint, from, local, radius)) local = from;
    }
    ({ x, z } = footprintWorld(footprint, local.x, local.z));
  }
  return { x, z, blocked, door };
}

export function footprintDoorDistance(footprint, x, z) {
  const local = footprintLocal(footprint, x, z);
  return Math.hypot(
    Math.max(0, Math.abs(local.x) - footprint.doorHalf),
    local.z - footprint.halfDepth,
  );
}
