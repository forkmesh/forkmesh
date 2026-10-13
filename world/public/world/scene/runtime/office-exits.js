import {
  OFFICE_AVATAR_RADIUS,
  OFFICE_DOOR_WIDTH,
  OFFICE_FRONT_Z,
} from "../../world-office-tower.js";
import { animateAvatarActivity, applyLegPitch } from "../avatars.js";
import {
  CAMERA_DISTANCE,
  CAMERA_OFFSET,
  OFFICE_EXIT_CAMERA_ZOOM,
  OFFICE_INTERIOR_EXIT_Z,
  ROOF_PARACHUTE_COLLAPSE_MS,
  ROOF_PARACHUTE_DEPLOY_VELOCITY,
  ZIP_LINE_ACCELERATION,
  ZIP_LINE_BEACH_ENTRY_RADIUS,
  ZIP_LINE_LAUNCH_RADIUS,
  ZIP_LINE_LIFT_MS,
  ZIP_LINE_MAX_SPEED,
  ZIP_LINE_RIDER_DROP,
  ZIP_LINE_RIDING_ACTIVITY,
  ZIP_LINE_ROOF_LOCAL,
  ZIP_LINE_SHOULDER_DROP,
} from "../gameplay-constants.js";
import { BEACH_CENTER_X, BEACH_CENTER_Z } from "../layout.js";
import { clamp, disposeObject3D } from "../object-utils.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  onMovement,
  reducedMotion,
  ctx,
  fns,
}) {
  function leaveOfficeInterior() {
    ctx.officeRoofJumping = false;
    fns.standUpFromOfficeChair();
    const localPosition = fns.officeAvatarLocalPosition(ctx.player);
    const crossedLobbyDoorway =
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId === "lobby" &&
      Math.abs(localPosition.x) <=
        OFFICE_DOOR_WIDTH / 2 - OFFICE_AVATAR_RADIUS &&
      localPosition.z >= OFFICE_INTERIOR_EXIT_Z;
    ctx.officeSceneMode = "town";
    if (ctx.officeAquarium) ctx.officeAquarium.setFishBodiesVisible(false);
    ctx.officeCurrentFloorId = "lobby";
    fns.updateSceneLevelOfDetail(true);
    ctx.officeElevatorRide = null;
    fns.releaseOfficeElevatorCamera(false);
    ctx.currentSpace = "town-square";
    localPosition.x = clamp(
      localPosition.x,
      -OFFICE_DOOR_WIDTH / 2 + OFFICE_AVATAR_RADIUS,
      OFFICE_DOOR_WIDTH / 2 - OFFICE_AVATAR_RADIUS,
    );
    localPosition.y = 0.38;
    if (!crossedLobbyDoorway) {
      // Non-spatial teardown (for example page destruction) still needs a
      // safe outdoor position. A real doorway exit keeps its exact Z.
      localPosition.z = Math.max(localPosition.z, OFFICE_FRONT_Z + 0.82);
    }
    fns.applyOfficeAvatarLocalPosition(ctx.player, localPosition);
    ctx.lastPosition.copy(ctx.player.position);
    ctx.officeLocalParticipantId = "";
    ctx.officeWasMoving = false;
    ctx.officeExitPending = false;
    ctx.officeDoorwayEntryPending = false;
    ctx.officeLobbyPlayer.visible = false;
    ctx.player.visible = ctx.cameraMode !== "first-person";
    ctx.cameraFocus = null;
    // beginOfficeExit already pulled the eye close before the façade crossing.
    // Keep that same distance outside so the eased camera cannot travel through
    // the building shell. Preserve the doorway heading as well.
    fns.setCameraMode("third-person", "office-exit");
    ctx.cameraZoom = OFFICE_EXIT_CAMERA_ZOOM;
    ctx.cameraYaw = Math.atan2(
      Math.sin(ctx.player.rotation.y),
      Math.cos(ctx.player.rotation.y),
    );
    ctx.cameraPitch = Math.asin(CAMERA_OFFSET[1] / CAMERA_DISTANCE);
    ctx.pinchStartZoom = ctx.cameraZoom;
    fns.setOfficeParticipants([]);
    ctx.officeBubbles.forEach((element) => element.remove());
    ctx.officeBubbles.clear();
    onMovement({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: 0.38,
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      activity: "exploring the Town Square",
      space: "town-square",
      moving: false,
    });
  }

  function completeOfficeRoofExit({
    parachute = true,
    activity = "jumping from the Office roof",
  } = {}) {
    if (
      ctx.officeSceneMode !== "lobby" ||
      ctx.officeCurrentFloorId !== "rooftop"
    ) {
      return false;
    }
    fns.standUpFromOfficeChair();
    const worldPosition = ctx.player.getWorldPosition(new THREE.Vector3());
    ctx.player.parent?.worldToLocal?.(worldPosition);
    ctx.player.position.copy(worldPosition);
    ctx.officeSceneMode = "town";
    if (ctx.officeAquarium) ctx.officeAquarium.setFishBodiesVisible(false);
    ctx.officeCurrentFloorId = "lobby";
    ctx.officeElevatorRide = null;
    fns.releaseOfficeElevatorCamera(false);
    ctx.currentSpace = "town-square";
    ctx.currentFloorY = fns.outdoorFloorYAt(ctx.player.position.x, ctx.player.position.z);
    ctx.officeRoofJumping = false;
    ctx.selectedLandmark = "";
    ctx.officeLocalParticipantId = "";
    ctx.officeWasMoving = false;
    ctx.officeExitPending = false;
    ctx.officeDoorwayEntryPending = false;
    ctx.officeLobbyPlayer.visible = false;
    ctx.player.visible = ctx.cameraMode !== "first-person";
    ctx.cameraFocus = null;
    fns.setCameraMode("third-person", "office-roof-jump");
    ctx.cameraZoom = Math.min(ctx.cameraZoom, 0.72);
    ctx.pinchStartZoom = ctx.cameraZoom;
    fns.setOfficeParticipants([]);
    ctx.officeBubbles.forEach((element) => element.remove());
    ctx.officeBubbles.clear();
    if (parachute) prepareRoofParachute();
    ctx.lastPosition.copy(ctx.player.position);
    onMovement({
      x: Number(ctx.player.position.x.toFixed(2)),
      y: Number(ctx.player.position.y.toFixed(2)),
      z: Number(ctx.player.position.z.toFixed(2)),
      heading: Number(ctx.player.rotation.y.toFixed(3)),
      activity,
      space: "town-square",
      moving: true,
    });
    // The controller owns the audited attendance checkout. Trigger it only
    // once the avatar has physically cleared the rooftop perimeter.
    ctx.officeExitHandler?.();
    return true;
  }

  function createRoofParachute() {
    const group = new THREE.Group();
    group.name = "forkmesh-mini-roof-parachute";
    group.visible = false;
    group.scale.setScalar(0.04);
    const canopy = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 1.55, 24, 12, 0, Math.PI * 2, 0, Math.PI / 2),
      resources.material(THREE, {
        color: "#58a6ff",
        emissive: "#123d68",
        emissiveIntensity: 0.24,
        roughness: 0.62,
        metalness: 0,
        side: THREE.DoubleSide,
        forceSinglePass: true,
      }, "MeshStandardMaterial"),
    );
    canopy.name = "forkmesh-mini-roof-parachute-canopy";
    canopy.position.y = 5.7;
    group.add(canopy);
    const cords = new THREE.LineSegments(
      new THREE.BufferGeometry().setAttribute(
        "position",
        new THREE.Float32BufferAttribute(
          [
            -1.35, 5.65, -0.45, -0.42, 2.55, 0,
            1.35, 5.65, -0.45, 0.42, 2.55, 0,
            -1.35, 5.65, 0.45, -0.42, 2.55, 0,
            1.35, 5.65, 0.45, 0.42, 2.55, 0,
          ],
          3,
        ),
      ),
      new THREE.LineBasicMaterial({
        color: "#e6edf3",
        transparent: true,
        opacity: 0.82,
      }),
    );
    cords.name = "forkmesh-mini-roof-parachute-cords";
    group.add(cords);
    return group;
  }

  function removeRoofParachute() {
    if (!ctx.roofParachute) return;
    ctx.player.remove(ctx.roofParachute.group);
    disposeObject3D(ctx.roofParachute.group);
    ctx.roofParachute = null;
  }

  function prepareRoofParachute() {
    removeRoofParachute();
    const group = createRoofParachute();
    ctx.player.add(group);
    ctx.roofParachute = {
      group,
      deployedAt: 0,
      landedAt: 0,
    };
  }

  function updateRoofParachute(time) {
    const parachute = ctx.roofParachute;
    if (!parachute) return false;
    if (
      !parachute.deployedAt &&
      ctx.jumpVelocity <= ROOF_PARACHUTE_DEPLOY_VELOCITY &&
      ctx.player.position.y > ctx.currentFloorY + 0.5
    ) {
      parachute.deployedAt = time;
      parachute.group.visible = true;
    }
    if (!parachute.deployedAt) return false;
    if (parachute.landedAt) {
      const collapse = clamp(
        (time - parachute.landedAt) / ROOF_PARACHUTE_COLLAPSE_MS,
        0,
        1,
      );
      parachute.group.scale.set(
        1 - collapse * 0.72,
        Math.max(0.03, 1 - collapse),
        1 - collapse * 0.72,
      );
      parachute.group.position.y = -collapse * 2.6;
      if (collapse >= 1) removeRoofParachute();
      return false;
    }
    const opening = clamp((time - parachute.deployedAt) / 360, 0, 1);
    const eased = 1 - (1 - opening) ** 3;
    parachute.group.scale.setScalar(Math.max(0.04, eased));
    parachute.group.rotation.z =
      Math.sin(time * 0.0032) * 0.055 * eased;
    return true;
  }

  function beginOfficeRoofJump() {
    if (
      ctx.officeSceneMode !== "lobby" ||
      ctx.officeCurrentFloorId !== "rooftop" ||
      ctx.officeElevatorRide ||
      ctx.officeRoofJumping
    ) {
      return false;
    }
    fns.standUpFromOfficeChair();
    fns.cancelDash();
    removeRoofParachute();
    ctx.officeRoofJumping = true;
    ctx.jumpVelocity = 10.8;
    ctx.jumpQueued = false;
    ctx.cameraFocus = null;
    return true;
  }

  function startZipLineRide() {
    if (
      ctx.zipLineRide ||
      ctx.officeSceneMode !== "lobby" ||
      ctx.officeCurrentFloorId !== "rooftop" ||
      ctx.officeElevatorRide
    ) {
      return false;
    }
    fns.cancelDash();
    removeRoofParachute();
    ctx.officeRoofJumping = false;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    if (
      !completeOfficeRoofExit({
        parachute: false,
        activity: ZIP_LINE_RIDING_ACTIVITY,
      })
    ) {
      return false;
    }
    fns.completeStartHereStep("explore");
    ctx.zipLineRide = {
      from: ctx.player.position.clone(),
      lastPosition: ctx.player.position.clone(),
      lift: 0,
      // A click from across the roof glides to the anchor instead of snapping.
      liftMs:
        ZIP_LINE_LIFT_MS +
        Math.hypot(
          ctx.zipLineStart.x - ctx.player.position.x,
          ctx.zipLineStart.z - ctx.player.position.z,
        ) * 15,
      progress: 0,
      speed: 0,
      lastEmit: 0,
    };
    ctx.player.rotation.y = ctx.zipLineHeading;
    ctx.zipLineStrap.visible = true;
    return true;
  }

  function startNearbyZipLineRide() {
    if (
      ctx.officeSceneMode !== "lobby" ||
      ctx.officeCurrentFloorId !== "rooftop"
    ) {
      return false;
    }
    const local = fns.officeAvatarLocalPosition(ctx.player);
    return (
      Math.hypot(
        local.x - ZIP_LINE_ROOF_LOCAL[0],
        local.z - ZIP_LINE_ROOF_LOCAL[1],
      ) <= ZIP_LINE_LAUNCH_RADIUS && startZipLineRide()
    );
  }

  function endZipLineRide() {
    ctx.zipLineRide = null;
    ctx.zipLineStrap.visible = false;
    ctx.zipLineTrolley.position.copy(ctx.zipLineStart);
    ctx.player.userData.leftArm.rotation.x = 0;
    ctx.player.userData.rightArm.rotation.x = 0;
    applyLegPitch(ctx.player, 0, 0);
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    fns.cancelDash();
  }

  // Returns true while the cable owns the avatar. Anything that moves the
  // avatar from outside (a teleport, a space change) drops it off the line.
  function updateZipLineRide(delta, time) {
    const ride = ctx.zipLineRide;
    if (ctx.player.position.distanceToSquared(ride.lastPosition) > 1) {
      endZipLineRide();
      return false;
    }
    const scale = ctx.player.scale.y || 1;
    const drop = ZIP_LINE_RIDER_DROP * scale;
    if (ride.lift < 1) {
      // Winch the rider off the roof to hanging height at the anchor.
      ride.lift = Math.min(1, ride.lift + (delta * 1000) / ride.liftMs);
      const eased = 1 - (1 - ride.lift) ** 3;
      ctx.player.position.set(
        ride.from.x + (ctx.zipLineStart.x - ride.from.x) * ride.lift,
        ride.from.y + (ctx.zipLineStart.y - drop - ride.from.y) * eased,
        ride.from.z + (ctx.zipLineStart.z - ride.from.z) * ride.lift,
      );
    } else {
      const remaining = (ctx.zipLineStopProgress - ride.progress) * ctx.zipLineLength;
      ride.speed = Math.min(
        ZIP_LINE_MAX_SPEED,
        ride.speed + ZIP_LINE_ACCELERATION * delta,
        Math.max(3, remaining * 1.6),
      );
      ride.progress = Math.min(
        ctx.zipLineStopProgress,
        ride.progress + (ride.speed * delta) / ctx.zipLineLength,
      );
      fns.zipLineCablePoint(ride.progress, ctx.player.position);
      ctx.player.position.y -= drop;
    }
    fns.zipLineCablePoint(ride.lift < 1 ? 0 : ride.progress, ctx.zipLineTrolley.position);
    ctx.zipLineStrap.scale.y = Math.max(
      0.01,
      ctx.zipLineTrolley.position.y -
        ctx.player.position.y -
        (ZIP_LINE_RIDER_DROP - ZIP_LINE_SHOULDER_DROP) * scale,
    );
    ctx.player.rotation.y = ctx.zipLineHeading;
    ctx.player.userData.leftArm.rotation.x = -0.34;
    ctx.player.userData.rightArm.rotation.x = -0.34;
    applyLegPitch(ctx.player, -0.42, -0.26);
    ctx.player.userData.inputEnergy = fns.decayedPointerEnergy(performance.now());
    animateAvatarActivity(ctx.player, time, delta, reducedMotion);
    if (
      !ctx.beachSceneActive &&
      Math.hypot(
        ctx.player.position.x - BEACH_CENTER_X,
        ctx.player.position.z - BEACH_CENTER_Z,
      ) <= ZIP_LINE_BEACH_ENTRY_RADIUS
    ) {
      fns.setBeachScene(true, { relocate: false });
    }
    ride.lastPosition.copy(ctx.player.position);
    ctx.lastPosition.copy(ctx.player.position);
    const arrived = ride.progress >= ctx.zipLineStopProgress;
    const now = performance.now();
    if (arrived || now - ride.lastEmit > 450) {
      ride.lastEmit = now;
      fns.queueMovementEvent({
        x: Number(ctx.player.position.x.toFixed(2)),
        y: Number(ctx.player.position.y.toFixed(2)),
        z: Number(ctx.player.position.z.toFixed(2)),
        heading: Number(ctx.player.rotation.y.toFixed(3)),
        space: ctx.currentSpace,
        moving: !arrived,
        activity: arrived
          ? "arriving at the ForkMesh beach"
          : ZIP_LINE_RIDING_ACTIVITY,
      });
    }
    // Let go just above the landing pad; ordinary gravity sets the rider down.
    if (arrived) endZipLineRide();
    return true;
  }

  function beginOfficeExit() {
    if (ctx.officeSceneMode === "town") return false;
    if (
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId !== "lobby"
    ) {
      return fns.travelToOfficeFloor("lobby");
    }
    ctx.officeExitPending = false;
    ctx.officeSlidingDoorOpen = 1;
    // Transition to one façade-safe third-person chase shot while still inside.
    // The ordinary render loop eases into this distance, and leaveOfficeInterior
    // keeps it unchanged after crossing for a continuous outside reveal.
    fns.setCameraMode("third-person", "office-exit-approach");
    ctx.cameraFocus = null;
    ctx.cameraZoom = Math.min(ctx.cameraZoom, OFFICE_EXIT_CAMERA_ZOOM);
    ctx.pinchStartZoom = ctx.cameraZoom;
    // Do not rewrite the avatar heading here. The visitor may already be
    // walking through the doorway, and preserving that pose lets the same
    // forward input continue smoothly outside.
    return true;
  }

  return {
    leaveOfficeInterior,
    completeOfficeRoofExit,
    removeRoofParachute,
    prepareRoofParachute,
    updateRoofParachute,
    beginOfficeRoofJump,
    startZipLineRide,
    startNearbyZipLineRide,
    updateZipLineRide,
    beginOfficeExit,
  };
}
