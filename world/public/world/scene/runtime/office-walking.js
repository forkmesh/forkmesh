import { CAMPFIRE_SEATED_ACTIVITY } from "../../world-data.js";
import {
  officeFloorY,
  officeHexContains,
} from "../../world-office-tower.js";
import {
  GAIT_LEG_SWING,
  animateAvatarActivity,
  applyLegPitch,
  applySeatedLegPose,
  seatedAvatarY,
} from "../avatars.js";
import {
  PLAYER_DASH_ARRIVE_DISTANCE,
  PLAYER_DASH_SPEED,
} from "../gameplay-constants.js";
import { clamp } from "../object-utils.js";

export function functions({
  THREE,
  onMovement,
  onOfficeElevatorSound,
  reducedMotion,
  ctx,
  fns,
}) {
  let lastOfficeMovementEmit = 0;


  function walkOfficeParticipant(delta, time) {
    const avatar = ctx.officeParticipants.get(ctx.officeLocalParticipantId);
    if (!avatar) return;
    const presence = avatar.userData.officePresence || {};
    const input =
      presence.pose === "seated"
        ? ctx.seatedMovementInput
        : fns.movementInput();
    const movement = input.movement;
    const walking = movement.lengthSq() > 0;
    const movementSpeed = fns.movementSpeedForInput(input, delta);
    if (walking) {
      const previousPosition = ctx.movementPreviousPosition.copy(avatar.position);
      avatar.position.addScaledVector(
        movement,
        movementSpeed * delta,
      );
      avatar.position.y = officeFloorY("marketing") + 0.38;
      avatar.rotation.y = Math.atan2(-movement.x, -movement.z);
      if (fns.constrainOfficeInteriorWalls(avatar, previousPosition)) return;
    }
    const gait = walking ? Math.sin(time * 0.012) * 0.48 : 0;
    avatar.userData.leftArm.rotation.x = gait;
    avatar.userData.rightArm.rotation.x = -gait;
    applyLegPitch(avatar, -gait * GAIT_LEG_SWING, gait * GAIT_LEG_SWING);
    avatar.userData.officePresence = {
      ...presence,
      x: avatar.position.x,
      y: 0.38,
      z: avatar.position.z,
      yaw: avatar.rotation.y,
      moving: walking,
    };
    const shouldEmit = walking
      ? performance.now() - lastOfficeMovementEmit >= 120
      : ctx.officeWasMoving;
    if (shouldEmit) {
      lastOfficeMovementEmit = performance.now();
      // Presence serialization, socket backpressure checks, and persistence
      // run in a timer after this animation frame has rendered.
      fns.queueMovementEvent({
        x: Number(avatar.position.x.toFixed(2)),
        y: 0.38,
        z: Number(avatar.position.z.toFixed(2)),
        yaw: Number(avatar.rotation.y.toFixed(3)),
        moving: walking,
      }, true);
    }
    ctx.officeWasMoving = walking;
  }

  function walkOfficeLobbyPlayer(delta, time) {
    const avatar = ctx.player;
    if (ctx.officeElevatorRide) {
      ctx.officeLobbyPlayerMoving = true;
      avatar.userData.leftArm.rotation.x = 0;
      avatar.userData.rightArm.rotation.x = 0;
      applyLegPitch(avatar, 0, 0);
      animateAvatarActivity(avatar, time, delta, reducedMotion);
      return;
    }
    const input = fns.movementInput();
    const movement = input.movement;
    if (ctx.officeChairSeat) {
      if (!movement.lengthSq() && !ctx.dashTarget && !ctx.jumpQueued) {
        ctx.officeLobbyPlayerMoving = false;
        fns.applyOfficeChairSeatPose();
        animateAvatarActivity(avatar, time, delta, reducedMotion);
        return;
      }
      fns.standUpFromOfficeChair();
    }
    let walking = movement.lengthSq() > 0;
    const movementSpeed = fns.movementSpeedForInput(input, delta);
    if (walking) {
      fns.cancelDash();
      const previousPosition = ctx.movementPreviousPosition.copy(avatar.position);
      avatar.position.addScaledVector(
        movement,
        movementSpeed * delta,
      );
      if (!ctx.officeRoofJumping) {
        avatar.position.y = officeFloorY(ctx.officeCurrentFloorId) + 0.38;
      }
      avatar.rotation.y = Math.atan2(-movement.x, -movement.z);
      if (
        !ctx.officeRoofJumping &&
        fns.tryOfficeFloorWarpDoorway(avatar, previousPosition)
      ) {
        return;
      }
      if (
        !ctx.officeRoofJumping &&
        fns.constrainOfficeInteriorWalls(avatar, previousPosition)
      ) {
        return;
      }
    } else if (ctx.dashTarget) {
      const previousPosition = ctx.movementPreviousPosition.copy(avatar.position);
      const toTarget = ctx.movementDashDirection.set(
        ctx.dashTarget.x - avatar.position.x,
        0,
        ctx.dashTarget.z - avatar.position.z,
      );
      const remaining = toTarget.length();
      const step = PLAYER_DASH_SPEED * ctx.moveSpeedScale * delta;
      if (remaining <= Math.max(step, PLAYER_DASH_ARRIVE_DISTANCE)) {
        avatar.position.x = ctx.dashTarget.x;
        avatar.position.z = ctx.dashTarget.z;
        fns.cancelDash();
      } else {
        toTarget.divideScalar(remaining);
        avatar.position.x += toTarget.x * step;
        avatar.position.z += toTarget.z * step;
        avatar.rotation.y = Math.atan2(-toTarget.x, -toTarget.z);
      }
      if (!ctx.officeRoofJumping) {
        avatar.position.y = officeFloorY(ctx.officeCurrentFloorId) + 0.38;
      }
      walking = true;
      if (
        !ctx.officeRoofJumping &&
        fns.tryOfficeFloorWarpDoorway(avatar, previousPosition)
      ) {
        return;
      }
      if (
        !ctx.officeRoofJumping &&
        fns.constrainOfficeInteriorWalls(avatar, previousPosition)
      ) {
        return;
      }
    }
    if (ctx.officeRoofJumping) {
      const roofY = officeFloorY("rooftop") + 0.38;
      ctx.jumpVelocity -= 14 * delta;
      avatar.position.y += ctx.jumpVelocity * delta;
      const localPosition = fns.officeAvatarLocalPosition(avatar);
      const outsideRoof = !officeHexContains(
        localPosition.x,
        localPosition.z,
        -0.45,
      );
      if (outsideRoof && avatar.position.y > roofY - 0.1) {
        fns.completeOfficeRoofExit();
        return;
      }
      if (avatar.position.y <= roofY) {
        avatar.position.y = roofY;
        ctx.jumpVelocity = 0;
        ctx.officeRoofJumping = false;
      }
      fns.clearOfficeFurniture(avatar);
    }
    ctx.officeLobbyPlayerMoving = walking;
    const gait = walking ? Math.sin(time * 0.012) * 0.48 : 0;
    avatar.userData.leftArm.rotation.x = gait;
    avatar.userData.rightArm.rotation.x = -gait;
    applyLegPitch(
      avatar,
      -gait * GAIT_LEG_SWING,
      gait * GAIT_LEG_SWING,
    );
    animateAvatarActivity(avatar, time, delta, reducedMotion);
  }

  function updateOfficeElevator(time) {
    const ride = ctx.officeElevatorRide;
    if (!ride) return false;
    const progress = clamp(
      (time - ride.startedAt) / Math.max(1, ride.duration),
      0,
      1,
    );
    const eased =
      progress < 0.5
        ? 4 * progress ** 3
        : 1 - (-2 * progress + 2) ** 3 / 2;
    ctx.player.position.y =
      ride.fromY + (ride.toY - ride.fromY) * eased;
    ctx.officeElevatorCar.position.y = ctx.player.position.y - 0.38;
    const closed =
      progress < 0.16
        ? progress / 0.16
        : progress > 0.84
          ? (1 - progress) / 0.16
          : 1;
    ctx.officeElevatorDoors[0].position.x = -4.36 + 2.12 * closed;
    ctx.officeElevatorDoors[1].position.x = 4.36 - 2.12 * closed;
    ctx.currentFloorY = ctx.player.position.y;
    if (progress < 1) return true;
    ctx.officeCurrentFloorId = ride.floorId;
    // Elevator completion also changes the active story without necessarily
    // changing the camera LOD class.
    fns.syncOfficeFloorVisibility();
    ctx.currentFloorY = officeFloorY(ride.floorId) + 0.38;
    ctx.player.position.y = ctx.currentFloorY;
    ctx.officeElevatorCar.position.y = officeFloorY(ride.floorId);
    ctx.officeElevatorDoors[0].position.x = -4.36;
    ctx.officeElevatorDoors[1].position.x = 4.36;
    ctx.currentSpace = `office-${ride.floorId}`;
    ctx.officeElevatorRide = null;
    fns.releaseOfficeElevatorCamera(true);
    fns.renderOfficeMarketingTasks();
    onOfficeElevatorSound("arrive", {
      floor: ride.floorId,
      duration: ride.duration,
    });
    return true;
  }

  // Sitting is a local pose, not a teleport: the avatar lands on the plank it
  // was clicked on, faces the flames and keeps that pose until it moves again.
  function sitOnCampfireBench(seat) {
    fns.dismountSwing({ relocate: false });
    fns.dismountBike({ relocate: false });
    fns.dismountCar({ relocate: false });
    fns.dismountQuadcopter({ relocate: false });
    const seatPoint = seat.getWorldPosition(new THREE.Vector3());
    ctx.benchSeat = {
      // Hips on the plank, not feet: the avatar drops until its thighs rest on
      // the seat and its shins hang off the front edge down to the ground.
      position: new THREE.Vector3(
        seatPoint.x,
        seatedAvatarY(
          seatPoint.y + ctx.CAMPFIRE_SEAT_HALF_THICKNESS,
          ctx.player.scale.x,
        ),
        seatPoint.z,
      ),
      // Face the flames: headings elsewhere use atan2(-dx, -dz), so pointing at
      // the pit means negating the seat -> campfire vector.
      heading: Math.atan2(
        seatPoint.x - ctx.campfire.position.x,
        seatPoint.z - ctx.campfire.position.z,
      ),
    };
    fns.cancelDash();
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    applyBenchSeatPose();
    ctx.lastPosition.copy(ctx.player.position);
    onMovement({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving: false,
      activity: CAMPFIRE_SEATED_ACTIVITY,
    });
  }

  function applyBenchSeatPose() {
    if (!ctx.benchSeat) return;
    ctx.player.position.copy(ctx.benchSeat.position);
    ctx.player.rotation.y = ctx.benchSeat.heading;
    ctx.player.userData.leftArm.rotation.x = 0;
    ctx.player.userData.rightArm.rotation.x = 0;
    applySeatedLegPose(ctx.player);
  }

  function standUpFromBench() {
    if (!ctx.benchSeat) return;
    ctx.benchSeat = null;
    ctx.player.position.y = ctx.currentFloorY;
    applyLegPitch(ctx.player, 0, 0);
    if (!ctx.localBenchName) return;
    // Walking away hands the plank back to the roster's own reading of this
    // account, which is that it is out and about in the world.
    const index = ctx.campfire.userData.seatByName?.get(ctx.localBenchName.toLowerCase());
    const owner = ctx.localBenchName;
    ctx.localBenchName = "";
    if (!Number.isInteger(index)) return;
    fns.setCampfireSeatLabel(index, owner, "away");
    fns.repaintCampfireSeatLabels();
  }

  function sitOnWorldSeat(seat) {
    if (!seat || ctx.officeSceneMode !== "town") return false;
    fns.dismountSwing({ relocate: false });
    fns.dismountBike({ relocate: false });
    fns.dismountCar({ relocate: false });
    fns.dismountQuadcopter({ relocate: false });
    const seatPoint = seat.getWorldPosition(new THREE.Vector3());
    ctx.benchSeat = {
      position: new THREE.Vector3(
        seatPoint.x,
        seatedAvatarY(seatPoint.y + 0.12, ctx.player.scale.x),
        seatPoint.z,
      ),
      heading: Number(seat.userData.worldSeatHeading) || 0,
      activity: String(
        seat.userData.worldSeatActivity || "sitting in the World",
      ),
    };
    fns.cancelDash();
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    applyBenchSeatPose();
    ctx.lastPosition.copy(ctx.player.position);
    onMovement({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      space: ctx.currentSpace,
      moving: false,
      activity: ctx.benchSeat.activity,
    });
    return true;
  }

  return {
    walkOfficeParticipant,
    walkOfficeLobbyPlayer,
    updateOfficeElevator,
    sitOnCampfireBench,
    applyBenchSeatPose,
    standUpFromBench,
    sitOnWorldSeat,
  };
}
