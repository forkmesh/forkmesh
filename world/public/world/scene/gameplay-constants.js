import {
  OFFICE_AVATAR_RADIUS,
  OFFICE_FLOOR_HEIGHT,
  OFFICE_FRONT_Z,
  OFFICE_HEX_RADIUS,
  officeHexSide,
  officeHexWallPoint,
} from "../world-office-tower.js";

// Base (from-rest) speed. Raised so keyboard movement leaves standstill with
// more pace by default; multiplied by the per-device move-speed control.
export const PLAYER_SPEED = 6.4;
export const PLAYER_MAX_SPEED = 17;
// Walking input accelerates from base pace into a higher "running" pace over a
// short interval so holding a direction grows into a smooth speed gradient.
export const PLAYER_SPEED_GRADIENT_UP_TIME = 0.85;
export const PLAYER_SPEED_GRADIENT_DOWN_TIME = 0.25;
// Holding either Shift key is an explicit sprint: fast enough to cross the
// World quickly, while still using the ordinary collision and presence path.
export const PLAYER_SPRINT_MULTIPLIER = 2.6;
export const BIKE_RIDE_SPEED_MULTIPLIER = 2.15;
export const BIKE_RIDING_ACTIVITY = "riding the World bike lane";
export const WORLD_BIKE_MOUNT_DISTANCE = 5;
export const CAR_RIDE_SPEED_MULTIPLIER = 3.2;
export const CAR_RIDING_ACTIVITY = "driving the beach road";
export const QUADCOPTER_HORIZONTAL_SPEED = 42;
export const QUADCOPTER_VERTICAL_SPEED = 28;
export const QUADCOPTER_CRUISE_MAX_SPEED = 150;
export const QUADCOPTER_CRUISE_ACCELERATION = 90;
export const QUADCOPTER_MAX_ALTITUDE = 480;
export const QUADCOPTER_MOUNT_DISTANCE = 7;
// Motor-to-motor radius of the craft. The Office tower shell is solid at this
// distance so the rotors stop at the curtain wall instead of the pilot's seat
// crossing it.
export const QUADCOPTER_SHELL_RADIUS = 3.2;
// The altitude a quadcopter sits at with its skids on the ground, matching the
// two spawn coordinates and the floor of the ride's own altitude clamp.
export const QUADCOPTER_REST_ALTITUDE = 0.24;
// Wide enough to hold the whole airframe (rotor tips reach 3.8 from the hub)
// clear of a wall it was parked against, and still well inside the mount
// distance for a visitor standing on the other side of that wall.
export const QUADCOPTER_PARK_CLEARANCE = 4;
export const QUADCOPTER_PARK_SEARCH_STEPS = 120;
export const QUADCOPTER_RIDING_ACTIVITY = "flying the World quadcopter";
export const DRONE_COURSE_RING_COUNT = 10;
export const DRONE_COURSE_RING_RADIUS = 8;
export const DRONE_COURSE_START = 50;
export const DRONE_COURSE_WEAVE_WIDTH = 18;
export const DRONE_COURSE_WEAVE_HEIGHT = 14;
export const DRONE_COURSE_FLOOR = 14;
export const DRONE_COURSE_DETAIL_ENTER_DISTANCE = 150;
export const DRONE_COURSE_COARSE_DISTANCE = 190;
export const DRONE_RACE_GATE_RADIUS = 9.5;
export const DRONE_RACE_GATE_HALF_SIZE = 10;
export const DRONE_RACE_ARROWS_PER_TARGET = 3;
export const DRONE_RACE_ARROW_SPACING = 12;
export const DRONE_RACE_ARROW_TILT = 0.95;
export const DRONE_RACE_GUIDE_DISTANCE = 11;
export const DRONE_RACE_GHOST_SAMPLE_MS = 100;
export const DRONE_RACE_GHOST_MAX_SAMPLES = 6000;
export const DRONE_RACE_MAX_LAP_MS = 3_600_000;
export const DRONE_RACE_REMOTE_STALE_MS = 15_000;
export const DRONE_RACE_STORAGE_KEY = "forkmesh.world.droneRace.v1";
export const COASTER_APOTHEM = 320;
export const COASTER_CORNER_RADIUS = 80;
export const COASTER_LIFT_HEIGHT = 60;
export const COASTER_GRAVITY = 30;
export const COASTER_CHAIN_SPEED = 13;
export const COASTER_STATION_SPEED = 7;
export const COASTER_SAMPLE_STEP = 0.5;
export const COASTER_RAIL_GAUGE = 2.2;
export const COASTER_CAR_COUNT = 3;
export const COASTER_CAR_SPACING = 4.6;
export const COASTER_SEAT_HEIGHT = 1.05;
export const COASTER_MOUNT_DISTANCE = 12;
export const COASTER_DETAIL_DISTANCE = 420;
export const COASTER_RIDING_ACTIVITY = "riding the ForkMesh coaster";
export const JETPACK_HORIZONTAL_SPEED = 30;
export const JETPACK_VERTICAL_SPEED = 22;
export const JETPACK_MAX_ALTITUDE = 480;
export const JETPACK_FLYING_ACTIVITY = "flying with a jetpack";
// Recreation now belongs to the Office campus garden instead of occupying the
// Town Square lawn. Both fixtures remain on the same continuous walk surface.
export const GYM_POSITION = Object.freeze([66, 0.14, -137]);
export const GYM_BENCH_LIE_POSITION = Object.freeze([-7.6, 1.67, 0.525]);
export const GYM_BENCH_PRESS_TOP_Y = 2.74;
export const GYM_BENCH_PRESS_DEPTH = 0.5;
export const GYM_BENCH_REP_MS = 900;
export const SWING_SET_POSITION = Object.freeze([43, 0, -137]);
export const GYM_HEAVY_WEIGHT_LB = 315;
export const GYM_MAX_WEIGHT_LB = 1200;
// Double-clicking the ground sends the avatar to that spot at a dash speed far
// above the walking cap, so crossing the whole square takes a couple of seconds
// without teleporting the avatar out from under the camera.
export const PLAYER_DASH_SPEED = 48;
export const PLAYER_DASH_ARRIVE_DISTANCE = 0.3;
export const PLAYER_SUPER_JUMP_VELOCITY = 82;
export const PLAYER_SUPER_JUMP_MOVE_MULTIPLIER = 3.5;
export const ROOF_PARACHUTE_DEPLOY_VELOCITY = -0.35;
export const ROOF_PARACHUTE_TERMINAL_VELOCITY = -8.4;
export const ROOF_PARACHUTE_GRAVITY = 9.6;
export const ROOF_PARACHUTE_COLLAPSE_MS = 900;
// The rooftop zip line leaves a mast on the Office roof's east edge (Office
// interior coordinates) and lands on a mast inside the beach horizon. The
// roof anchor is high enough that a hanging rider clears the glass barrier.
export const ZIP_LINE_ROOF_LOCAL = Object.freeze([
  OFFICE_HEX_RADIUS - 9,
  -4,
]);
export const ZIP_LINE_ROOF_ANCHOR_HEIGHT = 10.5;
export const ZIP_LINE_BEACH_ANCHOR_HEIGHT = 4.95;
export const ZIP_LINE_BEACH_OFFSET = 44;
// Beach is an isolated scene. The rider switches into it once inside the
// painted horizon so the last stretch of cable lands on visible sand.
export const ZIP_LINE_BEACH_ENTRY_RADIUS = 70;
export const ZIP_LINE_SAG = 9;
// Cable to the soles of a hanging rider, and cable to the harness shoulders.
export const ZIP_LINE_RIDER_DROP = 4.4;
export const ZIP_LINE_SHOULDER_DROP = 1.7;
export const ZIP_LINE_MAX_SPEED = 38;
export const ZIP_LINE_ACCELERATION = 9;
export const ZIP_LINE_LIFT_MS = 800;
export const ZIP_LINE_LAUNCH_RADIUS = 9;
export const ZIP_LINE_RIDING_ACTIVITY = "riding the zip line to the beach";
export const CAMERA_OFFSET = [17, 16, 21];
export const CAMERA_DISTANCE = Math.hypot(...CAMERA_OFFSET);
export const QUADCOPTER_ORBIT_LEVEL_PITCH = Math.asin(CAMERA_OFFSET[1] / CAMERA_DISTANCE);
// Let players pull all the way back to a map-scale view where the World is a
// small speck. The enlarged far plane keeps that overview visible instead of
// clipping the ground and distant landmarks.
export const CAMERA_ZOOM_MIN = 0.06;
export const CAMERA_ZOOM_MAX = 28;
// The office is intentionally twenty times farther down the horizon, but its
// sub-900-triangle silhouette should remain barely visible from Town. This
// still covers the maximum strategic camera pullback without an infinite far
// plane or logarithmic-depth cost.
export const CAMERA_FAR_PLANE = 16_000;
export const CAMERA_LOOK_SENSITIVITY = 0.0022;
// Dragging upward lowers the orbit eye beneath the target, which is how this
// camera looks into the sky. Allow the full arc in both directions.
export const CAMERA_PITCH_MIN = -Math.PI / 2 + 0.01;
// Stop just shy of vertical so the camera stays numerically stable while a
// player can still look directly into the sky.
export const CAMERA_PITCH_MAX = Math.PI / 2 - 0.01;
export const WORLD_GROUND_Y = 0;
export const CAMERA_GROUND_CLEARANCE = 0.6;
// Pull the third-person eye close before it crosses the Office façade. Keeping
// the same chase distance on both sides of the doorway prevents the camera
// lerp from briefly travelling through the building shell during checkout.
export const OFFICE_EXIT_CAMERA_ZOOM = 0.48;
export const FIRST_PERSON_EYE_HEIGHT = 2.2;
export const FIRST_PERSON_ZOOM_MIN = 0.25;
export const FIRST_PERSON_ZOOM_MAX = 5;
export const FIRST_PERSON_PITCH_MIN = -Math.PI / 2 + 0.01;
export const FIRST_PERSON_PITCH_MAX = Math.PI / 2 - 0.01;
const REPOSITORY_FIRST_PERSON_DISTANCE = 5.5;
const REPOSITORY_FIRST_PERSON_PITCH = -0.08;
export const OFFICE_HEIGHT = OFFICE_FLOOR_HEIGHT;
export const OFFICE_LOBBY_SURFACE_Y = 0.38;
export const OFFICE_BUILD_STAMP_REFRESH_MS = 30 * 1000;
export const OFFICE_BUILD_STAMP_HIDDEN_RETRY_MS = 2000;
export const OFFICE_INTERIOR_WALL_LIMIT = OFFICE_FRONT_Z - 0.54;
export const OFFICE_DOOR_HEIGHT = OFFICE_HEIGHT;
// Commit entry only after the avatar's complete collision capsule has cleared
// the inner face of the jamb. The old trigger sat outside the facade, which
// counted visitors as indoors and switched colliders before physical entry.
export const OFFICE_DOORWAY_ENTRY_Z =
  OFFICE_FRONT_Z - OFFICE_AVATAR_RADIUS - 0.06;
// This open tunnel spans both sides of the facade and gives a rejected entry
// callback several inward frames to retry without ever creating a wall.
export const OFFICE_DOORWAY_APPROACH_Z =
  OFFICE_FRONT_Z + OFFICE_AVATAR_RADIUS + 0.9;
export const OFFICE_DOORWAY_PASSAGE_MIN_Z = OFFICE_DOORWAY_ENTRY_Z - 2;
// The generic interior rectangle stops 0.65 units shy of the wall. Overlap
// the doorway tunnel with that exact edge so there is no disconnected sliver
// for the lobby collider to reject just after the entry handoff.
export const OFFICE_DOORWAY_INTERIOR_JOIN_Z =
  OFFICE_FRONT_Z - OFFICE_AVATAR_RADIUS - 0.7;
// Leaving is counted only after the whole avatar clears the outer jamb.
export const OFFICE_INTERIOR_EXIT_Z =
  OFFICE_FRONT_Z + OFFICE_AVATAR_RADIUS + 0.06;
export const OFFICE_ELEVATOR_HALF_WIDTH = 5;
export const OFFICE_ELEVATOR_HALF_DEPTH = 4;
export const OFFICE_ELEVATOR_CUT_MARGIN = 0.35;
export const OFFICE_ROOF_BARRIER_TOP = 3.35;
export const OFFICE_ELEVATOR_SHAFT_ROOF_HEIGHT = 8.4;
export const OFFICE_FLOOR_WARP_DOOR_WIDTH = 5.3;
export const OFFICE_FLOOR_WARP_DOOR_HEIGHT = 5.8;
export const OFFICE_FLOOR_WARP_DOOR_SPACING = 5.95;
export const OFFICE_FLOOR_WARP_DOOR_START_X = -OFFICE_HEX_RADIUS / 2 + 3.8;
export const OFFICE_FLOOR_WARP_DOOR_Z = -OFFICE_FRONT_Z + 0.34;
export const OFFICE_FLOOR_WARP_TRIGGER_Z = -OFFICE_FRONT_Z + 1.18;
// The reef is a scene-owned lobby exhibit rather than part of the reusable
// tower shell. Keep its physical footprint beside its scene integration so
// every movement path observes the same cabinet and glass boundary.
export const OFFICE_AQUARIUM_WALL = "back-left";
export const OFFICE_AQUARIUM_POSITION = Object.freeze([
  officeHexWallPoint(OFFICE_AQUARIUM_WALL, 0, 2.1).x,
  0,
  officeHexWallPoint(OFFICE_AQUARIUM_WALL, 0, 2.1).z,
]);
export const OFFICE_AQUARIUM_ROTATION_Y =
  Math.PI - officeHexSide(OFFICE_AQUARIUM_WALL).angle;
export const OFFICE_AQUARIUM_HALF_DEPTH = 1.66;
export const OFFICE_AQUARIUM_HALF_LENGTH = 13.8;
export const OFFICE_AQUARIUM_BOUNDS = Object.freeze({
  minX: -OFFICE_AQUARIUM_HALF_DEPTH,
  maxX: OFFICE_AQUARIUM_HALF_DEPTH,
  minZ: -OFFICE_AQUARIUM_HALF_LENGTH,
  maxZ: OFFICE_AQUARIUM_HALF_LENGTH,
});

export function officeAquariumLocalPoint(x, z, target = {}) {
  const dx = Number(x) - OFFICE_AQUARIUM_POSITION[0];
  const dz = Number(z) - OFFICE_AQUARIUM_POSITION[2];
  const cos = Math.cos(OFFICE_AQUARIUM_ROTATION_Y);
  const sin = Math.sin(OFFICE_AQUARIUM_ROTATION_Y);
  target.x = dx * cos - dz * sin;
  target.z = dx * sin + dz * cos;
  return target;
}

export const LIGHT_LEVEL_MIN = 40;
export const LIGHT_LEVEL_MAX = 140;
export const LIGHT_LEVEL_DEFAULT = 100;
