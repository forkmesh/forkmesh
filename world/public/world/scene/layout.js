import {
  OFFICE_BRIDGE_WIDTH,
  OFFICE_DEPTH,
} from "../world-office-tower.js";
import { hashNumber } from "../world-avatar-face.js";

export const WORLD_RADIUS = 1900;
// Marketplace district packages may sit far outside the town simulation
// radius. Their spaces are namespaced "pkg-<package-id>-<floor>"; a space the
// package registers carries its own radius, and one no installed package owns
// (a peer visiting a district this device has not installed) keeps the relay's
// finite envelope so the avatar stays far away instead of snapping into town.
export const REMOTE_DISTRICT_RADIUS = 16_000;
export const CONTINUOUS_CITY_RADIUS = 365;
export const CONTINUOUS_CITY_CENTER_Z = -45;
export const CONTINUOUS_CITY_CORNER_RADIUS = 70;
export const BEACH_CENTER_X = 520;
export const BEACH_CENTER_Z = -80;
export const BEACH_RADIUS = 76;
export const BEACH_ROAD_MIN_X = 225;
export const BEACH_ROAD_MAX_X = BEACH_CENTER_X - BEACH_RADIUS + 12;
export const BEACH_ROAD_HALF_WIDTH = 14;
export const BEACH_TOWN_PORTAL_X = 330;
export const BEACH_SCENE_PORTAL_X = BEACH_CENTER_X - BEACH_RADIUS + 7;
export const REPOSITORY_ISLAND_CENTER_X = 130;
export const REPOSITORY_ISLAND_RING_RADIUS = 31;
// The satellite districts are joined by broad landscaped causeways, not
// narrow bridges. These same dimensions drive both geometry and movement so
// the visible land and the avatar's walkable surface cannot drift apart.
export const REPOSITORY_CONNECTION_MIN_X = 78;
export const REPOSITORY_CONNECTION_MAX_X = 103;
export const LEADERBOARD_ISLAND_CENTER_X = -130;
export const LEADERBOARD_CONNECTION_MIN_X = -103;
export const LEADERBOARD_CONNECTION_MAX_X = -78;
export const LEADERBOARD_DISTRICT_GROUND_RADIUS = 56;
export const BILLBOARD_RING = Object.freeze({
  minRadius: 38,
  groundMargin: 5,
  entryGap: 12,
  gap: 3,
  rimWidth: 0.35,
  color: "#8f96a8",
  accent: "#77d9ff",
  muted: "#2f6d84",
  sections: Object.freeze(["hub", "social", "leaderboards"]),
  padRadius: 2.2,
  padExit: 0.9,
  padGap: 0.25,
  cooldownMs: 60_000,
});
export const BILLBOARD_CIRCLE_GROUND_Y = 0.05;
export const BILLBOARD_CIRCLE_SURFACE_Y = BILLBOARD_CIRCLE_GROUND_Y + 0.07;
export const MEMBER_ISLAND_CENTER_Z = 130;
// Carry the south promenade beneath the Members Circle instead of stopping at
// its edge. The slightly raised dirt disk hides the final stretch, so visitors
// see one route meeting the clearing without a pavement/dirt seam.
export const MEMBER_PATH_END_Z = MEMBER_ISLAND_CENTER_Z;
// Every town promenade is cut from the same slab. Keeping its width, depth,
// and center plane in one place prevents adjacent segments from producing the
// doubled edges and hairline height changes that are especially visible from
// the zoomed-out camera.
export const WORLD_PATH_WIDTH = OFFICE_BRIDGE_WIDTH;
export const WORLD_PATH_HEIGHT = 0.08;
const WORLD_PATH_SURFACE_Y = 0.105;
export const WORLD_PATH_CENTER_Y =
  WORLD_PATH_SURFACE_Y - WORLD_PATH_HEIGHT / 2;
// Outdoors the World has no single walking plane. The city lawn, the town
// promenades, the district aprons, the bike lane and the office causeway each
// sit at their own height, and 0.38 — the top of the Office's floor slab — is
// an indoor number: used outside it parks a visitor a third of a unit above
// the grass. Standing avatars are placed on whatever walkable surface is
// directly beneath them, and this lawn height is what they fall back to when
// there is nothing under them at all.
export const CONTINUOUS_CITY_SURFACE_Y = 0.006;
// A ground probe starts this far above the visitor, so a kerb or bridge deck
// they have just stepped onto is still below the ray origin, and reaches this
// far down, so it can never pick up a deck several storeys below them.
export const GROUND_PROBE_RISE = 2.6;
export const GROUND_PROBE_DROP = 6;
// The Members Circle stands on its own dirt disc, which is laid just proud of
// the promenade it extends. Seats and standing places in the clearing are all
// measured from this surface.
export const CAMPFIRE_GROUND_Y = WORLD_PATH_SURFACE_Y + 0.01;
export const DISTRICT_GROUND_RADIUS = 48;
// The mirror-node yard is open to the rest of the World, and every cabinet in
// it is drawn at full detail from any camera distance. A cabinet is the node:
// its name, status light, and readouts are what the yard is for, so swapping
// them for silhouettes while zoomed out only made the ring unreadable.
export const NODE_PLAZA_MAX_RADIUS = 28;
// A node earns a cabinet by having been active within this window. Anything
// older is a machine that has stopped reporting, and a yard full of long-dead
// cabinets buries the nodes that are actually serving the mesh right now.
export const NODE_ACTIVE_WINDOW_MS = 24 * 60 * 60 * 1000;
export const REPOSITORY_GROUND_RADIUS = REPOSITORY_ISLAND_RING_RADIUS + 7;
export const REPOSITORY_RING_EXPAND_RADIUS = REPOSITORY_ISLAND_RING_RADIUS + 1.8;
export const REPOSITORY_RING_COLLAPSE_RADIUS = REPOSITORY_ISLAND_RING_RADIUS + 5.6;
// Folded away, the concentric rings sit exactly on the portal's outline torus
// (1.08 of the node radius, against the outermost ring's 1.58), so the moment
// they start to matter they grow out of the disc's own edge.
export const REPOSITORY_RING_COLLAPSED_SCALE = 1.08 / 1.58;
export const REPOSITORY_RING_EXPANSION_RATE = 4.6;
// Avatar geometry is shared across users, while each person remains complete
// and visible at every camera distance.
export const OFFICE_EXTERIOR_LOD_DISTANCE = OFFICE_DEPTH / 2 + 190;
// Panoramic office floors still look onto Town. Cull only roots that are both
// expensive and far enough away to contribute little to the glass-wall view.
export const ENCLOSURE_EXTERIOR_TRIANGLE_LIMIT = 5000;
export const ENCLOSURE_EXTERIOR_KEEP_RADIUS = OFFICE_DEPTH / 2 + 155;
// From a remote district package the mainland is a horizon footprint.
export const MAINLAND_DISTANT_TRIANGLE_BUDGET = 120;
export const WORLD_BIKE_LANE_CENTER_X = 0;
export const WORLD_BIKE_LANE_CENTER_Z = CONTINUOUS_CITY_CENTER_Z;
export const WORLD_ROAD_WIDTH = 10;
export const WORLD_ROAD_SURFACE_Y = CONTINUOUS_CITY_SURFACE_Y;
export const WORLD_ROAD_MARKING_Y = WORLD_ROAD_SURFACE_Y + 0.03;
export const WORLD_ROAD_CORNER_SEGMENTS = 16;
const WORLD_HEX_PITCH = CONTINUOUS_CITY_RADIUS * 2 + WORLD_ROAD_WIDTH;
export const WORLD_BIKE_LANE_APOTHEM = CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH / 2;
export const WORLD_BIKE_LANE_CORNER_RADIUS =
  CONTINUOUS_CITY_CORNER_RADIUS + WORLD_ROAD_WIDTH / 2;
export const WORLD_ROAD_JUNCTION_REACH = 18;
export const WORLD_ROAD_JUNCTION_WALK_RADIUS = 16;
export const DESERT_HEXES = Object.freeze(
  [0, 1, 2, 3, 4, 5].map((side) => {
    const angle = (side * Math.PI) / 3;
    const normalX = Math.cos(angle);
    const normalZ = Math.sin(angle);
    return Object.freeze({
      x: WORLD_BIKE_LANE_CENTER_X + normalX * WORLD_HEX_PITCH,
      z: CONTINUOUS_CITY_CENTER_Z + normalZ * WORLD_HEX_PITCH,
      normalX,
      normalZ,
    });
  }),
);
const WORLD_MESH_FLOWER_STEP = Object.freeze({
  x: WORLD_HEX_PITCH * 2.5,
  z: (WORLD_HEX_PITCH * Math.sqrt(3)) / 2,
});
function rotateMeshVector(vector, sixths) {
  const angle = (sixths * Math.PI) / 3;
  const cos = Math.cos(angle);
  const sin = Math.sin(angle);
  return { x: vector.x * cos - vector.z * sin, z: vector.x * sin + vector.z * cos };
}
const WORLD_MESH_SLOT_OFFSETS = Object.freeze(
  [0, 1, 2, 3, 4, 5]
    .map((side) => rotateMeshVector(WORLD_MESH_FLOWER_STEP, side))
    .concat(
      [0, 1, 2, 3, 4, 5].map((side) => {
        const first = rotateMeshVector(WORLD_MESH_FLOWER_STEP, side);
        const second = rotateMeshVector(WORLD_MESH_FLOWER_STEP, side + 1);
        return { x: first.x + second.x, z: first.z + second.z };
      }),
    )
    .map((offset) => Object.freeze(offset)),
);
export const WORLD_MESH_FLOWER_HEXES = Object.freeze(
  [Object.freeze({ x: 0, z: 0 })].concat(
    [0, 1, 2, 3, 4, 5].map((side) =>
      Object.freeze({
        x: Math.cos((side * Math.PI) / 3) * WORLD_HEX_PITCH,
        z: Math.sin((side * Math.PI) / 3) * WORLD_HEX_PITCH,
      }),
    ),
  ),
);
export const WORLD_MESH_FLOWER_REACH = WORLD_HEX_PITCH + CONTINUOUS_CITY_RADIUS * 1.2;
export const WORLD_MESH_HEX_SHAPE = "hex";
export const NEIGHBOUR_HEX_PETALS = Object.freeze([1, 2, 3, 4, 5]);
export const NEIGHBOUR_HEX_SLOT_COUNT = NEIGHBOUR_HEX_PETALS.length;
const NEIGHBOUR_HEX_OFFSETS = Object.freeze(
  NEIGHBOUR_HEX_PETALS.map((petal) => {
    const angle = (petal * Math.PI) / 3;
    return Object.freeze({
      x: Math.cos(angle) * WORLD_HEX_PITCH,
      z: Math.sin(angle) * WORLD_HEX_PITCH,
    });
  }),
);
export function neighbourHexPetal(slot) {
  const index = Number(slot);
  return Number.isInteger(index) ? NEIGHBOUR_HEX_PETALS[index] ?? -1 : -1;
}
export function worldMeshShape(shape, slot) {
  const index = Number(slot);
  return shape === WORLD_MESH_HEX_SHAPE &&
    Number.isInteger(index) && index >= 0 && index < NEIGHBOUR_HEX_SLOT_COUNT
    ? WORLD_MESH_HEX_SHAPE
    : "flower";
}
export function worldMeshSlotOffset(slot, shape = "flower") {
  const index = Number(slot);
  if (index === -1) return { x: 0, z: 0 };
  if (!Number.isInteger(index)) return null;
  if (worldMeshShape(shape, index) === WORLD_MESH_HEX_SHAPE) {
    return NEIGHBOUR_HEX_OFFSETS[index];
  }
  return WORLD_MESH_SLOT_OFFSETS[index] || null;
}
export function worldMeshHexesFor(shape) {
  return shape === WORLD_MESH_HEX_SHAPE
    ? WORLD_MESH_FLOWER_HEXES.slice(0, 1)
    : WORLD_MESH_FLOWER_HEXES;
}
export const INSTANCE_HEX_SLOTS = Object.freeze(
  WORLD_MESH_SLOT_OFFSETS.map((offset) => {
    const reach = Math.hypot(offset.x, offset.z);
    return Object.freeze({
      x: WORLD_BIKE_LANE_CENTER_X + offset.x,
      z: CONTINUOUS_CITY_CENTER_Z + offset.z,
      normalX: offset.x / reach,
      normalZ: offset.z / reach,
    });
  }),
);
export const WORLD_HEX_TILES = Object.freeze([
  Object.freeze({
    x: WORLD_BIKE_LANE_CENTER_X,
    z: CONTINUOUS_CITY_CENTER_Z,
    normalX: 1,
    normalZ: 0,
  }),
  ...DESERT_HEXES,
]);
export const WORLD_ROAD_JUNCTIONS = Object.freeze(
  [0, 1, 2, 3, 4, 5].map((corner) => {
    const angle = Math.PI / 6 + (corner * Math.PI) / 3;
    const reach = (WORLD_BIKE_LANE_APOTHEM * 2) / Math.sqrt(3);
    return Object.freeze({
      x: WORLD_BIKE_LANE_CENTER_X + Math.cos(angle) * reach,
      z: CONTINUOUS_CITY_CENTER_Z + Math.sin(angle) * reach,
      angle,
    });
  }),
);
export const DESERT_PLOT_APOTHEM = 88;
const DESERT_PLOT_TANGENT_OFFSET = 104;
export const DESERT_PLOT_SURFACE_Y = CONTINUOUS_CITY_SURFACE_Y;
export const DESERT_PLOTS = Object.freeze(
  DESERT_HEXES.flatMap((hex, side) => {
    const { normalX, normalZ } = hex;
    const distance =
      CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH + DESERT_PLOT_APOTHEM;
    return [-1, 1].map((direction) =>
      Object.freeze({
        x:
          WORLD_BIKE_LANE_CENTER_X +
          normalX * distance -
          normalZ * direction * DESERT_PLOT_TANGENT_OFFSET,
        z:
          CONTINUOUS_CITY_CENTER_Z +
          normalZ * distance +
          normalX * direction * DESERT_PLOT_TANGENT_OFFSET,
        normalX,
        normalZ,
        hex: side,
      }),
    );
  }),
);
// Floor-local position on the Engineering story. It is deliberately not part
// of the shared Town layout: a saved outdoor object placement must never pull
// an interior fixture back out of the building.
export const SYSTEM_CAPACITY_ENGINEERING_POSITION = Object.freeze([
  -15,
  0,
  17,
]);
export const SERVER_CABINET_YARD_ORIGIN = Object.freeze([18, 0, 0]);
export const ARRIVAL_GRID_BOUNDS = Object.freeze({
  minX: -10.8,
  maxX: 10.8,
  minZ: 14,
  maxZ: 32.4,
});
export const TREES_PER_LANDMARK = 3;
export const TREE_MIN_SPACING = 3.2;
const TREE_PORTAL_CLEARANCE = 7;
const TREE_LANDMARK_CLEARANCE = Object.freeze({
  fountain: 9,
  repositories: 9,
  organizations: 10,
  fediverse: 9,
  events: 9,
  neighborhood: 9,
  broadcast: 9,
  office: 10,
});

export function deterministicFraction(value) {
  return (hashNumber(value) % 1_000_003) / 1_000_003;
}

export function pointInsideBounds(x, z, bounds) {
  return (
    x >= bounds.minX &&
    x <= bounds.maxX &&
    z >= bounds.minZ &&
    z <= bounds.maxZ
  );
}

function isArrivalGridPosition(x, z) {
  return pointInsideBounds(Number(x) || 0, Number(z) || 0, ARRIVAL_GRID_BOUNDS);
}

export function arrivalFacingHeading(x, z, heading) {
  return isArrivalGridPosition(x, z) ? Math.PI : heading;
}

// Stable, readable id slug built from the only durable name a scene prop has:
// its placard title, or a node's name.
export function worldLayoutId(prefix, name) {
  const slug = String(name || "")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 60);
  return slug ? prefix + slug : "";
}
