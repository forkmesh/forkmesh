export const OFFICE_HEX_APOTHEM = 37;
export const OFFICE_HEX_RADIUS = (OFFICE_HEX_APOTHEM * 2) / Math.sqrt(3);
export const OFFICE_WIDTH = OFFICE_HEX_RADIUS * 2;
export const OFFICE_DEPTH = OFFICE_HEX_APOTHEM * 2;
export const OFFICE_ENTRANCE_Z = -170;
export const OFFICE_ISLAND_CENTER = Object.freeze([
  0,
  0,
  OFFICE_ENTRANCE_Z - OFFICE_DEPTH / 2,
]);
export const OFFICE_ISLAND_RADIUS = Math.ceil(OFFICE_HEX_RADIUS) + 14;
export const OFFICE_BRIDGE_WIDTH = 12;
export const OFFICE_BRIDGE_START_Z = -86;
export const OFFICE_BRIDGE_END_Z = -108;

export const OFFICE_HEX_SIDES = Object.freeze(
  ["front-right", "front", "front-left", "back-left", "back", "back-right"].map(
    (id, index) => {
      const angle = Math.PI / 6 + (index * Math.PI) / 3;
      const normalX = Math.round(Math.cos(angle) * 1e12) / 1e12;
      const normalZ = Math.round(Math.sin(angle) * 1e12) / 1e12;
      return Object.freeze({
        id,
        angle,
        normalX,
        normalZ,
        tangentX: -normalZ,
        tangentZ: normalX,
        rotationY: Math.atan2(-normalX, -normalZ),
        length: OFFICE_HEX_RADIUS,
      });
    },
  ),
);

export const OFFICE_HEX_CORNERS = Object.freeze(
  [0, 1, 2, 3, 4, 5].map((index) =>
    Object.freeze({
      x: Math.round(Math.cos((index * Math.PI) / 3) * OFFICE_HEX_RADIUS * 1e9) / 1e9,
      z: Math.round(Math.sin((index * Math.PI) / 3) * OFFICE_HEX_RADIUS * 1e9) / 1e9,
    }),
  ),
);

const OFFICE_HEX_SIDE_BY_ID = new Map(
  OFFICE_HEX_SIDES.map((side) => [side.id, side]),
);

export function officeHexSide(id) {
  return OFFICE_HEX_SIDE_BY_ID.get(String(id || "")) || null;
}

export function officeHexExtent(x, z) {
  const px = Math.abs(Number(x));
  const pz = Math.abs(Number(z));
  return Math.max(pz, (Math.sqrt(3) / 2) * px + 0.5 * pz);
}

export function officeHexContains(x, z, inset = 0) {
  const px = Number(x);
  const pz = Number(z);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  return officeHexExtent(px, pz) <= OFFICE_HEX_APOTHEM - (Number(inset) || 0) + 1e-9;
}

export function officeHexHalfWidth(z, inset = 0) {
  const reach = OFFICE_HEX_APOTHEM - (Number(inset) || 0);
  const pz = Math.abs(Number(z) || 0);
  if (pz > reach) return 0;
  return (reach - 0.5 * pz) / (Math.sqrt(3) / 2);
}

export function officeHexWallPoint(sideId, along = 0, inset = 0) {
  const side = officeHexSide(sideId);
  if (!side) return { x: 0, z: 0, rotationY: 0 };
  const reach = OFFICE_HEX_APOTHEM - (Number(inset) || 0);
  const offset = Number(along) || 0;
  return {
    x: side.normalX * reach + side.tangentX * offset,
    z: side.normalZ * reach + side.tangentZ * offset,
    rotationY: side.rotationY,
  };
}

export function officeHexOutline(inset = 0) {
  const radius = ((OFFICE_HEX_APOTHEM - (Number(inset) || 0)) * 2) / Math.sqrt(3);
  return OFFICE_HEX_CORNERS.map((_, index) => [
    Math.cos((index * Math.PI) / 3) * radius,
    Math.sin((index * Math.PI) / 3) * radius,
  ]);
}

// Give every team a genuinely spacious story. The original eight-unit
// spacing let the larger floor exhibits visually intersect the slabs above.
export const OFFICE_FLOOR_HEIGHT = 16;
export const OFFICE_FLOOR_COUNT = 4;
export const OFFICE_TOWER_HEIGHT = OFFICE_FLOOR_HEIGHT * OFFICE_FLOOR_COUNT;
export const OFFICE_FRONT_Z = OFFICE_DEPTH / 2;
export const OFFICE_DISTANT_DRAW_BUDGET = 64;
export const OFFICE_DOOR_WIDTH = 10;
export const OFFICE_AVATAR_RADIUS = 0.46;
export const OFFICE_ENTER_DISTANCE = 6.5;
export const OFFICE_EXIT_DISTANCE = 7.5;

export function nextOfficeZoneState(currentState, distance) {
  const threshold =
    currentState === "nearby"
      ? OFFICE_EXIT_DISTANCE
      : OFFICE_ENTER_DISTANCE;
  return Number.isFinite(distance) && distance <= threshold
    ? "nearby"
    : "distant";
}
export const OFFICE_ELEVATOR_CENTER_X = 12;
export const OFFICE_ELEVATOR_CENTER_Z = -OFFICE_FRONT_Z;
export const OFFICE_ELEVATOR_ROTATION_Y = Math.PI;

export const OFFICE_FLOORS = Object.freeze([
  Object.freeze({
    id: "lobby",
    level: 0,
    label: "Lobby",
    team: "",
    publicForMembers: true,
    description: "Welcome desk, time clock, and chrome ForkMesh fountain",
  }),
  Object.freeze({
    id: "marketing",
    level: 1,
    label: "Marketing",
    team: "marketing",
    description: "Campaign studio, task wall, and encrypted meeting table",
  }),
  Object.freeze({
    id: "engineering",
    level: 2,
    label: "Engineering",
    team: "engineering",
    description: "Pairing stations, a live build runway, and system capacity",
  }),
  Object.freeze({
    id: "rooftop",
    level: 3,
    label: "Rooftop Patio",
    team: "",
    publicForMembers: true,
    description: "Glass-safe patio, telescope, planters, and seating",
  }),
]);

// Keep this list byte-for-byte aligned with entry.py's
// OFFICE_FLOOR_TEAM_ALIASES for every floor that exists here. The Worker remains authoritative: this copy only
// explains which elevator floor a website organization-team checkbox will
// unlock after the server accepts the membership change and /office/floors is
// refreshed.
export const OFFICE_FLOOR_TEAM_ALIASES = Object.freeze({
  marketing: Object.freeze([
    "marketing",
    "marketing-team",
    "growth",
    "brand",
    "communications",
    "comms",
  ]),
  engineering: Object.freeze([
    "engineering",
    "engineers",
    "development",
    "developers",
    "platform",
    "frontend",
    "backend",
  ]),
});

const OFFICE_FLOOR_BY_ID = new Map(
  OFFICE_FLOORS.map((floor) => [floor.id, floor]),
);

export function officeFloorById(value) {
  return OFFICE_FLOOR_BY_ID.get(String(value || "").trim().toLowerCase()) || null;
}

export function officeFloorY(value) {
  const floor =
    typeof value === "number"
      ? OFFICE_FLOORS[Math.max(0, Math.min(OFFICE_FLOORS.length - 1, value))]
      : officeFloorById(value);
  return (floor?.level || 0) * OFFICE_FLOOR_HEIGHT;
}

export function officeTeamSlug(value) {
  return String(value || "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "")
    .slice(0, 64);
}

export function officeFloorsForTeam(value) {
  const team = officeTeamSlug(value);
  if (!team) return [];
  return OFFICE_FLOORS.filter((floor) =>
    OFFICE_FLOOR_TEAM_ALIASES[floor.id]?.includes(team),
  );
}

export function normalizeOfficeFloorAccess(payload = {}) {
  const authenticated = payload?.authenticated === true;
  const supplied = new Set(
    Array.isArray(payload?.allowedFloorIds)
      ? payload.allowedFloorIds
          .map((value) => officeFloorById(value)?.id)
          .filter(Boolean)
      : [],
  );
  if (authenticated) {
    // The lobby and the rooftop patio are the two common floors every member
    // shares. Every department story — Marketing included — is a team floor:
    // it only unlocks through the server-issued allowlist above.
    supplied.add("lobby");
    supplied.add("rooftop");
  }
  return {
    authenticated,
    account: authenticated
      ? String(payload?.account || "").trim().toLowerCase().slice(0, 64)
      : "",
    allowedFloorIds: [...supplied],
    teams: Array.isArray(payload?.teams)
      ? payload.teams
          .map((value) => String(value || "").trim().toLowerCase().slice(0, 64))
          .filter(Boolean)
          .slice(0, 32)
      : [],
  };
}

export function canAccessOfficeFloor(access, floorId) {
  const normalized = normalizeOfficeFloorAccess(access);
  const floor = officeFloorById(floorId);
  return Boolean(
    normalized.authenticated &&
      floor &&
      normalized.allowedFloorIds.includes(floor.id),
  );
}

export function officeCampusSurfaceContains(x, z, radius = 0) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  const islandDistance = Math.hypot(
    px - OFFICE_ISLAND_CENTER[0],
    pz - OFFICE_ISLAND_CENTER[2],
  );
  if (islandDistance <= OFFICE_ISLAND_RADIUS - margin + 1e-9) return true;
  return (
    Math.abs(px - OFFICE_ISLAND_CENTER[0]) <=
      OFFICE_BRIDGE_WIDTH / 2 - margin + 1e-9 &&
    pz <= OFFICE_BRIDGE_START_Z + margin &&
    pz >= OFFICE_BRIDGE_END_Z - margin
  );
}

// The tower shell is a solid volume for every movement path that leaves the
// ground. Walking is fenced by the town wall constraint, but flight — the
// quadcopter, the wearable jetpack, a super jump — used to skip that pass
// entirely, so a visitor could cross the curtain wall in mid-air and hover
// inside a team floor the elevator refuses to open. Coordinates are
// tower-local: X/Z relative to the campus centre, Y measured from the deck
// the tower stands on. Flying over the roof line stays free.
export function officeTowerShellContains(x, y, z, radius = 0) {
  const px = Number(x);
  const py = Number(y);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(py) || !Number.isFinite(pz)) {
    return false;
  }
  return (
    py >= -margin &&
    py <= OFFICE_TOWER_HEIGHT + margin &&
    officeHexContains(px, pz, -margin)
  );
}

// Where a flyer that reached the inside of the shell belongs instead. The face
// it actually crossed wins, so a craft driven at the glass is returned to the
// façade it came from rather than teleported out of the far side; a position
// that was already inside falls back to the nearest façade. Returns null
// when the point is clear of the tower, otherwise the corrected tower-local
// point and the face that stopped it.
export function officeTowerShellExit(previous, current, radius = 0) {
  const margin = Math.max(0, Number(radius) || 0);
  const x = Number(current?.x);
  const y = Number(current?.y);
  const z = Number(current?.z);
  if (!officeTowerShellContains(x, y, z, margin)) return null;
  const reach = OFFICE_HEX_APOTHEM + margin;
  const roofY = OFFICE_TOWER_HEIGHT + margin;
  const priorX = Number.isFinite(Number(previous?.x)) ? Number(previous.x) : x;
  const priorY = Number.isFinite(Number(previous?.y)) ? Number(previous.y) : y;
  const priorZ = Number.isFinite(Number(previous?.z)) ? Number(previous.z) : z;
  const faces = [
    {
      face: "roof",
      side: null,
      depth: roofY - y,
      crossed: priorY >= roofY,
    },
    ...OFFICE_HEX_SIDES.map((side) => ({
      face: side.id,
      side,
      depth: reach - (side.normalX * x + side.normalZ * z),
      crossed: side.normalX * priorX + side.normalZ * priorZ >= reach,
    })),
  ];
  const entered = faces.filter((candidate) => candidate.crossed);
  const [exit] = (
    entered.length
      ? entered
      : faces.filter((candidate) => candidate.side)
  ).sort((left, right) => left.depth - right.depth);
  if (!exit.side) return { x, y: roofY, z, face: exit.face };
  return {
    x: x + exit.side.normalX * exit.depth,
    y,
    z: z + exit.side.normalZ * exit.depth,
    face: exit.face,
  };
}

// Every round table in the tower is a cylinder, so its collider is the same
// circle rather than a rectangle inscribed in it. `topY` is the height of the
// tabletop above the floor slab: a visitor whose feet are above it — a rooftop
// jump — passes over the table, and anyone below it is stopped by the wood
// instead of standing inside it.
//
// The Marketing meeting table is CylinderGeometry(5.9, 5.65, 0.5) centred on
// the floor with its top 2.35 above the slab, ringed by chairs at radius 7.4.
export const OFFICE_MEETING_TABLE = Object.freeze({
  type: "circle",
  x: 0,
  z: 0,
  radius: 5.9,
  topY: 2.35,
});

export const OFFICE_LOUNGE = Object.freeze({
  x: 19,
  z: 11,
  seatOffsetX: 6.5,
});

const OFFICE_LOUNGE_TABLE = Object.freeze({
  type: "circle",
  x: OFFICE_LOUNGE.x,
  z: OFFICE_LOUNGE.z,
  radius: 3.2,
  topY: 1.26,
});

export const OFFICE_LOBBY_FOUNTAIN = Object.freeze({
  x: -19,
  z: 11,
  radius: 8.5,
});

export const OFFICE_RECEPTION_DESK = Object.freeze({
  x: 0,
  z: -13,
  halfWidth: 15,
  halfDepth: 2.5,
});

export const OFFICE_ENGINEERING_STATION_XS = Object.freeze(
  [-28, -14, 0, 14, 28],
);
export const OFFICE_ENGINEERING_STATION_Z = -4;
export const OFFICE_ENGINEERING_STATION_HALF_WIDTH = 5.5;
export const OFFICE_ENGINEERING_STATION_HALF_DEPTH = 2.2;

export const OFFICE_ROOFTOP_TABLES = Object.freeze(
  [-18, 0, 18].map((x) => Object.freeze({ x, z: 16 })),
);

const FLOOR_OBSTACLES = Object.freeze({
  lobby: Object.freeze([
    Object.freeze({ type: "circle", ...OFFICE_LOBBY_FOUNTAIN }),
    Object.freeze({
      type: "rect",
      minX: OFFICE_RECEPTION_DESK.x - OFFICE_RECEPTION_DESK.halfWidth - 0.5,
      maxX: OFFICE_RECEPTION_DESK.x + OFFICE_RECEPTION_DESK.halfWidth + 0.5,
      minZ: OFFICE_RECEPTION_DESK.z - OFFICE_RECEPTION_DESK.halfDepth - 0.2,
      maxZ: OFFICE_RECEPTION_DESK.z + OFFICE_RECEPTION_DESK.halfDepth + 0.2,
    }),
    OFFICE_LOUNGE_TABLE,
  ]),
  marketing: Object.freeze([
    // Match the actual round tabletop instead of fencing off the chairs,
    // boards, and most of the studio with oversized invisible rectangles. The
    // former 7.8x4 rectangle was inscribed inside the 11.8-wide top, so a
    // visitor walking in from the north or south stood inside the wood.
    OFFICE_MEETING_TABLE,
    OFFICE_LOUNGE_TABLE,
  ]),
  engineering: Object.freeze([
    ...OFFICE_ENGINEERING_STATION_XS.map((x) =>
      Object.freeze({
        type: "rect",
        minX: x - OFFICE_ENGINEERING_STATION_HALF_WIDTH,
        maxX: x + OFFICE_ENGINEERING_STATION_HALF_WIDTH,
        minZ: OFFICE_ENGINEERING_STATION_Z - OFFICE_ENGINEERING_STATION_HALF_DEPTH,
        maxZ: OFFICE_ENGINEERING_STATION_Z + OFFICE_ENGINEERING_STATION_HALF_DEPTH,
      }),
    ),
    OFFICE_LOUNGE_TABLE,
  ]),
  rooftop: Object.freeze(
    OFFICE_ROOFTOP_TABLES.map((table) =>
      Object.freeze({
        type: "circle",
        x: table.x,
        z: table.z,
        radius: 3.5,
        topY: 2.85,
      }),
    ),
  ),
});

export const OFFICE_ELEVATOR_SHELL = Object.freeze([
  Object.freeze({
    type: "rect",
    minX: OFFICE_ELEVATOR_CENTER_X - 5.45,
    maxX: OFFICE_ELEVATOR_CENTER_X - 4.55,
    minZ: OFFICE_ELEVATOR_CENTER_Z - 4.45,
    maxZ: OFFICE_ELEVATOR_CENTER_Z + 4.45,
  }),
  Object.freeze({
    type: "rect",
    minX: OFFICE_ELEVATOR_CENTER_X + 4.55,
    maxX: OFFICE_ELEVATOR_CENTER_X + 5.45,
    minZ: OFFICE_ELEVATOR_CENTER_Z - 4.45,
    maxZ: OFFICE_ELEVATOR_CENTER_Z + 4.45,
  }),
  Object.freeze({
    type: "rect",
    minX: OFFICE_ELEVATOR_CENTER_X - 5.45,
    maxX: OFFICE_ELEVATOR_CENTER_X + 5.45,
    minZ: OFFICE_ELEVATOR_CENTER_Z - 4.45,
    maxZ: OFFICE_ELEVATOR_CENTER_Z - 3.55,
  }),
]);

const OFFICE_COMMON_OBSTACLES = OFFICE_ELEVATOR_SHELL;

export function officeInteriorContains(x, z, radius = OFFICE_AVATAR_RADIUS) {
  const margin = Math.max(0, Number(radius) || 0);
  return officeHexContains(x, z, margin + 0.65);
}

export function officeElevatorCabinContains(
  x,
  z,
  radius = OFFICE_AVATAR_RADIUS,
) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  return (
    Math.abs(px - OFFICE_ELEVATOR_CENTER_X) <= 4.45 - margin &&
    Math.abs(pz - OFFICE_ELEVATOR_CENTER_Z) <= 3.45 - margin
  );
}

// The campus walker leaves the tower footprint behind at the facade, but the
// cabin does not: half of its glass envelope stands outdoors. Outdoor movement
// has to honour the same three panes the interior obstacle set already does,
// or a visitor steps through them and ends up standing in a cab that no office
// entry ever initialized.
export function officeElevatorShellContains(
  x,
  z,
  radius = OFFICE_AVATAR_RADIUS,
) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  if (!Number.isFinite(px) || !Number.isFinite(pz)) return false;
  return OFFICE_ELEVATOR_SHELL.some(
    (pane) =>
      px >= pane.minX - margin &&
      px <= pane.maxX + margin &&
      pz >= pane.minZ - margin &&
      pz <= pane.maxZ + margin,
  );
}

// feetY is how far the visitor's feet stand above this floor's slab, which is
// 0 for everyone on the ground and only rises during a rooftop jump. Furniture
// that declares a `topY` stops being solid once the feet clear it: jumping over
// a patio table is meant to work, walking through one at shin height is not.
export function officePointHitsObstacle(
  floorId,
  x,
  z,
  radius = OFFICE_AVATAR_RADIUS,
  feetY = 0,
) {
  const px = Number(x);
  const pz = Number(z);
  const margin = Math.max(0, Number(radius) || 0);
  const height = Number.isFinite(Number(feetY)) ? Number(feetY) : 0;
  return [
    ...OFFICE_COMMON_OBSTACLES,
    ...(FLOOR_OBSTACLES[officeFloorById(floorId)?.id] || []),
  ].some(
    (obstacle) => {
      if (Number.isFinite(obstacle.topY) && height >= obstacle.topY) {
        return false;
      }
      if (obstacle.type === "circle") {
        return (
          Math.hypot(px - obstacle.x, pz - obstacle.z) <
          obstacle.radius + margin
        );
      }
      return (
        px >= obstacle.minX - margin &&
        px <= obstacle.maxX + margin &&
        pz >= obstacle.minZ - margin &&
        pz <= obstacle.maxZ + margin
      );
    },
  );
}

export function officeInteriorPointIsWalkable(
  floorId,
  x,
  z,
  radius = OFFICE_AVATAR_RADIUS,
  feetY = 0,
) {
  return (
    (
      officeInteriorContains(x, z, radius) ||
      officeElevatorCabinContains(x, z, radius)
    ) &&
    !officePointHitsObstacle(floorId, x, z, radius, feetY)
  );
}

function roundPose(value) {
  return Math.round(value * 10000) / 10000;
}

// The open floor ring between the meeting tabletop and the chairs around it.
export const OFFICE_MEETING_STANDING_RADIUS =
  OFFICE_MEETING_TABLE.radius + OFFICE_AVATAR_RADIUS + 0.35;

// Where a standing meeting attendee belongs. Both callers used to fold a pose
// into a box a few units wide around the middle of the Marketing room, which
// is the middle of the round table: an attendee who had not walked anywhere
// yet, and any attendee who walked past the box, was drawn standing inside the
// wood. Keep the reported spot when it is on open floor, and otherwise leave
// the tabletop by its nearest edge instead of folding into it.
export function officeMeetingStandingPoint(x, z) {
  const reach = OFFICE_HEX_APOTHEM - OFFICE_AVATAR_RADIUS - 0.65;
  const rawX = Number(x) || 0;
  const rawZ = Number(z) || 0;
  const extent = officeHexExtent(rawX, rawZ);
  const shrink = extent > reach ? reach / extent : 1;
  const px = rawX * shrink;
  const pz = rawZ * shrink;
  const dx = px - OFFICE_MEETING_TABLE.x;
  const dz = pz - OFFICE_MEETING_TABLE.z;
  const distance = Math.hypot(dx, dz);
  if (distance >= OFFICE_MEETING_STANDING_RADIUS) return { x: px, z: pz };
  // A pose on the table's own axis has no direction to be pushed along. Send
  // it to the room's front edge, which is the side every default meeting pose
  // has always faced the table from.
  const angle = distance > 1e-6 ? Math.atan2(dz, dx) : Math.PI / 2;
  // A pose is a rendered stance and a broadcast frame, not a collision result,
  // so round it: a point on the table's axis reports 0 instead of the float
  // remainder of the trigonometry that moved it.
  return {
    x: roundPose(
      OFFICE_MEETING_TABLE.x +
        Math.cos(angle) * OFFICE_MEETING_STANDING_RADIUS,
    ),
    z: roundPose(
      OFFICE_MEETING_TABLE.z +
        Math.sin(angle) * OFFICE_MEETING_STANDING_RADIUS,
    ),
  };
}

// Nearest point outside every solid footprint on a floor. A visitor who ends a
// jump inside a table has no valid earlier position to slide back to — the one
// it jumped from is a whole arc away — so the answer has to be geometric:
// leave each footprint by its closest edge.
export function officePointOutsideObstacles(
  floorId,
  x,
  z,
  radius = OFFICE_AVATAR_RADIUS,
  feetY = 0,
) {
  const margin = Math.max(0, Number(radius) || 0);
  const height = Number.isFinite(Number(feetY)) ? Number(feetY) : 0;
  const gap = 1e-4;
  const obstacles = [
    ...OFFICE_COMMON_OBSTACLES,
    ...(FLOOR_OBSTACLES[officeFloorById(floorId)?.id] || []),
  ].filter(
    (obstacle) =>
      !(Number.isFinite(obstacle.topY) && height >= obstacle.topY),
  );
  let px = Number(x) || 0;
  let pz = Number(z) || 0;
  // Leaving one footprint can push a point into a neighbouring one, so repeat
  // until the point is clear of all of them. The tower's obstacles never chain
  // more deeply than the elevator shell's three panes.
  for (let pass = 0; pass < 4; pass += 1) {
    let moved = false;
    for (const obstacle of obstacles) {
      if (obstacle.type === "circle") {
        const dx = px - obstacle.x;
        const dz = pz - obstacle.z;
        const distance = Math.hypot(dx, dz);
        const clearance = obstacle.radius + margin + gap;
        if (distance >= clearance) continue;
        const angle = distance > 1e-6 ? Math.atan2(dz, dx) : 0;
        px = obstacle.x + Math.cos(angle) * clearance;
        pz = obstacle.z + Math.sin(angle) * clearance;
        moved = true;
        continue;
      }
      const minX = obstacle.minX - margin;
      const maxX = obstacle.maxX + margin;
      const minZ = obstacle.minZ - margin;
      const maxZ = obstacle.maxZ + margin;
      if (px < minX || px > maxX || pz < minZ || pz > maxZ) continue;
      const [exit] = [
        { depth: px - minX, x: minX - gap, z: pz },
        { depth: maxX - px, x: maxX + gap, z: pz },
        { depth: pz - minZ, x: px, z: minZ - gap },
        { depth: maxZ - pz, x: px, z: maxZ + gap },
      ].sort((left, right) => left.depth - right.depth);
      px = exit.x;
      pz = exit.z;
      moved = true;
    }
    if (!moved) break;
  }
  return { x: px, z: pz };
}
