import {
  OFFICE_AVATAR_RADIUS,
  OFFICE_DOOR_WIDTH,
  OFFICE_ELEVATOR_CENTER_X,
  OFFICE_ELEVATOR_CENTER_Z,
  OFFICE_FLOOR_HEIGHT,
  OFFICE_FRONT_Z,
  OFFICE_HEX_APOTHEM,
  OFFICE_HEX_RADIUS,
  OFFICE_HEX_SIDES,
  officeElevatorCabinContains,
  officeFloorY,
} from "../../world-office-tower.js";
import {
  CAMERA_DISTANCE,
  CAMERA_GROUND_CLEARANCE,
  FIRST_PERSON_EYE_HEIGHT,
  OFFICE_DOORWAY_ENTRY_Z,
  OFFICE_DOOR_HEIGHT,
  WORLD_GROUND_Y,
} from "../gameplay-constants.js";
import {
  arrivalFacingHeading,
} from "../layout.js";
import {
  WORLD_SPACE_FLOORS,
  officeCampusSpace,
  packageSpace,
  worldRadiusForSpace,
} from "../movement.js";
import { clamp } from "../object-utils.js";

export function functions({
  onLocationChange,
  reducedMotion,
  ctx,
  fns,
}) {
  function officeCameraDistanceLimit(
    target,
    direction,
    requestedDistance,
  ) {
    const requested = Math.max(0, Number(requestedDistance) || 0);
    if (
      ctx.officeSceneMode === "town" ||
      !target ||
      !direction ||
      requested <= 0
    ) {
      return requested;
    }
    const localTarget = ctx.officeInterior.worldToLocal(
      ctx.cameraLimitLocalTarget.copy(target),
    );
    const ridingElevator =
      Boolean(ctx.officeElevatorRide) ||
      officeElevatorCabinContains(
        localTarget.x,
        localTarget.z,
        0.08,
      );
    if (ctx.officeCurrentFloorId === "rooftop" && !ridingElevator) {
      return requested;
    }
    const floorBase = ridingElevator
      ? ctx.officeElevatorCar.position.y
      : officeFloorY(ctx.officeCurrentFloorId);
    const bounds = ridingElevator
      ? {
          minX: OFFICE_ELEVATOR_CENTER_X - 4.42,
          maxX: OFFICE_ELEVATOR_CENTER_X + 4.42,
          minY: floorBase + 0.5,
          maxY: floorBase + 7.02,
          minZ: OFFICE_ELEVATOR_CENTER_Z - 3.42,
          maxZ: OFFICE_ELEVATOR_CENTER_Z + 3.42,
        }
      : {
          minX: -OFFICE_HEX_RADIUS + 0.72,
          maxX: OFFICE_HEX_RADIUS - 0.72,
          minY: floorBase + 0.5,
          maxY: floorBase + OFFICE_FLOOR_HEIGHT - 0.55,
          minZ: -OFFICE_FRONT_Z + 0.72,
          maxZ: OFFICE_FRONT_Z - 0.72,
        };
    const frontDirection = Number(direction.z) || 0;
    const rawFrontDistance =
      frontDirection > 1e-6
        ? (OFFICE_FRONT_Z - localTarget.z) / frontDirection
        : NaN;
    const targetInDoorwayThreshold =
      localTarget.z >= OFFICE_FRONT_Z &&
      localTarget.z <= OFFICE_DOORWAY_ENTRY_Z + 0.08;
    const portalDistance =
      rawFrontDistance >= 0
        ? rawFrontDistance
        : targetInDoorwayThreshold
          ? 0
          : NaN;
    const portalX =
      localTarget.x + (Number(direction.x) || 0) * portalDistance;
    const portalY =
      localTarget.y + (Number(direction.y) || 0) * portalDistance;
    const rayThroughOpenLobbyPortal =
      !ridingElevator &&
      ctx.officeCurrentFloorId === "lobby" &&
      ctx.officeSlidingDoorOpen >= 0.9 &&
      frontDirection > 1e-6 &&
      Number.isFinite(portalDistance) &&
      Math.abs(portalX) <= OFFICE_DOOR_WIDTH / 2 - 0.08 &&
      portalY >= 0.08 &&
      portalY <= OFFICE_DOOR_HEIGHT - 0.08;
    const axes = [
      ["x", "minX", "maxX"],
      ["y", "minY", "maxY"],
      ["z", "minZ", "maxZ"],
    ];
    let boundaryDistance = Infinity;
    axes.forEach(([axis, minKey, maxKey]) => {
      const component = Number(direction[axis]) || 0;
      if (Math.abs(component) < 1e-6) return;
      // The open front door is a real portal in the floor envelope. Only its
      // Z face is omitted; side walls, floor/ceiling, rear wall, elevator, and
      // every ray that misses the actual aperture retain the normal clamp.
      if (
        axis === "z" &&
        component > 0 &&
        rayThroughOpenLobbyPortal
      ) {
        return;
      }
      const edge = component > 0 ? bounds[maxKey] : bounds[minKey];
      const distance = (edge - localTarget[axis]) / component;
      if (distance >= 0) boundaryDistance = Math.min(boundaryDistance, distance);
    });
    if (!ridingElevator) {
      for (const side of OFFICE_HEX_SIDES) {
        if (side.normalX === 0) continue;
        const along =
          side.normalX * (Number(direction.x) || 0) +
          side.normalZ * (Number(direction.z) || 0);
        if (along <= 1e-6) continue;
        const distance =
          (OFFICE_HEX_APOTHEM - 0.72 -
            (side.normalX * localTarget.x + side.normalZ * localTarget.z)) /
          along;
        if (distance >= 0) boundaryDistance = Math.min(boundaryDistance, distance);
      }
    }
    if (!Number.isFinite(boundaryDistance)) return requested;
    return Math.min(requested, Math.max(0, boundaryDistance - 0.24));
  }

  // District packages stay part of the outdoor World scene, so they cannot
  // use the original Office's local-space envelope above. Each installed one
  // may tighten the orbit distance while the visitor is inside it.
  function districtCameraDistanceLimit(target, direction, requestedDistance) {
    let distance = Math.max(0, Number(requestedDistance) || 0);
    if (ctx.officeSceneMode !== "town" || !target || !direction || distance <= 0) {
      return distance;
    }
    for (let index = 0; index < ctx.districtPackages.length; index += 1) {
      const limit = ctx.districtPackages[index].instance.cameraDistanceLimit;
      if (typeof limit !== "function") continue;
      const next = Number(limit(target, direction, distance, ctx.player));
      if (Number.isFinite(next) && next >= 0) distance = Math.min(distance, next);
    }
    return distance;
  }

  function groundCameraDistanceLimit(target, direction, requestedDistance) {
    const requested = Math.max(0, Number(requestedDistance) || 0);
    if (
      ctx.officeSceneMode !== "town" ||
      !target ||
      !direction ||
      requested <= 0
    ) {
      return requested;
    }
    const rise = Number(direction.y) || 0;
    if (rise > -1e-6) return requested;
    const headroom = Math.max(
      0,
      (Number(target.y) || 0) -
        (WORLD_GROUND_Y + CAMERA_GROUND_CLEARANCE),
    );
    return Math.min(requested, headroom / -rise);
  }

  function updateCamera(delta) {
    const officeAvatar =
      ctx.officeSceneMode === "meeting"
        ? ctx.officeParticipants.get(ctx.officeLocalParticipantId) || ctx.officeLobbyPlayer
        : ctx.officeSceneMode === "lobby"
          ? ctx.player
          : null;
    if (ctx.officeSceneMode === "lobby") {
      const localPosition = fns.officeAvatarLocalPosition(
        ctx.player,
        ctx.officeCameraLocalPosition,
      );
      const insideElevator = officeElevatorCabinContains(
        localPosition.x,
        localPosition.z,
        OFFICE_AVATAR_RADIUS,
      );
      if (!insideElevator) {
        fns.releaseOfficeElevatorCamera(false);
      } else if (ctx.officeElevatorRide || !ctx.officeElevatorCameraReleased) {
        fns.lockOfficeElevatorCamera();
      }
    } else {
      fns.releaseOfficeElevatorCamera(false);
    }
    if (ctx.officeElevatorCameraLocked) {
      // Elevator security-camera framing from the upper rear-left corner of
      // the moving glass car. It looks diagonally across the right-wall
      // controls and out through the exterior-facing opening, never back into
      // the Office interior.
      ctx.cameraEye.set(-3.72, 6.46, 2.7);
      ctx.officeElevatorCar.localToWorld(ctx.cameraEye);
      ctx.cameraLookTarget.set(3.4, 3.15, -5.4);
      ctx.officeElevatorCar.localToWorld(ctx.cameraLookTarget);
      ctx.camera.position.copy(ctx.cameraEye);
      ctx.camera.lookAt(ctx.cameraLookTarget);
      return;
    }
    if (ctx.cameraMode === "first-person") {
      const firstPersonAvatar = officeAvatar || ctx.player;
      firstPersonAvatar.getWorldPosition(ctx.cameraEye);
      ctx.cameraEye.y += FIRST_PERSON_EYE_HEIGHT;
      const horizontal = Math.cos(ctx.firstPersonPitch);
      ctx.cameraDirection.set(
        -Math.sin(ctx.cameraYaw) * horizontal,
        -Math.sin(ctx.firstPersonPitch),
        -Math.cos(ctx.cameraYaw) * horizontal,
      );
      // A following eye camera must not ease behind the moving avatar. Copying
      // the position directly avoids visible lag and motion sickness.
      ctx.camera.position.copy(ctx.cameraEye);
      ctx.cameraLookTarget.copy(ctx.cameraEye).add(ctx.cameraDirection);
      ctx.camera.lookAt(ctx.cameraLookTarget);
      return;
    }
    const target = ctx.cameraTarget;
    if (officeAvatar) {
      officeAvatar.getWorldPosition(target);
      target.y += FIRST_PERSON_EYE_HEIGHT;
    } else if (ctx.cameraFocus) {
      target.copy(ctx.cameraFocus);
    } else {
      target.copy(ctx.player.position);
      target.y += FIRST_PERSON_EYE_HEIGHT;
    }
    const officeFocused = Boolean(ctx.cameraFocus && ctx.selectedLandmark === "office");
    const officeInteriorFocused = ctx.officeSceneMode !== "town";
    // The Office keeps the same orbit feel as the rest of the World. A local
    // ray-to-bounds limit below stops zoom at the current floor walls or the
    // panoramic elevator glass instead of shrinking the whole interior view.
    const focusScale = officeInteriorFocused
      ? 1
      : officeFocused
        ? ctx.camera.aspect < 0.75 ? 1.36 : 0.87
        : ctx.cameraFocus
          ? 0.78
          : 1;
    const requestedDistance = CAMERA_DISTANCE * ctx.cameraZoom * focusScale;
    const offsetDirection = ctx.cameraOffsetDirection.set(
      Math.sin(ctx.cameraYaw) * Math.cos(ctx.cameraPitch),
      Math.sin(ctx.cameraPitch),
      Math.cos(ctx.cameraYaw) * Math.cos(ctx.cameraPitch),
    );
    const distance = groundCameraDistanceLimit(
      target,
      offsetDirection,
      districtCameraDistanceLimit(
        target,
        offsetDirection,
        officeCameraDistanceLimit(
          target,
          offsetDirection,
          requestedDistance,
        ),
      ),
    );
    const desired = ctx.cameraDesired
      .copy(target)
      .addScaledVector(offsetDirection, distance);
    const localTarget = ctx.officeInterior.worldToLocal(
      ctx.cameraLocalTarget.copy(target),
    );
    const rooftopPatioCamera =
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId === "rooftop" &&
      !ctx.officeElevatorRide &&
      !officeElevatorCabinContains(
        localTarget.x,
        localTarget.z,
        0.08,
      );
    if (rooftopPatioCamera) {
      // Looking upward puts an orbit camera below its eye target. Preserve the
      // requested outward X/Z zoom, but keep that eye just above the roof slab
      // instead of letting a steep pitch pass through the story below.
      const localDesired = ctx.officeInterior.worldToLocal(
        ctx.cameraLocalDesired.copy(desired),
      );
      localDesired.y = Math.max(
        localDesired.y,
        officeFloorY("rooftop") + 0.55,
      );
      desired.copy(ctx.officeInterior.localToWorld(localDesired));
    }
    if (reducedMotion) ctx.camera.position.copy(desired);
    else if (ctx.cameraSnapPending) ctx.camera.position.copy(desired);
    else ctx.camera.position.lerp(desired, 1 - Math.pow(0.0008, delta));
    ctx.cameraSnapPending = false;
    if (ctx.officeSceneMode === "town") {
      ctx.camera.position.y = Math.max(
        ctx.camera.position.y,
        WORLD_GROUND_Y + CAMERA_GROUND_CLEARANCE,
      );
    }
    if (rooftopPatioCamera) {
      // Clamp the actual eased camera as well as its destination. Otherwise a
      // prior below-slab frame could lerp through the roof on its way back up.
      const localCamera = ctx.officeInterior.worldToLocal(
        ctx.cameraLocalPosition.copy(ctx.camera.position),
      );
      localCamera.y = Math.max(
        localCamera.y,
        officeFloorY("rooftop") + 0.55,
      );
      ctx.camera.position.copy(ctx.officeInterior.localToWorld(localCamera));
    }
    if (ctx.officeElevatorRide) {
      // The cabin moves faster than a softly lerped orbit camera. Clamp the
      // eased result back inside its live glass envelope so the eye never
      // trails through a floor slab while the car is between stories.
      const localCamera = ctx.officeInterior.worldToLocal(
        ctx.cameraLocalPosition.copy(ctx.camera.position),
      );
      localCamera.x = clamp(
        localCamera.x,
        OFFICE_ELEVATOR_CENTER_X - 4.42,
        OFFICE_ELEVATOR_CENTER_X + 4.42,
      );
      localCamera.y = clamp(
        localCamera.y,
        ctx.officeElevatorCar.position.y + 0.5,
        ctx.officeElevatorCar.position.y + 7.02,
      );
      localCamera.z = clamp(
        localCamera.z,
        OFFICE_ELEVATOR_CENTER_Z - 3.42,
        OFFICE_ELEVATOR_CENTER_Z + 3.42,
      );
      ctx.camera.position.copy(ctx.officeInterior.localToWorld(localCamera));
    }
    ctx.camera.lookAt(target);
  }

  function setSpawn(spawn = {}) {
    // An arrival cell is always a ground-level outdoor spot. Applying one to a
    // visitor who is standing inside the tower would drop them from their
    // current story onto the lobby slab, so the correction is declined until
    // they walk back out of the Office.
    if (ctx.officeSceneMode !== "town") return false;
    const requestedSpace = String(spawn.space || "");
    if (packageSpace(requestedSpace) && !ctx.packageSpaces.has(requestedSpace)) {
      // The district that owns this spot is not installed (yet). Park the
      // request; installing the package applies it.
      ctx.pendingPackageSpawn = { ...spawn };
      return false;
    }
    const space =
      Object.hasOwn(WORLD_SPACE_FLOORS, requestedSpace) ||
      ctx.packageSpaces.has(requestedSpace)
        ? requestedSpace
        : "town-square";
    const spawnRadius = worldRadiusForSpace(space, ctx.packageSpaces);
    const x = clamp(Number(spawn.x) || 0, -spawnRadius, spawnRadius);
    const z = clamp(Number(spawn.z) || 0, -spawnRadius, spawnRadius);
    const heading = clamp(Number(spawn.heading) || 0, -Math.PI, Math.PI);
    ctx.currentSpace = space;
    ctx.currentFloorY = ctx.packageSpaces.has(space)
      ? ctx.packageSpaces.get(space).floorY
      : officeCampusSpace(space)
        ? WORLD_SPACE_FLOORS[space]
        : fns.outdoorFloorYAt(x, z);
    ctx.player.position.set(x, ctx.currentFloorY, z);
    if (space === "town-square") {
      fns.constrainTownOfficeWalls(ctx.player.position.clone());
    }
    ctx.player.rotation.y = arrivalFacingHeading(x, z, heading);
    ctx.lastPosition.copy(ctx.player.position);
    ctx.wasWalking = false;
    ctx.cameraFocus = null;
    // Restored positions arrive after asynchronous bootstrap data. Do not
    // expose a long map-wide camera lerp after the loading cover disappears.
    ctx.cameraSnapPending = true;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
    if (space === "town-square") {
      fns.nearestLandmark();
    } else {
      ctx.currentLocation = space
        .split("-")
        .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
        .join(" ");
      onLocationChange(ctx.currentLocation, "");
    }
    return true;
  }

  return {
    updateCamera,
    setSpawn,
  };
}
