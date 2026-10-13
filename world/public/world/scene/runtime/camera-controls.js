import {
  canAccessOfficeFloor,
  officeFloorById,
  officeFloorY,
} from "../../world-office-tower.js";
import {
  CAMERA_PITCH_MAX,
  CAMERA_PITCH_MIN,
  FIRST_PERSON_PITCH_MAX,
  FIRST_PERSON_PITCH_MIN,
} from "../gameplay-constants.js";
import { clamp } from "../object-utils.js";

export function functions({
  THREE,
  onCameraMode,
  onMovement,
  onOfficeMovement,
  ctx,
  fns,
}) {
  let officeElevatorPriorCameraMode = null;
  let officeElevatorPriorCameraFov = null;


  function setCameraMode(mode, reason = "request") {
    const wantsFirstPerson =
      mode === true ||
      String(mode || "").trim().toLocaleLowerCase() === "first-person";
    const nextMode = wantsFirstPerson ? "first-person" : "third-person";
    const changed = nextMode !== ctx.cameraMode;
    if (changed) fns.cancelDash();
    ctx.cameraMode = nextMode;
    if (ctx.cameraMode === "first-person") {
      ctx.cameraFocus = null;
      if (ctx.bikeRide) {
        // Enter the rider's eyes facing the handlebars. Subsequent lane
        // curvature is applied as a wrapped heading delta, so an intentional
        // glance remains relative to the bicycle instead of drifting in world
        // space as the circular road turns underneath it.
        ctx.cameraYaw = Math.atan2(
          Math.sin(ctx.player.rotation.y),
          Math.cos(ctx.player.rotation.y),
        );
      }
      ctx.player.visible = false;
      if (ctx.officeSceneMode === "meeting") {
        const localParticipant =
          ctx.officeParticipants.get(ctx.officeLocalParticipantId);
        if (localParticipant) localParticipant.visible = false;
      }
      ctx.playerLabel.style.opacity = "0";
      ctx.playerLabel.style.visibility = "hidden";
    } else {
      ctx.player.visible = ctx.officeSceneMode !== "meeting";
      if (ctx.officeSceneMode === "meeting") {
        const localParticipant =
          ctx.officeParticipants.get(ctx.officeLocalParticipantId);
        if (localParticipant) localParticipant.visible = true;
      }
      // Leave the eyes at their neutral field of view so the next first-person
      // visit does not start already pinned against the zoom-out floor.
      ctx.firstPersonZoom = 1;
      ctx.camera.fov = 44;
      ctx.camera.updateProjectionMatrix();
    }
    ctx.renderer.domElement.dataset.cameraMode = ctx.cameraMode;
    if (changed) onCameraMode({ mode: ctx.cameraMode, reason });
    return ctx.cameraMode;
  }

  function lockOfficeElevatorCamera() {
    if (!ctx.officeElevatorCameraLocked) {
      officeElevatorPriorCameraMode = ctx.cameraMode;
      officeElevatorPriorCameraFov = ctx.camera.fov;
    }
    ctx.officeElevatorCameraLocked = true;
    ctx.officeElevatorCameraReleased = false;
    if (ctx.cameraMode !== "first-person") {
      setCameraMode("first-person", "office-elevator-enter");
    }
    if (ctx.camera.fov !== 58) {
      ctx.camera.fov = 58;
      ctx.camera.updateProjectionMatrix();
    }
  }

  function releaseOfficeElevatorCamera(released = true) {
    if (ctx.officeElevatorCameraLocked || officeElevatorPriorCameraMode) {
      const prior = officeElevatorPriorCameraMode || "third-person";
      ctx.officeElevatorCameraLocked = false;
      officeElevatorPriorCameraMode = null;
      if (ctx.cameraMode !== prior) {
        setCameraMode(prior, "office-elevator-exit");
      }
      if (Number.isFinite(officeElevatorPriorCameraFov)) {
        ctx.camera.fov = officeElevatorPriorCameraFov;
        ctx.camera.updateProjectionMatrix();
      }
      officeElevatorPriorCameraFov = null;
    }
    ctx.officeElevatorCameraReleased = Boolean(released);
  }

  function setCameraView(view = {}) {
    setCameraMode(
      view.mode === "first-person" ? "first-person" : "third-person",
      "shared-link",
    );
    const yaw = Number(view.yaw);
    const pitch = Number(view.pitch);
    const zoom = Number(view.zoom);
    if (Number.isFinite(yaw)) {
      ctx.cameraYaw = Math.atan2(Math.sin(yaw), Math.cos(yaw));
    }
    if (ctx.cameraMode === "first-person") {
      if (Number.isFinite(pitch)) {
        ctx.firstPersonPitch = clamp(
          pitch,
          FIRST_PERSON_PITCH_MIN,
          FIRST_PERSON_PITCH_MAX,
        );
      }
    } else if (Number.isFinite(pitch)) {
      ctx.cameraPitch = clamp(pitch, CAMERA_PITCH_MIN, CAMERA_PITCH_MAX);
    }
    if (Number.isFinite(zoom)) fns.setCameraZoom(zoom);
  }

  function getSavedViewState() {
    const cameraState = {
      mode: ctx.cameraMode,
      yaw: ctx.cameraYaw,
      pitch:
        ctx.cameraMode === "first-person" ? ctx.firstPersonPitch : ctx.cameraPitch,
      zoom: ctx.cameraMode === "first-person" ? ctx.firstPersonZoom : ctx.cameraZoom,
    };
    if (ctx.officeSceneMode !== "town") {
      const avatar =
        ctx.officeSceneMode === "meeting"
          ? ctx.officeParticipants.get(ctx.officeLocalParticipantId) || ctx.player
          : ctx.player;
      const local = fns.officeAvatarLocalPosition(avatar, new THREE.Vector3());
      return {
        office: true,
        floorId: ctx.officeCurrentFloorId,
        x: Number(local.x.toFixed(3)),
        y: Number(local.y.toFixed(3)),
        z: Number(local.z.toFixed(3)),
        heading: Number(avatar.rotation.y.toFixed(4)),
        camera: cameraState,
      };
    }
    return {
      office: false,
      floorId: "",
      x: Number(ctx.player.position.x.toFixed(3)),
      y: Number(ctx.player.position.y.toFixed(3)),
      z: Number(ctx.player.position.z.toFixed(3)),
      heading: Number(ctx.player.rotation.y.toFixed(4)),
      space: ctx.currentSpace,
      camera: cameraState,
    };
  }

  function restoreSavedViewState(view = {}) {
    if (view.office === true) {
      const floor = officeFloorById(String(view.floorId || ""));
      if (
        ctx.officeSceneMode === "town" ||
        !floor ||
        !canAccessOfficeFloor(ctx.officeFloorAccess, floor.id)
      ) {
        return false;
      }
      fns.enterOfficeLobby({ floorId: floor.id });
      const local = fns.nearestOfficeWalkablePosition(
        floor.id,
        new THREE.Vector3(
          Number(view.x) || 0,
          officeFloorY(floor.id) + 0.38,
          Number(view.z) || 0,
        ),
      );
      local.y = officeFloorY(floor.id) + 0.38;
      fns.applyOfficeAvatarLocalPosition(ctx.player, local);
      ctx.player.rotation.y = clamp(
        Number(view.heading) || 0,
        -Math.PI,
        Math.PI,
      );
      ctx.lastPosition.copy(ctx.player.position);
      ctx.cameraFocus = null;
      setCameraView(view.camera);
      return true;
    }
    if (ctx.officeSceneMode !== "town") return false;
    const restored = fns.setSpawn(view);
    if (restored) setCameraView(view.camera);
    return restored;
  }

  function focusRepositoryPortal(owner, name) {
    const key = `${String(owner || "").toLocaleLowerCase()}/${String(
      name || "",
    ).toLocaleLowerCase()}`;
    const record = ctx.repositoryPortals.get(key);
    if (!record?.group) return false;
    ctx.currentSpace = "town-square";
    ctx.currentFloorY = fns.outdoorFloorYAt(ctx.player.position.x, ctx.player.position.z);
    ctx.player.position.y = ctx.currentFloorY;
    fns.cancelDash();
    ctx.cameraFocus = record.group.position.clone();
    ctx.cameraFocus.y += 0.1;
    ctx.focusedRepositoryKey = key;
    // Every perimeter portal faces toward the world center. Put the camera on
    // that inward normal so the selected sunburst is presented straight-on.
    ctx.cameraYaw = -record.angle - Math.PI / 2;
    ctx.cameraPitch = 0.16;
    ctx.cameraZoom = Math.min(ctx.cameraZoom, 0.55);
    return true;
  }

  function clearFocus() {
    ctx.cameraFocus = null;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
  }

  function setControl(control, pressed) {
    const normalized = {
      forward: "KeyW",
      back: "KeyS",
      left: "KeyA",
      right: "KeyD",
    }[control];
    if (!normalized) return;
    if (pressed && !ctx.touchKeys.size && ctx.touchMovement.lengthSq() === 0) {
      ctx.movementInputStartedAt = ctx.movementInputStartedAt || performance.now();
    }
    if (pressed) ctx.touchKeys.add(normalized);
    else ctx.touchKeys.delete(normalized);
  }

  function setTouchMovement(x, y) {
    const nextX = Number(x);
    const nextY = Number(y);
    if (!Number.isFinite(nextX) || !Number.isFinite(nextY)) {
      ctx.touchMovement.set(0, 0);
      return { x: 0, y: 0, strength: 0 };
    }
    const length = Math.hypot(nextX, nextY);
    const scale = length > 1 ? 1 / length : 1;
    if (length > 0.01 && ctx.touchMovement.lengthSq() === 0) {
      ctx.movementInputStartedAt = ctx.movementInputStartedAt || performance.now();
    }
    ctx.touchMovement.set(nextX * scale, nextY * scale);
    if (ctx.touchMovement.lengthSq() < 0.0001) ctx.touchMovement.set(0, 0);
    return {
      x: ctx.touchMovement.x,
      y: ctx.touchMovement.y,
      strength: ctx.touchMovement.length(),
    };
  }

  function setTouchInteractionActive(active) {
    ctx.externalTouchInteractionActive = Boolean(active);
    if (
      !ctx.externalTouchInteractionActive &&
      !ctx.touchPointers.size &&
      ctx.pendingTouchResize
    ) {
      ctx.pendingTouchResize = false;
      ctx.resize();
    }
  }

  function movementInput() {
    const movement = ctx.movementVector.set(0, 0, 0);
    const forwardInput =
      Number(ctx.keys.has("KeyW") || ctx.keys.has("ArrowUp")) -
      Number(ctx.keys.has("KeyS") || ctx.keys.has("ArrowDown"));
    const rightInput =
      Number(ctx.keys.has("KeyD") || ctx.keys.has("ArrowRight")) -
      Number(ctx.keys.has("KeyA") || ctx.keys.has("ArrowLeft"));
    const touchStrength = Math.min(1, ctx.touchMovement.length());
    const forward = ctx.movementForward.set(
      -Math.sin(ctx.cameraYaw),
      0,
      -Math.cos(ctx.cameraYaw),
    );
    const right = ctx.movementRight.set(
      Math.cos(ctx.cameraYaw),
      0,
      -Math.sin(ctx.cameraYaw),
    );
    if (forwardInput || rightInput || touchStrength) {
      // Keyboard and analog movement follow the view direction. Only yaw
      // participates, so looking up or down never changes walking speed.
      movement.addScaledVector(forward, forwardInput);
      movement.addScaledVector(right, rightInput);
      movement.addScaledVector(forward, -ctx.touchMovement.y);
      movement.addScaledVector(right, ctx.touchMovement.x);
    }

    // Retain the programmatic directional API for older controllers. It must
    // use the same view-relative basis as keyboard and analog input so a
    // mounted quadcopter flies where its rider is facing.
    const legacyForwardInput =
      Number(ctx.touchKeys.has("KeyW")) - Number(ctx.touchKeys.has("KeyS"));
    const legacyRightInput =
      Number(ctx.touchKeys.has("KeyD")) - Number(ctx.touchKeys.has("KeyA"));
    movement.addScaledVector(forward, legacyForwardInput);
    movement.addScaledVector(right, legacyRightInput);
    const keyboardActive = Boolean(forwardInput || rightInput);
    const bikeDirection = clamp(
      forwardInput +
        Number(ctx.touchKeys.has("KeyW")) -
        Number(ctx.touchKeys.has("KeyS")) -
        ctx.touchMovement.y,
      -1,
      1,
    );
    const bikeSteer = clamp(
      rightInput + legacyRightInput + ctx.touchMovement.x,
      -1,
      1,
    );
    const sprinting =
      ctx.keys.has("ShiftLeft") || ctx.keys.has("ShiftRight");
    const legacyTouchActive = ctx.touchKeys.size > 0;
    const inputStrength =
      keyboardActive || legacyTouchActive ? 1 : touchStrength;
    ctx.movementInputState.keyboardActive = keyboardActive;
    ctx.movementInputState.bikeDirection = bikeDirection;
    ctx.movementInputState.bikeSteer = bikeSteer;
    ctx.movementInputState.ridingBike = Boolean(ctx.bikeRide);
    ctx.movementInputState.ridingCar = Boolean(ctx.carRide);
    ctx.movementInputState.sprinting = sprinting;
    ctx.movementInputState.touchStrength = touchStrength;
    ctx.movementInputState.inputStrength = inputStrength;
    if (movement.lengthSq()) movement.normalize();
    return ctx.movementInputState;
  }

  function queueMovementEvent(movement, office = false) {
    if (office) ctx.pendingOfficeMovementEvent = movement;
    else ctx.pendingMovementEvent = movement;
    const timer = office ? ctx.officeMovementEventTimer : ctx.movementEventTimer;
    if (timer) return;
    const scheduled = window.setTimeout(() => {
      if (office) {
        ctx.officeMovementEventTimer = 0;
        const next = ctx.pendingOfficeMovementEvent;
        ctx.pendingOfficeMovementEvent = null;
        if (!ctx.disposed && next) onOfficeMovement(next);
        return;
      }
      ctx.movementEventTimer = 0;
      const next = ctx.pendingMovementEvent;
      ctx.pendingMovementEvent = null;
      if (!ctx.disposed && next) onMovement(next);
    }, 0);
    if (office) ctx.officeMovementEventTimer = scheduled;
    else ctx.movementEventTimer = scheduled;
  }

  return {
    setCameraMode,
    lockOfficeElevatorCamera,
    releaseOfficeElevatorCamera,
    setCameraView,
    getSavedViewState,
    restoreSavedViewState,
    focusRepositoryPortal,
    clearFocus,
    setControl,
    setTouchMovement,
    setTouchInteractionActive,
    movementInput,
    queueMovementEvent,
  };
}
