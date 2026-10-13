import {
  AVATAR_ARM_LENGTH,
  AVATAR_SHOULDER_Y,
  applyLegPitch,
  applySeatedLegPose,
  poseAvatarArms,
  seatedAvatarY,
} from "../avatars.js";
import {
  BIKE_RIDING_ACTIVITY,
  WORLD_BIKE_MOUNT_DISTANCE,
} from "../gameplay-constants.js";
import { makeLabelSprite } from "../labels.js";
import { WORLD_ROAD_SURFACE_Y } from "../layout.js";
import { makeSharedMaterial } from "../materials.js";
import { roundedHexagonLoopLength } from "../movement.js";
import { setShadows } from "../object-utils.js";
import * as resources from "../resources.js";
import {
  createRoadRoute,
  createRoadSnapHit,
  roadRouteAdvance,
  roadRoutePlaceOnLoop,
  roadRoutePoint,
  roadRouteSnap,
  roadRouteTurnPending,
  roadSnapFind,
  worldRoadSnapGrid,
} from "./roads.js";

export const BIKE_SEAT_Y = 1.34;
export const BIKE_ARM_PITCH = 0.75;
export const BIKE_GRIP_Y =
  seatedAvatarY(BIKE_SEAT_Y + 0.05) +
  AVATAR_SHOULDER_Y -
  AVATAR_ARM_LENGTH * Math.cos(BIKE_ARM_PITCH);
export const BIKE_GRIP_Z = -AVATAR_ARM_LENGTH * Math.sin(BIKE_ARM_PITCH);
export const BIKE_STEER_PIVOT_Y = 1.1;
export const BIKE_GRIP_X = 0.62;
export const BIKE_TURN_BAR_YAW = 0.34;
export const BIKE_STEER_BAR_YAW = 0.16;
export const BIKE_TURN_LEAN = 0.09;
const BIKE_POSE_RATE = 7;
export const BIKE_LEAVE_ROAD_HOLD_SECONDS = 0.45;
export const BIKE_LEAVE_ROAD_STEER = 0.6;
export const BIKE_OFFROAD_TURN_RATE = 1.6;
export const BIKE_SNAP_BLEND_RATE = 4;

export function createBikeSteerHold() {
  return { steerSide: 0, steerHeld: 0, steerArmed: true };
}

export function bikeSteerHold(hold, steer, delta) {
  const value = Number(steer) || 0;
  const side =
    value >= BIKE_LEAVE_ROAD_STEER ? 1 : value <= -BIKE_LEAVE_ROAD_STEER ? -1 : 0;
  if (!side) {
    hold.steerSide = 0;
    hold.steerHeld = 0;
    hold.steerArmed = true;
    return false;
  }
  if (side !== hold.steerSide) {
    hold.steerSide = side;
    hold.steerHeld = 0;
  }
  hold.steerHeld += Math.max(0, Number(delta) || 0);
  return hold.steerArmed && hold.steerHeld >= BIKE_LEAVE_ROAD_HOLD_SECONDS;
}

export function bikeOffRoadStep(pose, steer, distance, delta) {
  const input = Math.max(-1, Math.min(1, Number(steer) || 0));
  const heading =
    pose.heading - input * BIKE_OFFROAD_TURN_RATE * Math.max(0, Number(delta) || 0);
  pose.heading = Math.atan2(Math.sin(heading), Math.cos(heading));
  const step = Number(distance) || 0;
  pose.x -= Math.sin(pose.heading) * step;
  pose.z -= Math.cos(pose.heading) * step;
  return pose;
}

export function bikeSnapBlendStart(blend, x, z, heading, point) {
  blend.x = x - point.x;
  blend.z = z - point.z;
  const turn = heading - point.heading;
  blend.heading = Math.atan2(Math.sin(turn), Math.cos(turn));
  return blend;
}

export function bikeSnapBlendDecay(blend, delta) {
  const keep = Math.exp(-BIKE_SNAP_BLEND_RATE * Math.max(0, Number(delta) || 0));
  blend.x *= keep;
  blend.z *= keep;
  blend.heading *= keep;
  if (Math.abs(blend.x) + Math.abs(blend.z) + Math.abs(blend.heading) < 1e-3) {
    blend.x = 0;
    blend.z = 0;
    blend.heading = 0;
  }
  return blend;
}

export function bikeCueSide(state) {
  if (state.offRoad) return Math.max(-1, Math.min(1, Number(state.steer) || 0));
  return roadRouteTurnPending(state.route);
}

export function bikeSteeringTargets(pendingSide, steer, target) {
  const input = Math.max(-1, Math.min(1, Number(steer) || 0));
  target.barYaw = -(pendingSide * BIKE_TURN_BAR_YAW + input * BIKE_STEER_BAR_YAW);
  target.lean = -pendingSide * BIKE_TURN_LEAN;
  return target;
}

export function bikeGripArmPitch(barYaw, side) {
  const reach =
    (side * BIKE_GRIP_X * Math.sin(barYaw)) /
    (AVATAR_ARM_LENGTH * Math.cos(BIKE_ARM_PITCH));
  return BIKE_ARM_PITCH + reach;
}

function addBikeHandlebar(THREE, bike, index, interactive) {
  const steel = resources.material(THREE, {
    color: "#c9d1d4",
    metalness: 0.6,
    roughness: 0.3,
  });
  const rubber = resources.material(THREE, {
    color: "#151a1a",
    roughness: 0.85,
  });
  const stemHeight = BIKE_GRIP_Y - BIKE_STEER_PIVOT_Y;
  const steering = new THREE.Group();
  steering.name = "forkmesh-world-bike-handlebar";
  steering.position.set(0, BIKE_STEER_PIVOT_Y, BIKE_GRIP_Z);
  const parts = [
    [
      resources.primitive(THREE, "CylinderGeometry", 0.05, 0.05, BIKE_STEER_PIVOT_Y - 0.62, 8),
      steel, 0, -(BIKE_STEER_PIVOT_Y - 0.62) / 2, 0, 0,
    ],
    [
      resources.primitive(THREE, "CylinderGeometry", 0.055, 0.055, stemHeight, 10),
      steel, 0, stemHeight / 2, 0, 0,
    ],
    [
      resources.primitive(THREE, "CylinderGeometry", 0.045, 0.045, BIKE_GRIP_X * 2, 10),
      steel, 0, stemHeight, 0, Math.PI / 2,
    ],
    [
      resources.primitive(THREE, "CylinderGeometry", 0.075, 0.075, 0.26, 10),
      rubber, -BIKE_GRIP_X, stemHeight, 0, Math.PI / 2,
    ],
    [
      resources.primitive(THREE, "CylinderGeometry", 0.075, 0.075, 0.26, 10),
      rubber, BIKE_GRIP_X, stemHeight, 0, Math.PI / 2,
    ],
  ];
  for (const [geometry, material, x, y, z, roll] of parts) {
    const part = new THREE.Mesh(geometry, material);
    part.position.set(x, y, z);
    part.rotation.z = roll;
    part.userData.bikeIndex = index;
    steering.add(part);
    interactive.push(part);
  }
  bike.add(steering);
  return steering;
}

export function functions({
  THREE,
  ctx,
  fns,
}) {
  const bikeLanePoint = { x: 0, z: 0, heading: 0 };
  const bikeSteering = { barYaw: 0, lean: 0 };
  const offRoadPose = { x: 0, z: 0, heading: 0 };
  const snapHit = createRoadSnapHit();

  function setBikeOnRoute(state) {
    roadRoutePoint(state.route, bikeLanePoint);
    const blend = state.blend;
    const heading = bikeLanePoint.heading + blend.heading;
    state.bike.position.set(
      bikeLanePoint.x + blend.x,
      WORLD_ROAD_SURFACE_Y + 0.06,
      bikeLanePoint.z + blend.z,
    );
    state.bike.rotation.y = Math.atan2(Math.sin(heading), Math.cos(heading));
  }

  function leaveRoad(state) {
    state.offRoad = true;
    state.snapArmed = false;
    state.blend.x = 0;
    state.blend.z = 0;
    state.blend.heading = 0;
    state.route.choice = 0;
  }

  function rideBikeOffRoad(state, distance, delta) {
    offRoadPose.x = state.bike.position.x;
    offRoadPose.z = state.bike.position.z;
    offRoadPose.heading = state.bike.rotation.y;
    bikeOffRoadStep(offRoadPose, state.steer, distance, delta);
    const aligned = roadSnapFind(
      worldRoadSnapGrid(),
      offRoadPose.x,
      offRoadPose.z,
      offRoadPose.heading,
      snapHit,
    );
    if (!state.snapArmed) {
      if (!snapHit.near) state.snapArmed = true;
    } else if (aligned) {
      roadRouteSnap(state.route, snapHit);
      roadRoutePoint(state.route, bikeLanePoint);
      bikeSnapBlendStart(
        state.blend,
        offRoadPose.x,
        offRoadPose.z,
        offRoadPose.heading,
        bikeLanePoint,
      );
      state.offRoad = false;
      state.hold.steerArmed = false;
      setBikeOnRoute(state);
      return;
    }
    state.bike.position.set(
      offRoadPose.x,
      ctx.currentFloorY + 0.06,
      offRoadPose.z,
    );
    state.bike.rotation.y = offRoadPose.heading;
  }

  function placeBikeOnLane(state, distance) {
    const along =
      (((Number(distance) || 0) % ctx.bikeLoopLength) + ctx.bikeLoopLength) %
      ctx.bikeLoopLength;
    state.along = along;
    state.offRoad = false;
    state.blend.x = 0;
    state.blend.z = 0;
    state.blend.heading = 0;
    roadRoutePlaceOnLoop(state.route, along);
    setBikeOnRoute(state);
  }

  function rideBikeAlongRoad(state, distance, steer, delta = 0) {
    state.steer = Number(steer) || 0;
    const leave = bikeSteerHold(state.hold, state.steer, delta);
    if (!state.offRoad && leave) leaveRoad(state);
    if (state.offRoad) {
      rideBikeOffRoad(state, distance, delta);
      return;
    }
    roadRouteAdvance(state.route, distance, state.steer);
    bikeSnapBlendDecay(state.blend, delta);
    setBikeOnRoute(state);
  }
  function addWorldBike(index, color, along) {
    const bike = new THREE.Group();
    bike.name = `forkmesh-world-bike-${index + 1}`;
    const frameMaterial = makeSharedMaterial(THREE, color, {
      metalness: 0.55,
      roughness: 0.32,
    });
    const tireMaterial = makeSharedMaterial(THREE, "#111716", {
      roughness: 0.8,
    });
    const wheels = [];
    for (const z of [-0.82, 0.82]) {
      const wheel = new THREE.Mesh(
        resources.primitive(THREE, "TorusGeometry", 0.58, 0.075, 8, 24),
        tireMaterial,
      );
      wheel.rotation.y = Math.PI / 2;
      wheel.position.set(0, 0.62, z);
      wheel.userData.bikeIndex = index;
      bike.add(wheel);
      wheels.push(wheel);
      ctx.interactive.push(wheel);
    }
    for (const [x, y, z, width, height, depth, rotation] of [
      [0, 0.82, 0, 0.08, 1.1, 0.08, 0],
      [0, 0.85, -0.38, 0.08, 0.08, 1.05, -0.48],
      [0, 0.85, 0.38, 0.08, 0.08, 1.05, 0.48],
    ]) {
      const bar = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", width, height, depth),
        frameMaterial,
      );
      bar.position.set(x, y, z);
      bar.rotation.x = rotation;
      bar.userData.bikeIndex = index;
      bike.add(bar);
      ctx.interactive.push(bar);
    }
    const seat = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.44, 0.1, 0.28),
      makeSharedMaterial(THREE, "#2b2420", { roughness: 0.72 }),
    );
    seat.position.set(0, BIKE_SEAT_Y, 0.18);
    const handlebar = addBikeHandlebar(THREE, bike, index, ctx.interactive);
    seat.userData.bikeIndex = index;
    bike.add(seat);
    ctx.interactive.push(seat);
    bike.userData.bikeIndex = index;
    const hint = makeLabelSprite(
      THREE,
      "RIDE BIKE",
      "CLICK OR E · W/S PEDAL · A/D STEER",
      color,
    );
    hint.name = `forkmesh-world-bike-${index + 1}-control-hint`;
    hint.position.set(0, 3.25, 0);
    hint.scale.set(5.6, 1.9, 1);
    bike.add(hint);
    setShadows(bike);
    ctx.world.add(bike);
    fns.registerWorldElement("bikes", "World bikes", "Vehicles & rides", bike);
    const state = {
      bike,
      wheels,
      seat,
      hint,
      handlebar,
      moving: false,
      along: 0,
      steer: 0,
      barYaw: 0,
      lean: 0,
      route: createRoadRoute(),
      offRoad: false,
      snapArmed: false,
      hold: createBikeSteerHold(),
      blend: { x: 0, z: 0, heading: 0 },
    };
    placeBikeOnLane(state, along * ctx.bikeLoopLength);
    ctx.bikeStates.push(state);
  }

  function rideBike(index) {
    if (ctx.officeSceneMode !== "town" || ctx.coasterRide) return;
    const state = ctx.bikeStates[index];
    if (!state) return;
    if (ctx.bikeRide?.index === index) {
      dismountBike();
      return;
    }
    fns.dismountSwing({ relocate: false });
    fns.standUpFromBench();
    fns.dismountCar({ relocate: false });
    fns.dismountQuadcopter({ relocate: false });
    ctx.bikeRide = { index };
    state.hint.visible = false;
    fns.completeStartHereStep("explore");
    fns.cancelDash();
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    ctx.player.position.x = state.bike.position.x;
    ctx.player.position.z = state.bike.position.z;
    ctx.player.rotation.y = state.bike.rotation.y;
    if (ctx.cameraMode === "first-person") {
      ctx.cameraYaw = Math.atan2(
        Math.sin(ctx.player.rotation.y),
        Math.cos(ctx.player.rotation.y),
      );
    }
    applyBikeRidePose(false, 0);
    ctx.lastPosition.copy(ctx.player.position);
    fns.queueMovementEvent({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving: false,
      activity: BIKE_RIDING_ACTIVITY,
    });
  }

  function toggleNearestBikeRide() {
    if (ctx.bikeRide) {
      dismountBike();
      return true;
    }
    if (ctx.officeSceneMode !== "town") return false;
    let nearestIndex = -1;
    let nearestDistanceSquared = WORLD_BIKE_MOUNT_DISTANCE ** 2;
    ctx.bikeStates.forEach((state, index) => {
      const distanceSquared =
        (state.bike.position.x - ctx.player.position.x) ** 2 +
        (state.bike.position.z - ctx.player.position.z) ** 2;
      if (distanceSquared > nearestDistanceSquared) return;
      nearestDistanceSquared = distanceSquared;
      nearestIndex = index;
    });
    if (nearestIndex < 0) return false;
    rideBike(nearestIndex);
    return true;
  }

  function applyBikeRidePose(moving, delta) {
    if (!ctx.bikeRide) return;
    const state = ctx.bikeStates[ctx.bikeRide.index];
    if (!state) return;
    state.bike.position.x = ctx.player.position.x;
    state.bike.position.z = ctx.player.position.z;
    state.bike.rotation.y = ctx.player.rotation.y;
    state.moving = Boolean(moving);
    if (moving) {
      const rotation =
        Math.max(0, delta) *
        ctx.keyboardMovementSpeed *
        (state.travelDirection || 1) /
        0.58;
      state.wheels.forEach((wheel) => {
        wheel.rotation.x -= rotation;
      });
    }
    bikeSteeringTargets(
      bikeCueSide(state),
      state.offRoad ? 0 : state.steer,
      bikeSteering,
    );
    const blend = 1 - Math.exp(-BIKE_POSE_RATE * Math.max(0, delta));
    state.barYaw += (bikeSteering.barYaw - state.barYaw) * blend;
    state.lean += (bikeSteering.lean - state.lean) * blend;
    state.handlebar.rotation.y = state.barYaw;
    state.bike.rotation.z = state.lean;
    const seatTop = state.seat.getWorldPosition(ctx.bikeSeatPosition);
    ctx.player.position.y = seatedAvatarY(seatTop.y + 0.05, ctx.player.scale.x);
    ctx.player.rotation.order = "YXZ";
    ctx.player.rotation.x = 0;
    ctx.player.rotation.z = state.lean;
    poseAvatarArms(
      ctx.player,
      bikeGripArmPitch(state.barYaw, -1),
      bikeGripArmPitch(state.barYaw, 1),
    );
    applySeatedLegPose(ctx.player);
  }

  function dismountBike({ relocate = true } = {}) {
    if (!ctx.bikeRide) return;
    const state = ctx.bikeStates[ctx.bikeRide.index];
    ctx.bikeRide = null;
    if (state) state.moving = false;
    if (state) {
      state.travelDirection = 0;
      state.steer = 0;
      state.hold.steerSide = 0;
      state.hold.steerHeld = 0;
      state.hold.steerArmed = true;
      state.barYaw = 0;
      state.lean = 0;
      state.handlebar.rotation.y = 0;
      state.bike.rotation.z = 0;
    }
    if (state?.hint) state.hint.visible = true;
    ctx.player.rotation.x = 0;
    ctx.player.rotation.z = 0;
    poseAvatarArms(ctx.player, 0, 0);
    applyLegPitch(ctx.player, 0, 0);
    if (relocate && state) {
      ctx.player.position.x += Math.cos(ctx.player.rotation.y) * 1.15;
      ctx.player.position.z -= Math.sin(ctx.player.rotation.y) * 1.15;
    }
    ctx.player.position.y = ctx.currentFloorY;
    ctx.lastPosition.copy(ctx.player.position);
  }

  return {
    placeBikeOnLane,
    rideBikeAlongRoad,
    addWorldBike,
    rideBike,
    toggleNearestBikeRide,
    applyBikeRidePose,
    dismountBike,
  };
}

export function build({
  addWorldBike,
  ctx,
}) {
  const bikeStates = [];
  ctx.bikeStates = bikeStates;
  const bikeLoopLength = roundedHexagonLoopLength();
  ctx.bikeLoopLength = bikeLoopLength;
  addWorldBike(0, "#77d9ff", 0.38);
  addWorldBike(1, "#9ef7c6", 0.43);
  return {
  };
}
