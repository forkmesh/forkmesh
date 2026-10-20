import {
  OFFICE_AVATAR_RADIUS,
  OFFICE_ISLAND_CENTER,
  OFFICE_TOWER_HEIGHT,
  officeFloorY,
  officeHexContains,
  officeInteriorPointIsWalkable,
} from "../world-office-tower.js";
import {
  OFFICE_AQUARIUM_BOUNDS,
  officeAquariumLocalPoint,
  QUADCOPTER_CRUISE_ACCELERATION,
  QUADCOPTER_CRUISE_MAX_SPEED,
  QUADCOPTER_HORIZONTAL_SPEED,
  QUADCOPTER_ORBIT_LEVEL_PITCH,
  QUADCOPTER_PARK_CLEARANCE,
  QUADCOPTER_PARK_SEARCH_STEPS,
  ROOF_PARACHUTE_GRAVITY,
  ROOF_PARACHUTE_TERMINAL_VELOCITY,
} from "./gameplay-constants.js";
import {
  BEACH_CENTER_X,
  BEACH_CENTER_Z,
  BEACH_RADIUS,
  BEACH_ROAD_HALF_WIDTH,
  BEACH_ROAD_MAX_X,
  BEACH_ROAD_MIN_X,
  CONTINUOUS_CITY_CENTER_Z,
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  CONTINUOUS_CITY_SURFACE_Y,
  REMOTE_DISTRICT_RADIUS,
  WORLD_BIKE_LANE_APOTHEM,
  WORLD_BIKE_LANE_CENTER_X,
  WORLD_BIKE_LANE_CENTER_Z,
  WORLD_BIKE_LANE_CORNER_RADIUS,
  WORLD_HEX_TILES,
  WORLD_MESH_FLOWER_REACH,
  WORLD_RADIUS,
  WORLD_ROAD_JUNCTIONS,
  WORLD_ROAD_JUNCTION_WALK_RADIUS,
  WORLD_ROAD_WIDTH,
  worldMeshHexesFor,
} from "./layout.js";
import { clamp } from "./object-utils.js";

// The public World has one mainland ground plane plus bounded floor contracts
// for the office interiors. District packages register their own floors at
// install time (see packageSpaces in createWorldScene). Deprecated
// destinations remain invalid spawns.
export const WORLD_SPACE_FLOORS = Object.freeze({
  "town-square": CONTINUOUS_CITY_SURFACE_Y,
  east: CONTINUOUS_CITY_SURFACE_Y,
  central: CONTINUOUS_CITY_SURFACE_Y,
  west: CONTINUOUS_CITY_SURFACE_Y,
  "office-lobby": 0.38,
  "office-marketing": officeFloorY("marketing") + 0.38,
  "office-engineering": officeFloorY("engineering") + 0.38,
  "office-rooftop": officeFloorY("rooftop") + 0.38,
});

// Spaces owned by marketplace district packages. The prefix is the contract
// shared with the relay (WORLD_PACKAGE_SPACE_RE in world/src/world.py) and
// the shell's knownWorldSpace() in world.js.
export function packageSpace(value) {
  return /^pkg-[a-z0-9]+(?:-[a-z0-9]+){1,8}$/.test(String(value || ""));
}

export function officeCampusSpace(value) {
  return String(value || "").startsWith("office-");
}

// `spaces` is the scene's live registry of district-package spaces; module
// scope holds none so that two scenes (or a Node import) never share one.
export function worldRadiusForSpace(value, spaces = null) {
  const district = spaces?.get(String(value || ""));
  if (district) return district.radius;
  if (packageSpace(value) || value === WORLD_MESH_SPACE) return REMOTE_DISTRICT_RADIUS;
  return WORLD_RADIUS;
}

export function worldCeilingForSpace(value, spaces = null) {
  const district = spaces?.get(String(value || ""));
  if (district) return district.ceiling;
  if (packageSpace(value)) return REMOTE_DISTRICT_RADIUS;
  if (officeCampusSpace(value)) return OFFICE_TOWER_HEIGHT + 40;
  return 40;
}

function pointHitsOfficeAquarium(floorId, x, z, radius = 0) {
  if (floorId !== "lobby") return false;
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  const local = officeAquariumLocalPoint(px, pz);
  return (
    local.x >= OFFICE_AQUARIUM_BOUNDS.minX - margin &&
    local.x <= OFFICE_AQUARIUM_BOUNDS.maxX + margin &&
    local.z >= OFFICE_AQUARIUM_BOUNDS.minZ - margin &&
    local.z <= OFFICE_AQUARIUM_BOUNDS.maxZ + margin
  );
}

export function officeScenePointIsWalkable(floorId, x, z, radius, feetY = 0) {
  return (
    officeInteriorPointIsWalkable(floorId, x, z, radius, feetY) &&
    !pointHitsOfficeAquarium(floorId, x, z, radius)
  );
}

export function hexagonSignedDistance(x, z, apothem) {
  let across = Math.abs(z);
  let along = Math.abs(x);
  const fold = Math.min(-0.8660254037844386 * across + 0.5 * along, 0);
  across += 2 * fold * 0.8660254037844386;
  along -= 2 * fold * 0.5;
  const halfEdge = 0.5773502691896258 * apothem;
  across -= clamp(across, -halfEdge, halfEdge);
  along -= apothem;
  return Math.hypot(across, along) * Math.sign(along);
}

export function roundedHexagonContains(
  x,
  z,
  centerX,
  centerZ,
  apothem,
  cornerRadius,
  margin = 0,
) {
  return (
    hexagonSignedDistance(x - centerX, z - centerZ, apothem - cornerRadius) <=
    cornerRadius - margin
  );
}

export function roundedHexagonOutline(apothem, cornerRadius, cornerSegments) {
  const corner = Math.min(Math.max(0, cornerRadius), apothem);
  const innerCircumradius = ((apothem - corner) * 2) / Math.sqrt(3);
  const segments = Math.max(1, Math.round(cornerSegments));
  const outline = [];
  for (let vertex = 0; vertex < 6; vertex += 1) {
    const vertexAngle = Math.PI / 6 + (vertex * Math.PI) / 3;
    const cornerX = Math.cos(vertexAngle) * innerCircumradius;
    const cornerY = Math.sin(vertexAngle) * innerCircumradius;
    for (let step = 0; step <= segments; step += 1) {
      const angle = vertexAngle - Math.PI / 6 + (step * Math.PI) / 3 / segments;
      outline.push([
        cornerX + Math.cos(angle) * corner,
        cornerY + Math.sin(angle) * corner,
      ]);
    }
  }
  return outline;
}

export function cityWalkSurfaceContains(x, z, radius = OFFICE_AVATAR_RADIUS) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  return roundedHexagonContains(
    px,
    pz,
    WORLD_BIKE_LANE_CENTER_X,
    CONTINUOUS_CITY_CENTER_Z,
    CONTINUOUS_CITY_RADIUS,
    CONTINUOUS_CITY_CORNER_RADIUS,
    margin,
  );
}

export const WORLD_MESH_SPACE = "world-mesh";

let worldMeshFlowers = [];
let worldMeshHexes = [];
let worldMeshJunctions = [];
let worldMeshPetals = new Set();

export function hexCorners(center) {
  const reach = (WORLD_BIKE_LANE_APOTHEM * 2) / Math.sqrt(3);
  return [0, 1, 2, 3, 4, 5].map((corner) => {
    const angle = Math.PI / 6 + (corner * Math.PI) / 3;
    return { x: center.x + Math.cos(angle) * reach, z: center.z + Math.sin(angle) * reach };
  });
}

export function setWorldMeshFlowers(flowers = []) {
  worldMeshFlowers = (Array.isArray(flowers) ? flowers : []).filter(
    (flower) => flower && Number.isFinite(flower.x) && Number.isFinite(flower.z),
  );
  worldMeshHexes = worldMeshFlowers.flatMap((flower) =>
    worldMeshHexesFor(flower.shape).map((hex) => ({ x: flower.x + hex.x, z: flower.z + hex.z })),
  );
  worldMeshJunctions = worldMeshHexes.flatMap(hexCorners);
  // A single hexagon standing on one of the town's desert petals owns that
  // petal: the town no longer claims the ground underneath it.
  worldMeshPetals = new Set(
    worldMeshFlowers
      .filter((flower) => flower.shape === "hex")
      .map((flower) => WORLD_HEX_TILES.findIndex((tile, index) =>
        index > 0 && Math.hypot(tile.x - flower.x, tile.z - flower.z) < 1))
      .filter((index) => index > 0),
  );
}

export function worldMeshReplacedPetals() {
  return [...worldMeshPetals].map((index) => index - 1);
}

function grownHexContains(px, pz, tile, margin) {
  return roundedHexagonContains(
    px,
    pz,
    tile.x,
    tile.z,
    CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH,
    CONTINUOUS_CITY_CORNER_RADIUS + WORLD_ROAD_WIDTH,
    margin,
  );
}

function junctionContains(px, pz, junctions, margin) {
  for (let index = 0; index < junctions.length; index += 1) {
    const junction = junctions[index];
    if (
      Math.hypot(px - junction.x, pz - junction.z) <=
      WORLD_ROAD_JUNCTION_WALK_RADIUS - margin
    ) {
      return true;
    }
  }
  return false;
}

export function worldMeshFlowerAt(x, z, radius = 0) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return null;
  for (let index = 0; index < worldMeshFlowers.length; index += 1) {
    const flower = worldMeshFlowers[index];
    if (Math.hypot(px - flower.x, pz - flower.z) > WORLD_MESH_FLOWER_REACH) continue;
    const hexes = worldMeshHexesFor(flower.shape);
    for (let hex = 0; hex < hexes.length; hex += 1) {
      const tile = {
        x: flower.x + hexes[hex].x,
        z: flower.z + hexes[hex].z,
      };
      if (grownHexContains(px, pz, tile, margin)) return flower;
    }
  }
  return null;
}

export function worldMeshOwnerAt(x, z) {
  const px = Number(x);
  const pz = Number(z);
  if (!Number.isFinite(px) || !Number.isFinite(pz) || !worldMeshFlowers.length) return null;
  for (let index = 0; index < WORLD_HEX_TILES.length; index += 1) {
    if (worldMeshPetals.has(index)) continue;
    const tile = WORLD_HEX_TILES[index];
    if (
      roundedHexagonContains(
        px,
        pz,
        tile.x,
        tile.z,
        CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH / 2,
        CONTINUOUS_CITY_CORNER_RADIUS + WORLD_ROAD_WIDTH / 2,
      )
    ) {
      return null;
    }
  }
  return worldMeshFlowerAt(px, pz);
}

export function desertWalkSurfaceContains(x, z, radius = OFFICE_AVATAR_RADIUS) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  const bound = WORLD_RADIUS - margin;
  if (Math.abs(px) <= bound && Math.abs(pz) <= bound) {
    for (let index = 0; index < WORLD_HEX_TILES.length; index += 1) {
      if (grownHexContains(px, pz, WORLD_HEX_TILES[index], margin)) return true;
    }
    if (junctionContains(px, pz, WORLD_ROAD_JUNCTIONS, margin)) return true;
  }
  for (let index = 0; index < worldMeshHexes.length; index += 1) {
    if (grownHexContains(px, pz, worldMeshHexes[index], margin)) return true;
  }
  return junctionContains(px, pz, worldMeshJunctions, margin);
}

export function beachWalkSurfaceContains(x, z, radius = OFFICE_AVATAR_RADIUS) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  if (
    px >= BEACH_ROAD_MIN_X + margin &&
    px <= BEACH_ROAD_MAX_X - margin &&
    Math.abs(pz - BEACH_CENTER_Z) <= BEACH_ROAD_HALF_WIDTH - margin
  ) {
    return true;
  }
  return (
    Math.hypot(px - BEACH_CENTER_X, pz - BEACH_CENTER_Z) <=
    BEACH_RADIUS - margin
  );
}

function worldWalkSurfaceContains(x, z, radius = OFFICE_AVATAR_RADIUS) {
  return (
    cityWalkSurfaceContains(x, z, radius) ||
    desertWalkSurfaceContains(x, z, radius) ||
    beachWalkSurfaceContains(x, z, radius)
  );
}

export function quadcopterFlightPitch(cameraMode, orbitPitch, firstPersonPitch) {
  const pitch =
    cameraMode === "first-person"
      ? firstPersonPitch
      : orbitPitch - QUADCOPTER_ORBIT_LEVEL_PITCH;
  return clamp(pitch, -Math.PI / 2, Math.PI / 2);
}

export function quadcopterFlightVector(target, yaw, pitch, forward, right) {
  const level = Math.cos(pitch);
  target.x = -Math.sin(yaw) * level * forward + Math.cos(yaw) * right;
  target.y = -Math.sin(pitch) * forward;
  target.z = -Math.cos(yaw) * level * forward - Math.sin(yaw) * right;
  return target;
}

export function parachuteSinkVelocity(velocity, delta) {
  return Math.max(
    ROOF_PARACHUTE_TERMINAL_VELOCITY,
    velocity - ROOF_PARACHUTE_GRAVITY * delta,
  );
}

export function quadcopterCruiseSpeed(heldSeconds) {
  return Math.min(
    QUADCOPTER_CRUISE_MAX_SPEED,
    QUADCOPTER_HORIZONTAL_SPEED +
      QUADCOPTER_CRUISE_ACCELERATION * Math.max(0, heldSeconds),
  );
}

export function parkedQuadcopterPoint(
  x,
  z,
  {
    officeX = OFFICE_ISLAND_CENTER[0],
    officeZ = OFFICE_ISLAND_CENTER[2],
    clearance = QUADCOPTER_PARK_CLEARANCE,
    walkSurfaceContains = worldWalkSurfaceContains,
  } = {},
) {
  const reachable = (pointX, pointZ) =>
    walkSurfaceContains(pointX, pointZ, clearance) &&
    !officeHexContains(pointX - officeX, pointZ - officeZ, -clearance);
  if (reachable(x, z)) return { x, z };
  for (let step = 1; step <= QUADCOPTER_PARK_SEARCH_STEPS; step += 1) {
    const progress = step / QUADCOPTER_PARK_SEARCH_STEPS;
    const nextX = x + (WORLD_BIKE_LANE_CENTER_X - x) * progress;
    const nextZ = z + (CONTINUOUS_CITY_CENTER_Z - z) * progress;
    if (reachable(nextX, nextZ)) return { x: nextX, z: nextZ };
  }
  return { x: WORLD_BIKE_LANE_CENTER_X, z: CONTINUOUS_CITY_CENTER_Z };
}

export function roundedHexagonLoopLength(
  apothem = WORLD_BIKE_LANE_APOTHEM,
  cornerRadius = WORLD_BIKE_LANE_CORNER_RADIUS,
) {
  const edge = ((apothem - cornerRadius) * 2) / Math.sqrt(3);
  return 6 * edge + 2 * Math.PI * cornerRadius;
}

export function roundedHexagonLoopPoint(
  distance,
  target = {},
  {
    centerX = WORLD_BIKE_LANE_CENTER_X,
    centerZ = WORLD_BIKE_LANE_CENTER_Z,
    apothem = WORLD_BIKE_LANE_APOTHEM,
    cornerRadius = WORLD_BIKE_LANE_CORNER_RADIUS,
  } = {},
) {
  const edge = ((apothem - cornerRadius) * 2) / Math.sqrt(3);
  const arc = (cornerRadius * Math.PI) / 3;
  const side = edge + arc;
  const total = 6 * side;
  const raw = Number(distance) || 0;
  const along = (((raw + edge / 2) % total) + total) % total;
  const index = Math.min(5, Math.floor(along / side));
  const rest = along - index * side;
  const normal = (index * Math.PI) / 3;
  let x;
  let z;
  let heading;
  if (rest < edge) {
    const cornerAngle = normal - Math.PI / 6;
    x =
      Math.cos(cornerAngle) * edge +
      Math.cos(normal) * cornerRadius -
      Math.sin(normal) * rest;
    z =
      Math.sin(cornerAngle) * edge +
      Math.sin(normal) * cornerRadius +
      Math.cos(normal) * rest;
    heading = normal;
  } else {
    const cornerAngle = normal + Math.PI / 6;
    heading = normal + (rest - edge) / Math.max(cornerRadius, 1e-6);
    x = Math.cos(cornerAngle) * edge + Math.cos(heading) * cornerRadius;
    z = Math.sin(cornerAngle) * edge + Math.sin(heading) * cornerRadius;
  }
  target.x = centerX + x;
  target.z = centerZ + z;
  target.heading = Math.atan2(Math.sin(heading), -Math.cos(heading));
  return target;
}

export function circularRideCameraYaw(
  currentYaw,
  previousHeading,
  nextHeading,
) {
  const yaw = Number(currentYaw);
  const previous = Number(previousHeading);
  const next = Number(nextHeading);
  if (
    !Number.isFinite(yaw) ||
    !Number.isFinite(previous) ||
    !Number.isFinite(next)
  ) {
    return Number.isFinite(yaw)
      ? Math.atan2(Math.sin(yaw), Math.cos(yaw))
      : 0;
  }
  const headingDelta = Math.atan2(
    Math.sin(next - previous),
    Math.cos(next - previous),
  );
  return Math.atan2(
    Math.sin(yaw + headingDelta),
    Math.cos(yaw + headingDelta),
  );
}

export const MOVEMENT_KEYS = new Set([
  "KeyW",
  "KeyA",
  "KeyS",
  "KeyD",
  "ArrowUp",
  "ArrowDown",
  "ArrowLeft",
  "ArrowRight",
]);
export const SPRINT_KEYS = new Set(["ShiftLeft", "ShiftRight"]);
