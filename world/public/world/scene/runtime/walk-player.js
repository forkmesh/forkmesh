import {
  OFFICE_AVATAR_RADIUS,
  officeCampusSurfaceContains,
} from "../../world-office-tower.js";
import {
  GAIT_LEG_SWING,
  animateAvatarActivity,
  applyLegPitch,
} from "../avatars.js";
import {
  BIKE_RIDING_ACTIVITY,
  CAR_RIDING_ACTIVITY,
  PLAYER_DASH_ARRIVE_DISTANCE,
  PLAYER_DASH_SPEED,
  PLAYER_SUPER_JUMP_MOVE_MULTIPLIER,
} from "../gameplay-constants.js";
import { circularRideCameraYaw, parachuteSinkVelocity } from "../movement.js";
import { constrainToFootprints, neighbourFootprints } from "../neighbour-footprints.js";
import { REMOTE_DETAIL_NEAR_DISTANCE_SQ } from "../quality.js";

export function functions({
  reducedMotion,
  ctx,
  fns,
}) {
  let neighbourDoorAt = 0;

  function constrainNeighbourWalls(previous) {
    const footprints = neighbourFootprints();
    if (!footprints.length) return;
    const result = constrainToFootprints(
      footprints, previous, ctx.player.position, OFFICE_AVATAR_RADIUS);
    if (!result.blocked) return;
    ctx.player.position.x = result.x;
    ctx.player.position.z = result.z;
    fns.cancelDash();
    const now = performance.now();
    if (result.door?.link && now - neighbourDoorAt > 4000) {
      neighbourDoorAt = now;
      fns.onNeighbourDoor?.({ ...result.door.link });
    }
  }

  function walkPlayer(delta, time) {
    const previousHorizontalPosition =
      ctx.movementPreviousPosition.copy(ctx.player.position);
    const input = fns.movementInput();
    const {
      bikeDirection,
      keyboardActive,
      movement,
    } = input;
    if (ctx.zipLineRide && fns.updateZipLineRide(delta, time)) {
      ctx.wasWalking = false;
      return;
    }
    if (ctx.coasterRide && fns.updateCoasterRide(input, delta, time)) {
      return;
    }
    if (fns.updateJetpackFlight(input, delta, time)) {
      return;
    }
    if (ctx.quadcopterRide) {
      fns.updateQuadcopterRide(input, delta, time);
      return;
    }
    if (fns.updateGymExercise(input, delta, time)) {
      return;
    }
    if (ctx.swingRide) {
      // Like the bench below: relocation ends the ride, and so does any
      // movement input, which keeps WASD as the universal "get off" gesture
      // alongside the second click on the seat.
      const displaced =
        ctx.player.position.distanceToSquared(
          fns.swingSeatRestPoint(ctx.swingRide.index),
        ) > 36;
      if (!displaced && !movement.lengthSq() && !ctx.dashTarget && !ctx.jumpQueued) {
        fns.applySwingRidePose();
        ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
        animateAvatarActivity(ctx.player, time, delta, reducedMotion);
        ctx.wasWalking = false;
        return;
      }
      fns.dismountSwing({ relocate: false });
    }
    if (ctx.benchSeat) {
      // Anything that relocates the avatar (a space change, a teleport) also
      // ends the sit; otherwise the held pose would drag it back to the bench.
      const displaced =
        ctx.player.position.distanceToSquared(ctx.benchSeat.position) > 9;
      if (!displaced && !movement.lengthSq() && !ctx.dashTarget && !ctx.jumpQueued) {
        fns.applyBenchSeatPose();
        ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
        animateAvatarActivity(ctx.player, time, delta, reducedMotion);
        ctx.wasWalking = false;
        return;
      }
      fns.standUpFromBench();
    }
    const officeCampus =
      officeCampusSurfaceContains(ctx.player.position.x, ctx.player.position.z);
    // A step off the lawn onto a promenade, a district apron or the office
    // causeway is a real change of height, so the floor travels with the
    // walker rather than being fixed once per space. Indoor storeys and
    // district packages keep the floors their own scenes hand them.
    if (ctx.officeSceneMode === "town" && !ctx.packageSpaces.has(ctx.currentSpace)) {
      ctx.currentFloorY = fns.groundHeightAt(
        ctx.player.position.x,
        ctx.player.position.z,
        ctx.currentFloorY,
        ctx.currentFloorY,
      );
    }
    if (ctx.jumpQueued && ctx.player.position.y <= ctx.currentFloorY + 0.02) {
      ctx.jumpVelocity = 5.4;
      ctx.jumpQueued = false;
    }
    const airborne =
      ctx.player.position.y > ctx.currentFloorY + 0.02 || ctx.jumpVelocity !== 0;
    const parachuting = fns.updateRoofParachute(time);
    if (airborne || !officeCampus) {
      ctx.jumpVelocity = parachuting
        ? parachuteSinkVelocity(ctx.jumpVelocity, delta)
        : ctx.jumpVelocity - 14 * delta;
      ctx.player.position.y += ctx.jumpVelocity * delta;
    } else {
      ctx.player.position.y = ctx.currentFloorY;
    }
    if (ctx.player.position.y <= ctx.currentFloorY) {
      ctx.player.position.y = ctx.currentFloorY;
      ctx.jumpVelocity = 0;
      ctx.superJumping = false;
      if (ctx.roofParachute?.deployedAt && !ctx.roofParachute.landedAt) {
        ctx.roofParachute.landedAt = time;
      }
    }
    fns.updateRoofParachute(time);
    let walking = false;
    if (ctx.bikeRide) {
      ctx.cameraFocus = null;
      fns.cancelDash();
      ctx.focusedRepositoryKey = "";
      const state = ctx.bikeStates[ctx.bikeRide.index];
      const rideDirection = Math.sign(bikeDirection);
      if (state && rideDirection) {
        // movementInputState is reused on the next frame. Adjust its two
        // bike-specific readings in place instead of allocating an object in
        // the hottest movement path.
        input.keyboardActive = keyboardActive || Boolean(rideDirection);
        input.inputStrength = Math.abs(bikeDirection);
        fns.movementSpeedForInput(input, delta);
        state.travelDirection = rideDirection;
        walking = true;
      } else {
        ctx.keyboardMovementSpeed = fns.baseMoveSpeed();
        if (state) state.travelDirection = 0;
      }
      if (state) {
        const previousBikeHeading = state.bike.rotation.y;
        fns.rideBikeAlongRoad(
          state,
          rideDirection * ctx.keyboardMovementSpeed * delta,
          input.bikeSteer,
          delta,
        );
        if (ctx.cameraMode === "first-person") {
          ctx.cameraYaw = circularRideCameraYaw(
            ctx.cameraYaw,
            previousBikeHeading,
            state.bike.rotation.y,
          );
        }
        ctx.player.position.x = state.bike.position.x;
        ctx.player.position.z = state.bike.position.z;
        ctx.player.rotation.y = state.bike.rotation.y;
      }
    } else if (movement.lengthSq()) {
      ctx.cameraFocus = null;
      // Any manual input takes the wheel back from a double-click dash.
      fns.cancelDash();
      ctx.focusedRepositoryKey = "";
      fns.movementSpeedForInput(input, delta);
      ctx.player.position.addScaledVector(
        movement,
        ctx.keyboardMovementSpeed *
          (ctx.superJumping ? PLAYER_SUPER_JUMP_MOVE_MULTIPLIER : 1) *
          delta,
      );
      ctx.player.rotation.y = Math.atan2(-movement.x, -movement.z);
      walking = true;
    } else if (ctx.dashTarget) {
      // Double-click travel: run straight at the clicked ground point, then
      // land exactly on it instead of jittering around the destination.
      const toTarget = ctx.movementDashDirection.set(
        ctx.dashTarget.x - ctx.player.position.x,
        0,
        ctx.dashTarget.z - ctx.player.position.z,
      );
      const remaining = toTarget.length();
      const step = PLAYER_DASH_SPEED * ctx.moveSpeedScale * delta;
      if (remaining <= Math.max(step, PLAYER_DASH_ARRIVE_DISTANCE)) {
        ctx.player.position.x = ctx.dashTarget.x;
        ctx.player.position.z = ctx.dashTarget.z;
        fns.cancelDash();
      } else {
        toTarget.divideScalar(remaining);
        ctx.player.position.x += toTarget.x * step;
        ctx.player.position.z += toTarget.z * step;
        ctx.player.rotation.y = Math.atan2(-toTarget.x, -toTarget.z);
      }
      ctx.keyboardMovementSpeed = fns.baseMoveSpeed();
      walking = true;
    } else {
      ctx.keyboardMovementSpeed = fns.baseMoveSpeed();
    }

    if (
      !ctx.activeWalkSurfaceContains(
        ctx.player.position.x,
        ctx.player.position.z,
        OFFICE_AVATAR_RADIUS,
      )
    ) {
      ctx.player.position.x = previousHorizontalPosition.x;
      ctx.player.position.z = previousHorizontalPosition.z;
      fns.cancelDash();
    }
    if (!ctx.superJumping) {
      fns.constrainTownOfficeWalls(previousHorizontalPosition);
      constrainNeighbourWalls(previousHorizontalPosition);
      for (let index = 0; index < ctx.districtPackages.length; index += 1) {
        ctx.districtPackages[index].instance.constrainPlayer?.(
          previousHorizontalPosition,
          ctx.player,
        );
      }
    } else if (
      fns.constrainFlightToOfficeShell(
        ctx.player.position,
        previousHorizontalPosition,
        OFFICE_AVATAR_RADIUS,
      ) === "roof"
    ) {
      // A super jump is allowed to clear the tower footprint, which is why the
      // flat town wall never applies to it. Coming back down through the roof
      // is not a way in: the arc ends on the roof line and the ordinary walk
      // constraint takes the visitor off the building from there.
      ctx.superJumping = false;
      ctx.jumpVelocity = 0;
    }
    if (ctx.carRide) {
      ctx.jumpQueued = false;
      ctx.jumpVelocity = 0;
      ctx.superJumping = false;
      fns.applyCarRidePose(walking, delta);
    } else if (ctx.bikeRide) {
      ctx.jumpQueued = false;
      ctx.jumpVelocity = 0;
      ctx.superJumping = false;
      fns.applyBikeRidePose(walking, delta);
    } else {
      const gait = walking ? Math.sin(time * 0.012) * 0.52 : 0;
      ctx.player.userData.leftArm.rotation.x = gait;
      ctx.player.userData.rightArm.rotation.x = -gait;
      applyLegPitch(ctx.player, -gait * GAIT_LEG_SWING, gait * GAIT_LEG_SWING);
      if (ctx.jumpVelocity === 0) {
        ctx.player.position.y +=
          walking ? Math.abs(Math.sin(time * 0.012)) * 0.035 : 0;
      }
    }
    ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
    animateAvatarActivity(ctx.player, time, delta, reducedMotion);

    if (
      performance.now() - ctx.lastMovementEmit > 450 &&
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
        moving: true,
        activity: ctx.carRide
          ? CAR_RIDING_ACTIVITY
          : ctx.bikeRide
            ? BIKE_RIDING_ACTIVITY
          : ctx.currentLocation === "Town Square"
            ? "exploring the Town Square"
            : `visiting ${ctx.currentLocation}`,
      });
    }
    if (!walking && ctx.wasWalking) {
      ctx.lastPosition.copy(ctx.player.position);
      fns.queueMovementEvent({
        x: Number(ctx.player.position.x.toFixed(2)),
        y: Number(ctx.player.position.y.toFixed(2)),
        z: Number(ctx.player.position.z.toFixed(2)),
        heading: Number(ctx.player.rotation.y.toFixed(3)),
        space: ctx.currentSpace,
        moving: false,
        activity: ctx.carRide
          ? CAR_RIDING_ACTIVITY
          : ctx.bikeRide
            ? BIKE_RIDING_ACTIVITY
          : ctx.currentLocation === "Town Square"
            ? "exploring the Town Square"
            : `visiting ${ctx.currentLocation}`,
      });
    }
    ctx.wasWalking = walking;
  }

  function remoteAvatarDetailDelta(avatar, time, delta) {
    const data = avatar.userData;
    data.remoteDetailDelta = (data.remoteDetailDelta || 0) + delta;
    ctx.remoteDetailSphere.center.copy(avatar.position);
    const coarse =
      avatar.position.distanceToSquared(ctx.camera.position) >
        REMOTE_DETAIL_NEAR_DISTANCE_SQ ||
      !ctx.remoteDetailFrustum.intersectsSphere(ctx.remoteDetailSphere);
    if (coarse) {
      if (time < (data.nextRemoteDetailAt || 0)) return -1;
      data.nextRemoteDetailAt = time + ctx.qualityProfile.visualAnimationMs;
    }
    const detailDelta = data.remoteDetailDelta;
    data.remoteDetailDelta = 0;
    return detailDelta;
  }

  return {
    walkPlayer,
    remoteAvatarDetailDelta,
  };
}
