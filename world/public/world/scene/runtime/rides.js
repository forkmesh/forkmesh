import { SWING_RIDING_ACTIVITY } from "../../world-data.js";
import {
  OFFICE_AVATAR_RADIUS,
  OFFICE_ISLAND_CENTER,
} from "../../world-office-tower.js";
import {
  GAIT_LEG_SWING,
  animateAvatarActivity,
  applyLegPitch,
  applySeatedLegPose,
  poseAvatarArms,
  seatedAvatarY,
} from "../avatars.js";
import {
  CAMERA_PITCH_MAX,
  CAMERA_PITCH_MIN,
  CAR_RIDING_ACTIVITY,
  GYM_BENCH_LIE_POSITION,
  GYM_BENCH_PRESS_DEPTH,
  GYM_BENCH_REP_MS,
  GYM_HEAVY_WEIGHT_LB,
  GYM_MAX_WEIGHT_LB,
  JETPACK_FLYING_ACTIVITY,
  JETPACK_HORIZONTAL_SPEED,
  JETPACK_MAX_ALTITUDE,
  JETPACK_VERTICAL_SPEED,
  QUADCOPTER_HORIZONTAL_SPEED,
  QUADCOPTER_MAX_ALTITUDE,
  QUADCOPTER_MOUNT_DISTANCE,
  QUADCOPTER_REST_ALTITUDE,
  QUADCOPTER_RIDING_ACTIVITY,
  QUADCOPTER_SHELL_RADIUS,
  QUADCOPTER_VERTICAL_SPEED,
} from "../gameplay-constants.js";
import { CAMPFIRE_GROUND_Y, CONTINUOUS_CITY_CENTER_Z } from "../layout.js";
import {
  parkedQuadcopterPoint,
  quadcopterCruiseSpeed,
  quadcopterFlightPitch,
  quadcopterFlightVector,
  worldRadiusForSpace,
} from "../movement.js";
import { clamp } from "../object-utils.js";

export function functions({
  THREE,
  onGymState,
  onJetpackChange,
  onMovement,
  onSwingRide,
  reducedMotion,
  ctx,
  fns,
}) {
  const SWING_MIN_AMPLITUDE = 0.14;
  const SWING_MAX_AMPLITUDE = 1.05;
  let jetpackVerticalControl = 0;


  function swingRideAmplitude() {
    return (
      SWING_MIN_AMPLITUDE +
      (SWING_MAX_AMPLITUDE - SWING_MIN_AMPLITUDE) * ctx.swingSpeedLevel
    );
  }

  function setSwingSpeed(value) {
    const numeric = Number(value);
    if (!Number.isFinite(numeric)) return;
    ctx.swingSpeedLevel = Math.min(1, Math.max(0.1, numeric / 100));
  }

  function swingSeatRestPoint(index) {
    return ctx.swingStates[index].anchor.getWorldPosition(new THREE.Vector3());
  }

  // Riding is a local pose like the campfire sit, but on a moving seat: the
  // avatar is re-glued to the plank every frame, so both camera modes follow
  // the arc for free. A second click on the same swing hops off.
  function rideSwing(index) {
    if (ctx.officeSceneMode !== "town" || ctx.coasterRide) return;
    const swing = ctx.swingStates[index];
    if (!swing) return;
    if (ctx.swingRide?.index === index) {
      dismountSwing();
      return;
    }
    if (swing.remoteId) {
      onSwingRide({
        riding: Boolean(ctx.swingRide),
        seat: ctx.swingRide ? ctx.swingRide.index : -1,
        denied: true,
      });
      return;
    }
    fns.dismountBike({ relocate: false });
    fns.standUpFromBench();
    ctx.swingRide = { index };
    fns.cancelDash();
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    applySwingRidePose();
    ctx.lastPosition.copy(ctx.player.position);
    onMovement({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving: false,
      activity: SWING_RIDING_ACTIVITY,
    });
    onSwingRide({ riding: true, seat: index, denied: false });
  }

  function applySwingRidePose() {
    if (!ctx.swingRide) return;
    const swing = ctx.swingStates[ctx.swingRide.index];
    const seatTop = swing.seat.getWorldPosition(new THREE.Vector3());
    ctx.player.position.set(
      seatTop.x,
      seatedAvatarY(seatTop.y + ctx.SWING_SEAT_HALF_THICKNESS, ctx.player.scale.x),
      seatTop.z,
    );
    // Yaw first, then pitch in the yawed frame: with the default XYZ order the
    // lean below would be a world-X pitch, which on a swing set that faces the
    // fountain at an angle reads as the rider rocking side to side.
    ctx.player.rotation.order = "YXZ";
    ctx.player.rotation.y = ctx.swingSet.rotation.y;
    // Lean with the ropes so the body traces the arc instead of staying bolt
    // upright at the peaks.
    ctx.player.rotation.x = swing.pivot.rotation.x;
    ctx.player.userData.leftArm.rotation.x = ctx.SWING_ARM_HOLD_PITCH;
    ctx.player.userData.rightArm.rotation.x = ctx.SWING_ARM_HOLD_PITCH;
    applySeatedLegPose(ctx.player);
  }

  function dismountSwing({ relocate = true } = {}) {
    if (!ctx.swingRide) return;
    const index = ctx.swingRide.index;
    ctx.swingRide = null;
    ctx.player.rotation.x = 0;
    ctx.player.userData.leftArm.rotation.x = 0;
    ctx.player.userData.rightArm.rotation.x = 0;
    applyLegPitch(ctx.player, 0, 0);
    if (relocate) {
      // Step off just in front of the seat's rest spot rather than standing
      // up inside the moving plank.
      const rest = swingSeatRestPoint(index);
      const heading = ctx.swingSet.rotation.y;
      ctx.player.position.set(
        rest.x - Math.sin(heading) * 1.1,
        ctx.currentFloorY,
        rest.z - Math.cos(heading) * 1.1,
      );
      ctx.player.rotation.y = heading;
      ctx.lastPosition.copy(ctx.player.position);
      fns.queueMovementEvent({
        x: Number(ctx.player.position.x.toFixed(2)),
        y: Number(ctx.player.position.y.toFixed(2)),
        z: Number(ctx.player.position.z.toFixed(2)),
        heading: Number(ctx.player.rotation.y.toFixed(3)),
        space: ctx.currentSpace,
        moving: false,
        activity:
          ctx.currentLocation === "Town Square"
            ? "exploring the Town Square"
            : `visiting ${ctx.currentLocation}`,
      });
    } else {
      ctx.player.position.y = ctx.currentFloorY;
    }
    onSwingRide({ riding: false, seat: -1, denied: false });
  }

  function rideCar(index) {
    if (ctx.officeSceneMode !== "town" || ctx.coasterRide) return;
    const state = ctx.carStates[index];
    if (!state) return;
    if (ctx.carRide?.index === index) {
      dismountCar();
      return;
    }
    dismountSwing({ relocate: false });
    fns.dismountBike({ relocate: false });
    dismountQuadcopter({ relocate: false });
    fns.standUpFromBench();
    ctx.carRide = { index };
    if (state.hint) state.hint.visible = false;
    fns.completeStartHereStep("explore");
    fns.cancelDash();
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    ctx.player.position.x = state.car.position.x;
    ctx.player.position.z = state.car.position.z;
    ctx.player.rotation.y = state.car.rotation.y;
    applyCarRidePose(false, 0);
    ctx.lastPosition.copy(ctx.player.position);
    fns.queueMovementEvent({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving: false,
      activity: CAR_RIDING_ACTIVITY,
    });
  }

  function applyCarRidePose(moving, delta) {
    if (!ctx.carRide) return;
    const state = ctx.carStates[ctx.carRide.index];
    if (!state) return;
    state.car.position.x = ctx.player.position.x;
    state.car.position.z = ctx.player.position.z;
    state.car.rotation.y = ctx.player.rotation.y;
    state.moving = Boolean(moving);
    if (moving) {
      const rotation = Math.max(0, delta) * ctx.keyboardMovementSpeed / 0.58;
      state.wheels.forEach((wheel) => {
        wheel.rotation.x -= rotation;
      });
    }
    ctx.player.position.y = seatedAvatarY(1.72, ctx.player.scale.x);
    ctx.player.rotation.order = "YXZ";
    ctx.player.rotation.x = 0;
    ctx.player.userData.leftArm.rotation.x = -0.22;
    ctx.player.userData.rightArm.rotation.x = -0.22;
    applySeatedLegPose(ctx.player);
  }

  function dismountCar({ relocate = true } = {}) {
    if (!ctx.carRide) return;
    const state = ctx.carStates[ctx.carRide.index];
    ctx.carRide = null;
    if (state) state.moving = false;
    if (state?.hint) state.hint.visible = true;
    ctx.player.rotation.x = 0;
    ctx.player.userData.leftArm.rotation.x = 0;
    ctx.player.userData.rightArm.rotation.x = 0;
    applyLegPitch(ctx.player, 0, 0);
    if (relocate && state) {
      ctx.player.position.x += Math.cos(ctx.player.rotation.y) * 2.5;
      ctx.player.position.z -= Math.sin(ctx.player.rotation.y) * 2.5;
    }
    ctx.player.position.y = ctx.currentFloorY;
    ctx.lastPosition.copy(ctx.player.position);
  }

  function rideQuadcopter(index) {
    if (ctx.officeSceneMode !== "town" || ctx.coasterRide) return;
    const state = ctx.quadcopterStates[index];
    if (!state) return;
    if (ctx.quadcopterRide?.index === index) {
      dismountQuadcopter();
      return;
    }
    dismountSwing({ relocate: false });
    fns.dismountBike({ relocate: false });
    dismountCar({ relocate: false });
    fns.standUpFromBench();
    ctx.quadcopterRide = { index, cruiseSeconds: 0, speed: 0 };
    state.hint.visible = false;
    fns.completeStartHereStep("explore");
    fns.cancelDash();
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    fns.removeRoofParachute();
    ctx.player.position.set(
      state.quadcopter.position.x,
      state.quadcopter.position.y + 1.02,
      state.quadcopter.position.z,
    );
    ctx.player.rotation.y = state.quadcopter.rotation.y;
    ctx.player.userData.leftArm.rotation.x = -0.28;
    ctx.player.userData.rightArm.rotation.x = -0.28;
    applySeatedLegPose(ctx.player);
    ctx.lastPosition.copy(ctx.player.position);
    ctx.cameraSnapPending = true;
    fns.queueMovementEvent({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving: false,
      activity: QUADCOPTER_RIDING_ACTIVITY,
    });
  }

  function toggleNearestQuadcopterRide() {
    if (ctx.quadcopterRide) {
      dismountQuadcopter();
      return true;
    }
    if (ctx.officeSceneMode !== "town") return false;
    let nearestIndex = -1;
    let nearestDistanceSquared = QUADCOPTER_MOUNT_DISTANCE ** 2;
    ctx.quadcopterStates.forEach((state, index) => {
      const distanceSquared =
        (state.quadcopter.position.x - ctx.player.position.x) ** 2 +
        (state.quadcopter.position.z - ctx.player.position.z) ** 2 +
        (state.quadcopter.position.y - ctx.player.position.y) ** 2;
      if (distanceSquared > nearestDistanceSquared) return;
      nearestDistanceSquared = distanceSquared;
      nearestIndex = index;
    });
    if (nearestIndex < 0) return false;
    rideQuadcopter(nearestIndex);
    return true;
  }

  // Stepping off has to leave the vehicle somewhere its own pilot can reach on
  // foot. Nothing else ever moves a quadcopter, and toggleNearestQuadcopterRide
  // measures a three-axis distance, so one abandoned in the air above the
  // Office - or standing inside the tower's keep-out footprint - stays visible
  // and permanently unmountable.
  function parkQuadcopter(state) {
    if (!state?.quadcopter) return;
    const office = ctx.landmarkObjects.get("office");
    const parked = parkedQuadcopterPoint(
      state.quadcopter.position.x,
      state.quadcopter.position.z,
      {
        officeX: office ? office.position.x : OFFICE_ISLAND_CENTER[0],
        officeZ: office ? office.position.z : OFFICE_ISLAND_CENTER[2],
        walkSurfaceContains: ctx.activeWalkSurfaceContains,
      },
    );
    state.quadcopter.position.set(
      parked.x,
      QUADCOPTER_REST_ALTITUDE,
      parked.z,
    );
  }

  function dismountQuadcopter({ relocate = true } = {}) {
    if (!ctx.quadcopterRide) return;
    const state = ctx.quadcopterStates[ctx.quadcopterRide.index];
    ctx.quadcopterRide = null;
    if (state) state.moving = false;
    if (state?.hint) state.hint.visible = true;
    parkQuadcopter(state);
    ctx.player.userData.leftArm.rotation.x = 0;
    ctx.player.userData.rightArm.rotation.x = 0;
    applyLegPitch(ctx.player, 0, 0);
    if (relocate && state) {
      // Land beside the parked airframe rather than the airborne coordinate the
      // ride ended on, so the visitor and the vehicle stay within mount range.
      ctx.player.position.x =
        state.quadcopter.position.x + Math.cos(ctx.player.rotation.y) * 2.2;
      ctx.player.position.z =
        state.quadcopter.position.z - Math.sin(ctx.player.rotation.y) * 2.2;
    }
    if (ctx.player.position.y > ctx.currentFloorY + 2) {
      fns.prepareRoofParachute();
      ctx.jumpVelocity = -0.4;
    } else {
      ctx.player.position.y = ctx.currentFloorY;
      ctx.jumpVelocity = 0;
    }
    ctx.lastPosition.copy(ctx.player.position);
  }

  function updateQuadcopterRide(input, delta, time) {
    if (!ctx.quadcopterRide) return false;
    const state = ctx.quadcopterStates[ctx.quadcopterRide.index];
    if (!state) {
      ctx.quadcopterRide = null;
      return false;
    }
    ctx.cameraFocus = null;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
    ctx.jumpQueued = false;
    ctx.jumpVelocity = 0;
    ctx.flightPreviousPosition.copy(state.quadcopter.position);
    const forwardInput = input.movement.dot(ctx.movementForward);
    const rightInput = input.movement.dot(ctx.movementRight);
    ctx.quadcopterRide.cruiseSeconds =
      forwardInput > 0 ? ctx.quadcopterRide.cruiseSeconds + delta : 0;
    const forwardSpeed =
      forwardInput > 0
        ? quadcopterCruiseSpeed(ctx.quadcopterRide.cruiseSeconds)
        : QUADCOPTER_HORIZONTAL_SPEED;
    ctx.quadcopterRide.speed = input.movement.lengthSq() ? forwardSpeed : 0;
    const flight = quadcopterFlightVector(
      ctx.quadcopterFlight,
      ctx.cameraYaw,
      quadcopterFlightPitch(ctx.cameraMode, ctx.cameraPitch, ctx.firstPersonPitch),
      forwardInput * forwardSpeed,
      rightInput * QUADCOPTER_HORIZONTAL_SPEED,
    );
    const vertical =
      Number(ctx.keys.has("Space")) -
      Number(ctx.keys.has("KeyC") || ctx.keys.has("ControlLeft") || ctx.keys.has("ControlRight"));
    const moving = flight.lengthSq() > 0 || vertical !== 0;
    if (flight.lengthSq()) {
      state.quadcopter.position.x += flight.x * delta;
      state.quadcopter.position.z += flight.z * delta;
      const distance = Math.hypot(
        state.quadcopter.position.x,
        state.quadcopter.position.z,
      );
      const boundary = worldRadiusForSpace(ctx.currentSpace) - 12;
      if (distance > boundary) {
        const scale = boundary / Math.max(1, distance);
        state.quadcopter.position.x *= scale;
        state.quadcopter.position.z *= scale;
      }
      if (flight.x * flight.x + flight.z * flight.z > 1e-6) {
        state.quadcopter.rotation.y = Math.atan2(-flight.x, -flight.z);
      }
    }
    state.quadcopter.position.y = clamp(
      state.quadcopter.position.y +
        (flight.y + vertical * QUADCOPTER_VERTICAL_SPEED) * delta,
      QUADCOPTER_REST_ALTITUDE,
      QUADCOPTER_MAX_ALTITUDE,
    );
    // The Office tower is solid to aircraft. A rider may fly over its roof but
    // never through the façade into a floor the elevator would refuse.
    fns.constrainFlightToOfficeShell(
      state.quadcopter.position,
      ctx.flightPreviousPosition,
      QUADCOPTER_SHELL_RADIUS,
    );
    state.moving = moving;
    const rotorSpeed = moving ? 0.42 : 0.19;
    state.rotors.forEach((rotor, index) => {
      rotor.rotation.y += rotorSpeed * (index % 2 ? -1 : 1);
    });
    ctx.player.position.set(
      state.quadcopter.position.x,
      state.quadcopter.position.y + 1.02,
      state.quadcopter.position.z,
    );
    ctx.player.rotation.y = state.quadcopter.rotation.y;
    ctx.player.userData.leftArm.rotation.x = -0.28;
    ctx.player.userData.rightArm.rotation.x = -0.28;
    applySeatedLegPose(ctx.player);
    ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
    animateAvatarActivity(ctx.player, time, delta, reducedMotion);
    if (
      performance.now() - ctx.lastMovementEmit > 250 &&
      ctx.player.position.distanceToSquared(ctx.lastPosition) > 0.025
    ) {
      ctx.lastMovementEmit = performance.now();
      ctx.lastPosition.copy(ctx.player.position);
      fns.queueMovementEvent({
        x: Number(ctx.player.position.x.toFixed(2)),
        y: Number(ctx.player.position.y.toFixed(2)),
        z: Number(ctx.player.position.z.toFixed(2)),
        heading: Number(ctx.player.rotation.y.toFixed(3)),
        space: ctx.currentSpace,
        moving,
        activity: QUADCOPTER_RIDING_ACTIVITY,
        race: fns.droneRacePresence(),
      });
    }
    ctx.wasWalking = moving;
    return true;
  }

  function setJetpackEquipped(equipped) {
    const next = equipped === true;
    if (next === ctx.jetpackEquipped) return ctx.jetpackEquipped;
    if (next && ctx.officeSceneMode !== "town") return false;
    if (next) {
      dismountSwing({ relocate: false });
      fns.dismountBike({ relocate: false });
      dismountCar({ relocate: false });
      dismountQuadcopter({ relocate: false });
      fns.standUpFromBench();
      stopGymExercise();
      fns.cancelDash();
      fns.removeRoofParachute();
      ctx.jumpVelocity = 0;
      ctx.jumpQueued = false;
      ctx.superJumping = false;
    } else {
      // Flight may cross water or scenery that is intentionally not walkable.
      // A mid-air unequip keeps its altitude but nudges the parachute column
      // toward the nearest point on the continuous city instead of eventually
      // marooning the player on a non-walkable landing coordinate.
      if (
        !ctx.activeWalkSurfaceContains(
          ctx.player.position.x,
          ctx.player.position.z,
          OFFICE_AVATAR_RADIUS,
        )
      ) {
        const startX = ctx.player.position.x;
        const startZ = ctx.player.position.z;
        for (let step = 1; step <= 80; step += 1) {
          const progress = step / 80;
          const x = startX + (0 - startX) * progress;
          const z =
            startZ +
            (CONTINUOUS_CITY_CENTER_Z - startZ) * progress;
          if (
            ctx.activeWalkSurfaceContains(x, z, OFFICE_AVATAR_RADIUS)
          ) {
            ctx.player.position.x = x;
            ctx.player.position.z = z;
            break;
          }
        }
      }
      if (ctx.player.position.y > ctx.currentFloorY + 1.5) {
        fns.prepareRoofParachute();
        ctx.jumpVelocity = -0.4;
      }
    }
    ctx.jetpackEquipped = next;
    jetpackVerticalControl = 0;
    if (ctx.player.userData.jetpack) {
      ctx.player.userData.jetpack.visible = next;
    }
    if (!next) {
      ctx.player.rotation.z = 0;
      ctx.player.userData.leftArm.rotation.x = 0;
      ctx.player.userData.rightArm.rotation.x = 0;
      applyLegPitch(ctx.player, 0, 0);
    }
    onJetpackChange({
      equipped: next,
      altitude: Math.max(0, ctx.player.position.y - ctx.currentFloorY),
      maxAltitude: JETPACK_MAX_ALTITUDE,
    });
    return ctx.jetpackEquipped;
  }

  function toggleJetpack() {
    return setJetpackEquipped(!ctx.jetpackEquipped);
  }

  function setJetpackVertical(direction, pressed = true) {
    const normalized =
      direction === "up" ? 1 : direction === "down" ? -1 : 0;
    if (!pressed && jetpackVerticalControl === normalized) {
      jetpackVerticalControl = 0;
    } else if (pressed) {
      jetpackVerticalControl = normalized;
    }
    return jetpackVerticalControl;
  }

  function updateJetpackFlight(input, delta, time) {
    if (!ctx.jetpackEquipped) return false;
    ctx.cameraFocus = null;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
    ctx.jumpQueued = false;
    ctx.jumpVelocity = 0;
    ctx.flightPreviousPosition.copy(ctx.player.position);
    const horizontal = ctx.movementVector.copy(input.movement);
    const keyboardVertical =
      Number(ctx.keys.has("Space")) -
      Number(
        ctx.keys.has("KeyC") ||
          ctx.keys.has("ControlLeft") ||
          ctx.keys.has("ControlRight"),
      );
    const vertical = keyboardVertical || jetpackVerticalControl;
    const speedMultiplier = input.sprinting ? 1.55 : 1;
    if (horizontal.lengthSq()) {
      ctx.player.position.addScaledVector(
        horizontal,
        JETPACK_HORIZONTAL_SPEED * speedMultiplier * delta,
      );
      ctx.player.rotation.y = Math.atan2(-horizontal.x, -horizontal.z);
    }
    const worldDistance = Math.hypot(ctx.player.position.x, ctx.player.position.z);
    const boundary = worldRadiusForSpace(ctx.currentSpace) - 6;
    if (worldDistance > boundary) {
      const scale = boundary / Math.max(1, worldDistance);
      ctx.player.position.x *= scale;
      ctx.player.position.z *= scale;
    }
    ctx.player.position.y = clamp(
      ctx.player.position.y +
        vertical * JETPACK_VERTICAL_SPEED * speedMultiplier * delta,
      ctx.currentFloorY + 0.05,
      JETPACK_MAX_ALTITUDE,
    );
    // Wearable flight obeys the same solid tower shell the quadcopter does.
    fns.constrainFlightToOfficeShell(
      ctx.player.position,
      ctx.flightPreviousPosition,
      OFFICE_AVATAR_RADIUS,
    );
    const moving = horizontal.lengthSq() > 0 || vertical !== 0;
    const thrust =
      0.38 +
      (vertical > 0 ? 0.75 : 0) +
      (horizontal.lengthSq() ? 0.34 : 0);
    const jetpack = ctx.player.userData.jetpack;
    jetpack?.userData?.flames?.forEach((flame, index) => {
      const flicker = reducedMotion
        ? 1
        : 0.86 + Math.sin(time * 0.025 + index * 2.1) * 0.14;
      flame.scale.set(
        0.8 + thrust * 0.22,
        Math.max(0.2, thrust * flicker),
        0.8 + thrust * 0.22,
      );
      flame.visible = true;
    });
    ctx.player.userData.leftArm.rotation.x = -0.34;
    ctx.player.userData.rightArm.rotation.x = -0.34;
    applyLegPitch(
      ctx.player,
      0.12 + Math.sin(time * 0.004) * 0.05,
      0.12 - Math.sin(time * 0.004) * 0.05,
    );
    ctx.player.rotation.z =
      horizontal.lengthSq() && !reducedMotion
        ? Math.sin(time * 0.003) * 0.025
        : 0;
    ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
    animateAvatarActivity(ctx.player, time, delta, reducedMotion);
    if (
      performance.now() - ctx.lastMovementEmit > 250 &&
      ctx.player.position.distanceToSquared(ctx.lastPosition) > 0.025
    ) {
      ctx.lastMovementEmit = performance.now();
      ctx.lastPosition.copy(ctx.player.position);
      fns.queueMovementEvent({
        x: Number(ctx.player.position.x.toFixed(2)),
        y: Number(ctx.player.position.y.toFixed(2)),
        z: Number(ctx.player.position.z.toFixed(2)),
        heading: Number(ctx.player.rotation.y.toFixed(3)),
        space: ctx.currentSpace,
        moving,
        activity: JETPACK_FLYING_ACTIVITY,
      });
      onJetpackChange({
        equipped: true,
        altitude: Math.max(0, ctx.player.position.y - ctx.currentFloorY),
        maxAltitude: JETPACK_MAX_ALTITUDE,
      });
    }
    ctx.wasWalking = moving;
    return true;
  }

  function stopGymExercise({ notify = true } = {}) {
    if (!ctx.gymState.active) return false;
    ctx.gymState.active = "";
    ctx.gymState.auto = false;
    ctx.gymState.repStartedAt = 0;
    ctx.player.rotation.x = 0;
    ctx.player.rotation.z = 0;
    poseAvatarArms(ctx.player, 0, 0);
    applyLegPitch(ctx.player, 0, 0);
    if (notify) onGymState(ctx.gymPublicState(false));
    return true;
  }

  function beginGymExercise(kind) {
    if (
      !["bench", "treadmill", "bike", "rower", "punch"].includes(kind) ||
      ctx.officeSceneMode !== "town"
    ) {
      return false;
    }
    if (ctx.jetpackEquipped) setJetpackEquipped(false);
    dismountSwing({ relocate: false });
    fns.dismountBike({ relocate: false });
    dismountCar({ relocate: false });
    dismountQuadcopter({ relocate: false });
    fns.standUpFromBench();
    fns.cancelDash();
    ctx.gymState.active = kind;
    ctx.gymState.auto = false;
    ctx.gymState.activityStartedAt = performance.now();
    const localPositions = {
      bench: GYM_BENCH_LIE_POSITION,
      treadmill: [0, 0.35, -3.3],
      bike: [6.9, 2.3, -3.05],
      rower: [6.6, 0.95, 4.65],
      punch: [-1.5, 0.18, 4.8],
    };
    const point = ctx.gym.localToWorld(
      new THREE.Vector3(...localPositions[kind]),
    );
    ctx.player.position.copy(point);
    ctx.player.rotation.set(0, kind === "punch" ? -Math.PI / 2 : 0, 0);
    ctx.lastPosition.copy(ctx.player.position);
    onGymState(ctx.gymPublicState(kind === "bench"));
    return true;
  }

  function setGymWeight(value) {
    const weight = clamp(
      Math.round((Number(value) || 45) / 5) * 5,
      45,
      GYM_MAX_WEIGHT_LB,
    );
    ctx.gymState.weightLb = weight;
    if (weight < GYM_HEAVY_WEIGHT_LB) ctx.gymState.auto = false;
    onGymState(ctx.gymPublicState(true));
    return weight;
  }

  function performGymRep() {
    if (ctx.gymState.active !== "bench") beginGymExercise("bench");
    const now = performance.now();
    if (now - ctx.gymState.repStartedAt < 720) return false;
    ctx.gymState.repStartedAt = now;
    ctx.gymState.reps += 1;
    onGymState(ctx.gymPublicState(true));
    return true;
  }

  function setGymAuto(enabled) {
    if (ctx.gymState.active !== "bench") beginGymExercise("bench");
    ctx.gymState.auto =
      enabled === true && ctx.gymState.weightLb >= GYM_HEAVY_WEIGHT_LB;
    if (ctx.gymState.auto && performance.now() - ctx.gymState.repStartedAt >= 720) {
      performGymRep();
    }
    onGymState(ctx.gymPublicState(true));
    return ctx.gymState.auto;
  }

  function updateGymExercise(input, delta, time) {
    if (!ctx.gymState.active) return false;
    if (input.movement.lengthSq() || ctx.jumpQueued || ctx.dashTarget) {
      stopGymExercise();
      return false;
    }
    const elapsed = Math.max(0, time - ctx.gymState.activityStartedAt);
    if (ctx.gymState.active === "bench") {
      const repStartedAt = ctx.gymState.repStartedAt;
      const progress = repStartedAt
        ? clamp((time - repStartedAt) / GYM_BENCH_REP_MS, 0, 1)
        : 1;
      const lowered = progress < 1 ? Math.sin(progress * Math.PI) : 0;
      ctx.player.rotation.order = "YXZ";
      ctx.player.rotation.set(Math.PI / 2, Math.PI, 0);
      poseAvatarArms(
        ctx.player,
        Math.PI / 2,
        Math.PI / 2,
        lowered * GYM_BENCH_PRESS_DEPTH,
      );
      applyLegPitch(ctx.player, 0.28, 0.28);
    } else if (ctx.gymState.active === "treadmill") {
      const gait = Math.sin(elapsed * 0.014) * 0.72;
      poseAvatarArms(ctx.player, gait, -gait);
      applyLegPitch(ctx.player, -gait * GAIT_LEG_SWING, gait * GAIT_LEG_SWING);
    } else if (ctx.gymState.active === "bike") {
      poseAvatarArms(ctx.player, 0.72, 0.72);
      const pedal = Math.sin(elapsed * 0.012) * 0.7;
      applyLegPitch(ctx.player, pedal, -pedal);
    } else if (ctx.gymState.active === "rower") {
      const stroke = Math.sin(elapsed * 0.006);
      const reach = 1 + stroke * 0.45;
      poseAvatarArms(ctx.player, reach, reach);
      applyLegPitch(ctx.player, 0.45 - stroke * 0.2, 0.45 - stroke * 0.2);
    } else if (ctx.gymState.active === "punch") {
      const punch = Math.sin(elapsed * 0.011);
      poseAvatarArms(
        ctx.player,
        punch > 0 ? 1.55 : 0.35,
        punch < 0 ? 1.55 : 0.35,
      );
      applyLegPitch(ctx.player, 0.1, -0.1);
    }
    ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
    animateAvatarActivity(ctx.player, time, delta, reducedMotion);
    ctx.wasWalking = false;
    return true;
  }

  // Keep the historical method name as the app-facing navigation contract,
  // but take visitors to the edge of the open member clearing.
  // Leaving the Office remains a deliberate walk through its own door.
  // A member's own bench, or the first bench past the roster if this visitor
  // has no account in the directory: an arriving guest gets a seat of their
  // own rather than the nearest stranger's plank.
  const campfireSeatWaiters = [];
  function campfireSeatsReady() {
    return (
      (ctx.campfire.userData.seatBenches || []).length > 0 &&
      ctx.campfire.userData.seatByName instanceof Map
    );
  }
  function whenCampfireSeatsReady(callback) {
    if (typeof callback !== "function") return;
    if (campfireSeatsReady()) {
      callback();
      return;
    }
    campfireSeatWaiters.push(callback);
  }
  function notifyCampfireSeatsReady() {
    if (!campfireSeatsReady()) return;
    campfireSeatWaiters.splice(0).forEach((callback) => {
      try {
        callback();
      } catch (_) {}
    });
  }
  function memberCircleSeatFor(name) {
    const benches = ctx.campfire.userData.seatBenches || [];
    if (!benches.length) return null;
    const wanted = String(name || "").trim().toLowerCase();
    const owned = wanted
      ? ctx.campfire.userData.seatByName?.get(wanted)
      : undefined;
    const index = Number.isInteger(owned)
      ? owned
      : Math.min(
        Math.max(0, Math.round(Number(ctx.campfire.userData.openSeatIndex) || 0)),
        benches.length - 1,
      );
    return benches[index] ? { index, seat: benches[index].seat } : null;
  }

  function returnToCampfireBench(name) {
    if (ctx.officeSceneMode !== "town") return false;
    const entrance = new THREE.Vector3(
      ctx.campfire.position.x,
      ctx.campfire.position.y + CAMPFIRE_GROUND_Y,
      ctx.campfire.position.z - ctx.MEMBER_CLEARING_RADIUS - 3.2,
    );
    ctx.currentSpace = "town-square";
    ctx.currentFloorY = fns.outdoorFloorYAt(entrance.x, entrance.z);
    ctx.focusedRepositoryKey = "";
    fns.standUpFromBench();
    const own = memberCircleSeatFor(name);
    if (own) {
      ctx.cameraFocus = null;
      ctx.localBenchName = String(name || "").trim().slice(0, 32);
      // The directory reads a present account as away, so its plank would
      // still say OUT AND ABOUT under the visitor now sitting on it. Repaint
      // it here instead of waiting out the roster poll.
      if (ctx.localBenchName) {
        fns.setCampfireSeatLabel(own.index, ctx.localBenchName, "seated");
        fns.repaintCampfireSeatLabels();
      }
      fns.sitOnCampfireBench(own.seat);
      return true;
    }
    ctx.player.position.copy(entrance);
    ctx.player.position.y = ctx.currentFloorY;
    ctx.player.rotation.y = Math.PI;
    ctx.cameraFocus = null;
    fns.cancelDash();
    ctx.lastPosition.copy(ctx.player.position);
    onMovement({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving: false,
      activity: "visiting the Members Circle",
    });
    return true;
  }

  // Old saved bench coordinates are restored as ordinary standing positions;
  // there is no roster-dependent bench ring to reconstruct.
  function restoreCampfireSeatIfNearby(spawn, name) {
    return false;
  }

  // Clicking the flames is a camera interaction: bring the fire and the
  // nearest arc of seated members into a close, steady composition without
  // moving the visitor out of their current place.
  function focusCampfireCircle() {
    if (ctx.officeSceneMode !== "town") return false;
    ctx.selectedLandmark = "campfire";
    ctx.currentSpace = "town-square";
    fns.setCameraMode("third-person", "campfire-focus");
    ctx.cameraFocus = ctx.campfire.position.clone();
    ctx.cameraFocus.y = 1.35;
    ctx.cameraPitch = clamp(0.34, CAMERA_PITCH_MIN, CAMERA_PITCH_MAX);
    fns.setCameraZoom(Math.min(ctx.cameraZoom, 0.68));
    ctx.cameraSnapPending = true;
    return true;
  }

  return {
    campfireSeatsReady,
    notifyCampfireSeatsReady,
    whenCampfireSeatsReady,
    swingRideAmplitude,
    setSwingSpeed,
    swingSeatRestPoint,
    rideSwing,
    applySwingRidePose,
    dismountSwing,
    rideCar,
    applyCarRidePose,
    dismountCar,
    rideQuadcopter,
    toggleNearestQuadcopterRide,
    dismountQuadcopter,
    updateQuadcopterRide,
    setJetpackEquipped,
    toggleJetpack,
    setJetpackVertical,
    updateJetpackFlight,
    stopGymExercise,
    beginGymExercise,
    setGymWeight,
    performGymRep,
    setGymAuto,
    updateGymExercise,
    returnToCampfireBench,
    restoreCampfireSeatIfNearby,
    focusCampfireCircle,
  };
}
