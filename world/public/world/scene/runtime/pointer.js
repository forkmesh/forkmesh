import { OFFICE_AVATAR_RADIUS } from "../../world-office-tower.js";
import { resolveInstancedHits } from "../../world-resource-cache.js";
import { resetAvatarLookDirection } from "../avatars.js";
import { qaBoardHitAction } from "../boards-tasks.js";
import {
  CAMERA_LOOK_SENSITIVITY,
  CAMERA_PITCH_MAX,
  CAMERA_PITCH_MIN,
  CAMERA_ZOOM_MAX,
  CAMERA_ZOOM_MIN,
  FIRST_PERSON_PITCH_MAX,
  FIRST_PERSON_PITCH_MIN,
  FIRST_PERSON_ZOOM_MAX,
  FIRST_PERSON_ZOOM_MIN,
} from "../gameplay-constants.js";
import { safeSiteReferrerURL } from "../leaderboards.js";
import {
  MOVEMENT_KEYS,
  SPRINT_KEYS,
  officeScenePointIsWalkable,
} from "../movement.js";
import { clamp, objectIsEffectivelyVisible } from "../object-utils.js";
import { START_HERE_STEPS } from "../town.js";

export function functions({
  THREE,
  identity,
  onAdminGuestCopy,
  onAgentBotChat,
  onAvatarSelect,
  onCameraLook,
  onCreateRepository,
  onDrinkWater,
  onFederatedWorldTravel,
  onFediverseProfile,
  onInstanceHexagonSelect,
  onFloorTap,
  onForkbotChat,
  onLandmarkSelect,
  onLaunchMirrorSelect,
  onMastodonBoardSelect,
  onMastodonOpenLink,
  onModeration,
  onOfficeChairSelect,
  onOfficeEnter,
  onOfficeMeetingBoardSelect,
  onOfficeRooftopLaptopSelect,
  onOfficeTaskBoardSelect,
  onOfficeTaskWallAction,
  onOpenForkmeshSong,
  onOrgTeamAssign,
  onQaAction,
  onQaVerdict,
  onReferralBoardSelect,
  onRewardBoardHover,
  onSiteReferrerOpen,
  onStartHereSelect,
  onStartNodeDownload,
  onStartWorldSignSelect,
  onSystemCapacityTableSelect,
  onUnverifiedAvatarDelete,
  onWorldBulletinSelect,
  onWorldDiscordBoardSelect,
  onWorldGeneralChatSelect,
  ctx,
  fns,
}) {
  let draggedQaCard = null;
  const canvasRect = { left: 0, top: 0, width: 1, height: 1 };
  let diagnosticsPointerLastAt = 0;
  // Rolling 0..1 measure of how much the mouse is actually moving, decayed
  // between samples. It only drives the local avatar's antenna blink rate;
  // nothing about it is sent to other visitors.
  let pointerEnergy = 0;
  let pointerEnergyAt = 0;
  let pointerEnergyX = 0;
  let pointerEnergyY = 0;
  let draggedCampfireLog = null;
  let pointerGestureMoved = false;
  let lastGestureDragged = false;
  let pinchStartDistance = 0;

  // The treasury board only ever shows what the shell last handed to
  // updateRewardPool, and that balance costs a public Solana RPC round trip,
  // so it is fetched on demand instead of on a timer. Resting the pointer on
  // the SOL sign is the refresh gesture; the sample gate keeps a fast mouse
  // sweep from adding an intersect test to every pointer event, and the edge
  // transition means holding still asks only once.
  const REWARD_BOARD_HOVER_SAMPLE_MS = 120;
  let rewardBoardHoverSampledAt = 0;
  let rewardBoardHovered = false;
  let hoverPickFrame = 0;
  const hoverPickEvent = { clientX: 0, clientY: 0 };
  const hoverPickBounds = new THREE.Box3();
  const hoverPickMatrix = new THREE.Matrix4();
  let hoverPickBoundsOwner = null;


  function invalidateCanvasRect() {
    ctx.canvasRectStale = true;
  }

  function canvasClientRect() {
    if (ctx.canvasRectStale) {
      const bounds = ctx.renderer.domElement.getBoundingClientRect();
      canvasRect.left = bounds.left;
      canvasRect.top = bounds.top;
      canvasRect.width = bounds.width || 1;
      canvasRect.height = bounds.height || 1;
      ctx.canvasRectStale = false;
    }
    return canvasRect;
  }

  function pointerCoordinates(event) {
    const rect = canvasClientRect();
    ctx.pointer.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
    ctx.pointer.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;
  }

  function setCameraZoom(value) {
    const next = Number(value);
    if (!Number.isFinite(next)) return ctx.cameraZoom;
    if (ctx.cameraMode === "first-person") {
      // Once the eyes are already as wide as they go, another notch backwards
      // steps out of the avatar's head entirely — that is where the gesture
      // was heading anyway.
      if (
        next < FIRST_PERSON_ZOOM_MIN &&
        ctx.firstPersonZoom <= FIRST_PERSON_ZOOM_MIN
      ) {
        fns.setCameraMode("third-person", "zoom-out");
        if (ctx.pinchActive) {
          // Re-anchor a live pinch, otherwise the orbital camera inherits the
          // scale the gesture started at inside first person.
          ctx.pinchStartZoom = ctx.cameraZoom;
          const spread = touchDistance();
          if (spread > 0) pinchStartDistance = spread;
        }
        return ctx.cameraZoom;
      }
      ctx.firstPersonZoom = clamp(
        next,
        FIRST_PERSON_ZOOM_MIN,
        FIRST_PERSON_ZOOM_MAX,
      );
      ctx.camera.fov = clamp(44 / ctx.firstPersonZoom, 10, 110);
      ctx.camera.updateProjectionMatrix();
      return ctx.firstPersonZoom;
    }
    ctx.cameraZoom = clamp(next, CAMERA_ZOOM_MIN, CAMERA_ZOOM_MAX);
    // This is intentionally not forced. Pointer and pinch events can arrive
    // much faster than display frames; the LOD pass only has work when zoom
    // crosses its near/far boundary.
    fns.updateSceneLevelOfDetail();
    return ctx.cameraZoom;
  }

  function touchDistance() {
    if (ctx.touchPointers.size < 2) return 0;
    const values = ctx.touchPointers.values();
    const first = values.next().value;
    const second = values.next().value;
    if (!first || !second) return 0;
    return Math.hypot(
      first.x - second.x,
      first.y - second.y,
    );
  }

  function beginPinchIfReady() {
    if (ctx.touchPointers.size < 2) return;
    const distance = touchDistance();
    if (distance <= 0) return;
    pinchStartDistance = distance;
    ctx.pinchStartZoom =
      ctx.cameraMode === "first-person" ? ctx.firstPersonZoom : ctx.cameraZoom;
    ctx.pinchActive = true;
    pointerGestureMoved = true;
    ctx.renderer.domElement.dataset.dragging = "true";
  }

  function rotateCamera(deltaX, deltaY) {
    if (!Number.isFinite(deltaX) || !Number.isFinite(deltaY)) return;
    if (ctx.officeElevatorCameraLocked) return;
    // The canvas behaves like a grabbed world: pull the scene with the
    // pointer, so the camera turns opposite to the hand's travel direction.
    ctx.cameraYaw -= deltaX * CAMERA_LOOK_SENSITIVITY;
    if (ctx.cameraMode === "first-person") {
      ctx.firstPersonPitch = clamp(
        ctx.firstPersonPitch + deltaY * CAMERA_LOOK_SENSITIVITY,
        FIRST_PERSON_PITCH_MIN,
        FIRST_PERSON_PITCH_MAX,
      );
    } else {
      ctx.cameraPitch = clamp(
        ctx.cameraPitch + deltaY * CAMERA_LOOK_SENSITIVITY,
        CAMERA_PITCH_MIN,
        CAMERA_PITCH_MAX,
      );
    }
    // Camera orbit is not a pose on the visitor's own avatar. Their head and
    // torso stay centred and level in their own view, and the look angles
    // travel out with presence instead, so only other visitors see where this
    // one is looking.
    resetAvatarLookDirection(ctx.player);
    onCameraLook({
      yaw: ctx.cameraYaw,
      pitch: ctx.cameraMode === "first-person" ? ctx.firstPersonPitch : ctx.cameraPitch,
    });
  }

  function handlePointerDown(event) {
    if (event.pointerType === "mouse" && event.button !== 0) return;
    if (event.pointerType === "touch") {
      ctx.touchPointers.set(event.pointerId, {
        x: event.clientX,
        y: event.clientY,
      });
      try {
        ctx.renderer.domElement.setPointerCapture(event.pointerId);
      } catch (_) {}
      if (ctx.touchPointers.size === 2) beginPinchIfReady();
    }
    if (ctx.primaryPointerId !== null) return;
    cancelHoverPick();
    ctx.canvasRectStale = true;
    pointerCoordinates(event);
    ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
    const pressHits = resolveInstancedHits(
      ctx.raycaster.intersectObjects(ctx.interactive, false),
    );
    const logHit = pressHits.find(
      ({ object }) => object.visible && object.userData?.campfireLog,
    );
    if (logHit) {
      draggedCampfireLog = logHit.object;
      draggedCampfireLog.userData.campfireCarried = true;
      draggedCampfireLog.material.emissive?.set?.("#d88a43");
      draggedCampfireLog.material.emissiveIntensity = 0.5;
    }
    if (!draggedCampfireLog) {
      const qaHit = pressHits.find(
        ({ object }) => object === ctx.qaBoardFace,
      );
      if (qaHit) {
        const qaAction = qaBoardHitAction(qaHit.uv, ctx.qaBoardSnapshot);
        if (qaAction?.action === "swipe") {
          draggedQaCard = { moved: false };
          fns.showQaSwipeCues(true);
        } else if (qaAction) {
          onQaAction(qaAction);
        }
      }
    }
    ctx.primaryPointerId = event.pointerId;
    ctx.pointerStart.set(event.clientX, event.clientY);
    ctx.pointerLast.copy(ctx.pointerStart);
    pointerGestureMoved = false;
    ctx.renderer.domElement.dataset.dragging = "true";
    try {
      ctx.renderer.domElement.setPointerCapture(event.pointerId);
    } catch (_) {}
  }

  // Energy fades with a ~0.5s half life so the antenna slows back down soon
  // after the mouse stops instead of coasting.
  function decayedPointerEnergy(now) {
    if (!pointerEnergyAt) return 0;
    const elapsed = Math.max(0, now - pointerEnergyAt) / 1000;
    return pointerEnergy * Math.pow(0.25, elapsed);
  }

  function recordPointerEnergy(event, now) {
    const x = Number(event.clientX) || 0;
    const y = Number(event.clientY) || 0;
    if (pointerEnergyAt) {
      const moved = Math.hypot(x - pointerEnergyX, y - pointerEnergyY);
      // ~700px of travel inside one half life saturates the blink rate.
      pointerEnergy = Math.min(1, decayedPointerEnergy(now) + moved / 700);
    }
    pointerEnergyAt = now;
    pointerEnergyX = x;
    pointerEnergyY = y;
  }

  function updateRewardBoardHover(event, now) {
    if (now - rewardBoardHoverSampledAt < REWARD_BOARD_HOVER_SAMPLE_MS) return;
    rewardBoardHoverSampledAt = now;
    const sign = ctx.landmarkObjects.get("fountain")?.userData?.treasurySign;
    if (
      !sign ||
      ctx.officeSceneMode !== "town" ||
      !objectIsEffectivelyVisible(sign)
    ) {
      rewardBoardHovered = false;
      return;
    }
    pointerCoordinates(event);
    ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
    const hovered =
      ctx.raycaster.ray.intersectsBox(hoverPickBoxFor(sign)) &&
      ctx.raycaster.intersectObject(sign, true).length > 0;
    if (hovered === rewardBoardHovered) return;
    rewardBoardHovered = hovered;
    if (hovered) onRewardBoardHover();
  }

  function hoverPickBoxFor(object) {
    if (
      hoverPickBoundsOwner !== object ||
      !hoverPickMatrix.equals(object.matrixWorld)
    ) {
      hoverPickBounds.setFromObject(object);
      hoverPickMatrix.copy(object.matrixWorld);
      hoverPickBoundsOwner = object;
    }
    return hoverPickBounds;
  }

  function scheduleHoverPick(event) {
    hoverPickEvent.clientX = event.clientX;
    hoverPickEvent.clientY = event.clientY;
    if (hoverPickFrame) return;
    hoverPickFrame = window.requestAnimationFrame(runHoverPick);
  }

  function aquariumPanelReachable() {
    return (
      Boolean(ctx.aquariumPanel) &&
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId === "lobby"
    );
  }

  function updateAquariumPanelHover(event) {
    const panel = ctx.aquariumPanel;
    if (!panel) return;
    let hit = null;
    if (aquariumPanelReachable()) {
      pointerCoordinates(event);
      ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
      hit = ctx.raycaster.intersectObjects(panel.targets, false)[0] || null;
    }
    const target = panel.hover(hit?.object || null, hit?.point || null);
    const canvas = ctx.renderer.domElement;
    if ((canvas.dataset.hoverTarget || "") !== target) {
      canvas.dataset.hoverTarget = target;
    }
  }

  function runHoverPick() {
    hoverPickFrame = 0;
    if (ctx.disposed || ctx.primaryPointerId !== null) return;
    updateRewardBoardHover(hoverPickEvent, performance.now());
    updateAquariumPanelHover(hoverPickEvent);
  }

  function cancelHoverPick() {
    if (!hoverPickFrame) return;
    window.cancelAnimationFrame(hoverPickFrame);
    hoverPickFrame = 0;
  }

  function handlePointerMove(event) {
    const pointerNow = performance.now();
    recordPointerEnergy(event, pointerNow);
    if (diagnosticsPointerLastAt > 0) {
      ctx.diagnosticsPointerWorstGapMs = Math.max(
        ctx.diagnosticsPointerWorstGapMs,
        pointerNow - diagnosticsPointerLastAt,
      );
    }
    diagnosticsPointerLastAt = pointerNow;
    ctx.diagnosticsPointerMoves = Math.min(1_000_000, ctx.diagnosticsPointerMoves + 1);
    let trackedTouch = false;
    if (event.pointerType === "touch" && ctx.touchPointers.has(event.pointerId)) {
      trackedTouch = true;
      ctx.touchPointers.set(event.pointerId, {
        x: event.clientX,
        y: event.clientY,
      });
      if (ctx.touchPointers.size >= 2) {
        if (!ctx.pinchActive) beginPinchIfReady();
        const distance = touchDistance();
        if (ctx.pinchActive && distance > 0) {
          // Spreading two fingers zooms in; bringing them together zooms out.
          // Every move is clamped, so repeated gestures can traverse the entire
          // supported near/far range without overshooting it.
          setCameraZoom(
            ctx.pinchStartZoom * (pinchStartDistance / distance),
          );
          event.preventDefault();
        }
        // A two-finger gesture is zoom-only. Never let either constituent
        // pointer also rotate the camera.
        pointerGestureMoved = true;
        event.preventDefault();
        return;
      }
    }
    // Only an idle fine pointer is a hover: a held button is a camera drag or
    // an object gesture, not somebody looking at the treasury board.
    if (event.pointerType === "mouse" && ctx.primaryPointerId === null) {
      scheduleHoverPick(event);
    }
    if (event.pointerId !== ctx.primaryPointerId) return;
    if (draggedCampfireLog) {
      const point = groundPointAt(event.clientX, event.clientY);
      if (point) {
        draggedCampfireLog.position.set(
          point.x - ctx.campfire.position.x,
          0.34,
          point.z - ctx.campfire.position.z,
        );
      }
      pointerGestureMoved = true;
      return;
    }
    if (draggedQaCard) {
      const deltaX = event.clientX - ctx.pointerStart.x;
      const deltaY = event.clientY - ctx.pointerStart.y;
      draggedQaCard.moved =
        draggedQaCard.moved || Math.hypot(deltaX, deltaY) > 5;
      ctx.qaBoardFace.position.x = clamp(deltaX / 110, -1.35, 1.35);
      ctx.qaBoardFace.position.y = clamp(-deltaY / 110, -1.1, 1.1);
      ctx.qaBoardFace.rotation.z = clamp(-deltaX * 0.0022, -0.2, 0.2);
      pointerGestureMoved = true;
      event.preventDefault();
      return;
    }
    ctx.pointerCurrent.set(event.clientX, event.clientY);
    const movedDistance = ctx.pointerStart.distanceTo(ctx.pointerCurrent);
    const canRotate =
      event.pointerType === "mouse" ||
      (event.pointerType === "touch" &&
        trackedTouch &&
        ctx.touchPointers.size === 1 &&
        !ctx.pinchActive);
    if (canRotate && movedDistance > 4) {
      if (pointerGestureMoved) {
        rotateCamera(
          ctx.pointerCurrent.x - ctx.pointerLast.x,
          ctx.pointerCurrent.y - ctx.pointerLast.y,
        );
      } else {
        pointerGestureMoved = true;
        rotateCamera(
          ctx.pointerCurrent.x - ctx.pointerStart.x,
          ctx.pointerCurrent.y - ctx.pointerStart.y,
        );
      }
      event.preventDefault();
    } else if (movedDistance > 9) {
      pointerGestureMoved = true;
    }
    ctx.pointerLast.copy(ctx.pointerCurrent);
  }

  function officeObjectMatchesCurrentFloor(object) {
    if (!object || ctx.officeSceneMode === "town") return true;
    // A car-mounted selector's floor id is its destination, not the story the
    // mesh occupies. Keep every destination button clickable from the cabin;
    // the server-derived allowed flag remains the authority on travel.
    if (object.userData?.interactive === "office-elevator-floor") {
      return true;
    }
    let current = object;
    let floorId = "";
    let insideOffice = false;
    while (current) {
      if (!floorId && current.userData?.officeFloorId) {
        floorId = String(current.userData.officeFloorId);
      }
      if (current === ctx.officeInterior) {
        insideOffice = true;
        break;
      }
      current = current.parent;
    }
    if (!insideOffice) return false;
    return !floorId || floorId === ctx.officeCurrentFloorId;
  }

  function finishPointer(event, cancelled = false) {
    const wasPinching = ctx.pinchActive || ctx.touchPointers.size > 1;
    let remainingTouch = null;
    if (event.pointerType === "touch") {
      ctx.touchPointers.delete(event.pointerId);
      if (ctx.touchPointers.size < 2) {
        ctx.pinchActive = false;
        pinchStartDistance = 0;
      }
      remainingTouch = ctx.touchPointers.entries().next().value || null;
      if (
        !ctx.touchPointers.size &&
        !ctx.externalTouchInteractionActive &&
        ctx.pendingTouchResize
      ) {
        ctx.pendingTouchResize = false;
        ctx.resize();
      }
    }
    try {
      ctx.renderer.domElement.releasePointerCapture(event.pointerId);
    } catch (_) {}
    if (event.pointerId !== ctx.primaryPointerId) {
      if (wasPinching && remainingTouch && ctx.primaryPointerId !== null) {
        const [, point] = remainingTouch;
        ctx.pointerStart.set(point.x, point.y);
        ctx.pointerLast.copy(ctx.pointerStart);
        pointerGestureMoved = true;
      }
      if (!ctx.touchPointers.size) ctx.renderer.domElement.dataset.dragging = "false";
      return;
    }
    if (event.pointerType === "touch" && remainingTouch) {
      const [pointerId, point] = remainingTouch;
      ctx.primaryPointerId = pointerId;
      ctx.pointerStart.set(point.x, point.y);
      ctx.pointerLast.copy(ctx.pointerStart);
      pointerGestureMoved = true;
      ctx.renderer.domElement.dataset.dragging = "true";
      return;
    }
    ctx.primaryPointerId = null;
    ctx.renderer.domElement.dataset.dragging =
      ctx.touchPointers.size ? "true" : "false";
    if (draggedCampfireLog) {
      const droppedLog = draggedCampfireLog;
      draggedCampfireLog = null;
      const point = groundPointAt(event.clientX, event.clientY);
      if (point && Math.hypot(point.x - ctx.campfire.position.x, point.z - ctx.campfire.position.z) < 1.05) {
        droppedLog.visible = false;
        droppedLog.userData.campfireCarried = false;
        ctx.fireLevel = Math.min(2.4, ctx.fireLevel + 0.28);
      }
      lastGestureDragged = true;
      pointerGestureMoved = false;
      return;
    }
    if (draggedQaCard) {
      const deltaX = event.clientX - ctx.pointerStart.x;
      const deltaY = event.clientY - ctx.pointerStart.y;
      draggedQaCard = null;
      fns.resetQaCardTransform();
      let verdict = "";
      if (
        !cancelled &&
        Math.abs(deltaX) >= 70 &&
        Math.abs(deltaX) > Math.abs(deltaY)
      ) {
        verdict = deltaX > 0 ? "pass" : "fail";
      } else if (
        !cancelled &&
        deltaY >= 70 &&
        Math.abs(deltaY) > Math.abs(deltaX)
      ) {
        verdict = "unsure";
      }
      if (verdict) {
        fns.showQaSwipeCues(false);
        onQaVerdict({ verdict });
      } else {
        fns.showQaSwipeCues(true, true);
      }
      lastGestureDragged = true;
      pointerGestureMoved = false;
      return;
    }
    const suppressTap =
      cancelled ||
      wasPinching ||
      pointerGestureMoved;
    // Remembered past the reset below so a double-click that ended in a camera
    // drag or pinch does not also fire off a dash.
    lastGestureDragged = suppressTap;
    pointerGestureMoved = false;
    if (suppressTap) return;

    // Use the down coordinates so a small amount of click jitter cannot select
    // an object that was not underneath the visible cursor at press time.
    pointerCoordinates({
      clientX: ctx.pointerStart.x,
      clientY: ctx.pointerStart.y,
    });
    ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
    const hits = resolveInstancedHits(
      ctx.raycaster.intersectObjects(ctx.interactive, false),
    ).filter(
      ({ object }) =>
        objectIsEffectivelyVisible(object) &&
        officeObjectMatchesCurrentFloor(object),
    );
    const firstHit = hits[0];
    const avatarHit = hits.find(
      ({ object, distance }) =>
        ctx.avatarSelections.has(object) &&
        (!firstHit || distance <= firstHit.distance + 0.75),
    );
    const hit = avatarHit || firstHit;
    const avatarSelection = hit?.object
      ? ctx.avatarSelections.get(hit.object)
      : null;
    fns.setActiveAvatarSelection(avatarSelection?.avatar || null);
    if (hit?.object && fns.interactWithDistrictPackage(hit.object)) {
      return;
    }
    if (
      hit?.object?.userData?.interactive === "aquarium-control" &&
      aquariumPanelReachable()
    ) {
      ctx.aquariumPanel.press(String(hit.object.userData.aquariumControl || ""));
      return;
    }
    if (
      hit?.object?.userData?.interactive === "aquarium-glass" &&
      aquariumPanelReachable()
    ) {
      ctx.aquariumPanel.touchGlass(hit.point);
      return;
    }
    if (!hit) {
      const floorPoint =
        ctx.officeSceneMode === "town" && !ctx.beachSceneActive
          ? groundPointAt(ctx.pointerStart.x, ctx.pointerStart.y)
          : null;
      if (floorPoint) {
        onFloorTap({
          x: floorPoint.x,
          y: floorPoint.y,
          z: floorPoint.z,
          clientX: ctx.pointerStart.x,
          clientY: ctx.pointerStart.y,
        });
      }
      return;
    }
    if (
      ctx.officeSceneMode !== "town" &&
      hit?.object?.userData?.interactive === "office-marketing-task-board"
    ) {
      const action =
        hit.object === ctx.officeMarketingTaskBoard.userData.face
          ? fns.marketingTaskWallAction(hit.uv)
          : null;
      if (action?.action === "select-assignee") {
        return;
      }
      if (action) {
        onOfficeTaskWallAction(action);
      } else {
        onOfficeTaskBoardSelect({
          state: ctx.officeMarketingTaskBoard.userData.taskState,
          count: ctx.officeMarketingTaskBoard.userData.taskCount,
        });
      }
      return;
    }
    if (
      ctx.officeSceneMode !== "town" &&
      hit?.object?.userData?.interactive === "office-marketing-proof-desk"
    ) {
      onOfficeTaskWallAction({
        action: "proof",
        member: String(hit.object.userData.marketingMember || ""),
      });
      return;
    }
    if (
      ctx.officeSceneMode !== "town" &&
      hit?.object?.userData?.interactive ===
        "office-marketing-initiatives-board"
    ) {
      const action = fns.marketingInitiativeWallAction(hit.uv);
      if (action) onOfficeTaskWallAction(action);
      return;
    }
    if (hit?.object?.userData?.interactive === "world-bulletin-scroll-up") {
      fns.scrollWorldBulletin(-1);
      return;
    }
    if (hit?.object?.userData?.interactive === "world-bulletin-scroll-down") {
      fns.scrollWorldBulletin(1);
      return;
    }
    if (hit?.object?.userData?.interactive === "world-bulletin") {
      onWorldBulletinSelect();
      return;
    }
    if (hit?.object?.userData?.interactive === "world-general-chat-board") {
      onWorldGeneralChatSelect();
      return;
    }
    if (hit?.object?.userData?.interactive === "world-discord-board") {
      onWorldDiscordBoardSelect();
      return;
    }
    const recordPage = hit?.object?.userData?.repositoryRecordPage;
    if (recordPage?.enabled === true) {
      fns.changeRepositoryRecordPage(
        String(recordPage.kind || ""),
        Number(recordPage.direction) || 0,
      );
      return;
    }
    if (hit?.object?.userData?.interactive === "mastodon-kiosk-scroll-up") {
      fns.scrollMastodonKiosk(-1);
      return;
    }
    if (hit?.object?.userData?.interactive === "mastodon-kiosk-scroll-down") {
      fns.scrollMastodonKiosk(1);
      return;
    }
    if (
      String(hit?.object?.userData?.interactive || "").startsWith(
        "mastodon-kiosk-open-",
      )
    ) {
      const href = String(hit.object.userData.href || "");
      if (href) onMastodonOpenLink(href);
      return;
    }
    if (hit?.object?.userData?.interactive === "mastodon-board") {
      onMastodonBoardSelect();
      return;
    }
    if (hit?.object?.userData?.interactive === "social-banner-open") {
      const href = String(hit.object.userData.href || "");
      if (href) onMastodonOpenLink(href);
      return;
    }
    if (hit?.object?.userData?.interactive === "leaderboard-card") {
      // The card itself identifies the board now: no canvas-space cell
      // arithmetic, so a click always lands on the ranking being looked at.
      const boardId = String(hit.object.userData.boardId || "");
      if (boardId === "referrals") {
        onReferralBoardSelect();
      } else if (boardId === "http-referrers") {
        const href = safeSiteReferrerURL(
          ctx.leaderboardCardState.siteRows?.[0],
        );
        if (href) onSiteReferrerOpen(href);
      }
      return;
    }
    if (hit?.object?.userData?.interactive === "start-node-download") {
      onStartNodeDownload();
      return;
    }
    if (hit?.object?.userData?.interactive === "launch-vultr-mirror") {
      onLaunchMirrorSelect();
      return;
    }
    if (hit?.object?.userData?.interactive === "start-world-sign") {
      onStartWorldSignSelect(hit.object.userData.plot);
      return;
    }
    if (hit?.object?.userData?.interactive === "drinking-fountain") {
      ctx.drinkingUntil = performance.now() + 2200;
      onDrinkWater();
      return;
    }
    if (hit?.object?.userData?.interactive === "federated-world-portal") {
      onFederatedWorldTravel({
        origin: String(hit.object.userData.origin || ""),
        label: String(hit.object.userData.label || "ForkMesh instance"),
      });
      return;
    }
    if (hit?.object?.userData?.interactive === "instance-hexagon") {
      onInstanceHexagonSelect({
        url: String(hit.object.userData.url || ""),
        label: String(hit.object.userData.label || "ForkMesh instance"),
        worldUrl: String(hit.object.userData.worldUrl || ""),
        arrivalSlot: Number(hit.object.userData.arrivalSlot),
      });
      return;
    }
    if (hit?.object?.userData?.interactive === "beach-scene-enter") {
      ctx.enterBeachScene();
      return;
    }
    if (hit?.object?.userData?.interactive === "beach-scene-exit") {
      ctx.leaveBeachScene();
      return;
    }
    if (hit?.object?.userData?.interactive === "office-floor-warp") {
      fns.warpToOfficeFloor(hit.object.userData.officeFloorTargetId);
      return;
    }
    if (
      hit?.object?.userData?.interactive ===
      "office-lobby-return-portal"
    ) {
      fns.warpToOfficeFloor("lobby");
      return;
    }
    if (hit?.object?.userData?.interactive === "system-capacity-table") {
      onSystemCapacityTableSelect({
        name: String(hit.object.userData.tableName || ""),
        rowCount: Number(hit.object.userData.rowCount) || 0,
      });
      return;
    }
    if (
      hit?.object?.userData?.interactive === "office-meeting-board" &&
      (ctx.officeSceneMode === "meeting" ||
        (ctx.officeSceneMode === "lobby" &&
          ctx.officeCurrentFloorId === "marketing"))
    ) {
      // The board hangs on the Marketing floor. Clicking it through the glass
      // from another storey must not hand a visitor a seat at that table.
      onOfficeMeetingBoardSelect();
      return;
    }
    if (hit?.object?.userData?.interactive === "office-chair") {
      if (ctx.officeSceneMode === "meeting") {
        onOfficeChairSelect(hit.object.userData.officeChairId);
      } else if (ctx.officeSceneMode === "lobby") {
        fns.sitOnOfficeChair(hit.object.userData.officeChairId);
      }
      return;
    }
    if (
      hit?.object?.userData?.interactive === "office-rooftop-laptop"
    ) {
      onOfficeRooftopLaptopSelect();
      return;
    }
    if (hit?.object?.userData?.interactive === "office-roof-zipline") {
      fns.startZipLineRide();
      return;
    }
    if (hit?.object?.userData?.interactive === "start-here-map") {
      onStartHereSelect({
        completed: ctx.startHereCompleted.size,
        total: START_HERE_STEPS.length,
      });
      return;
    }
    const chestControl = hit?.object ? ctx.chestControls.get(hit.object) : null;
    if (chestControl && fns.handleChestControl(chestControl, hit)) {
      return;
    }
    if (avatarSelection) {
      const selectedMember = fns.avatarSelectionPayload(avatarSelection);
      if (
        avatarSelection.avatar?.userData?.fediverseProfile?.state === "idle"
      ) {
        avatarSelection.avatar.userData.fediverseProfile = {
          state: "loading",
        };
        onFediverseProfile({
          peerId: String(avatarSelection.peerId || ""),
          name: selectedMember.name,
          accountStatus: selectedMember.accountStatus,
          self: selectedMember.self,
        });
      }
      if (
        hit.object === avatarSelection.avatar?.userData?.verifiedPin &&
        identity.isAdmin === true &&
        selectedMember.self !== true &&
        selectedMember.emailVerified !== true &&
        selectedMember.accountStatus.toLowerCase() !== "guest"
      ) {
        onUnverifiedAvatarDelete(selectedMember);
        return;
      }
      onAvatarSelect(fns.avatarSelectionPayload(avatarSelection));
      return;
    }
    const moderationAction = hit?.object
      ? ctx.moderationActions.get(hit.object)
      : null;
    const guestCopyAction = hit?.object
      ? ctx.adminGuestCopyActions.get(hit.object)
      : null;
    if (guestCopyAction) {
      onAdminGuestCopy({ ...guestCopyAction });
      return;
    }
    if (moderationAction) {
      onModeration({
        targetType: moderationAction.targetType,
        handle: moderationAction.handle,
        peerId: moderationAction.peerId,
        name: moderationAction.name,
      });
      return;
    }
    const orgTeamAction = hit?.object ? ctx.orgTeamActions.get(hit.object) : null;
    if (orgTeamAction) {
      onOrgTeamAssign({
        org: orgTeamAction.org,
        member: orgTeamAction.member,
        peerId: orgTeamAction.peerId,
        name: orgTeamAction.name,
      });
      return;
    }
    if (hit?.object?.userData?.campfireLog) {
      hit.object.userData.campfireCarried = true;
      hit.object.material.emissive?.set?.("#d88a43");
      hit.object.material.emissiveIntensity = 0.5;
      return;
    }
    if (hit?.object?.userData?.campfirePit) {
      const carried = ctx.interactive.find((object) => object.userData?.campfireCarried);
      if (carried) {
        carried.userData.campfireCarried = false;
        carried.visible = false;
        ctx.fireLevel = Math.min(2.4, ctx.fireLevel + 0.28);
      }
      fns.focusCampfireCircle();
      return;
    }
    if (hit?.object?.userData?.campfireBench) {
      fns.sitOnCampfireBench(hit.object);
      return;
    }
    if (hit?.object?.userData?.worldSeat) {
      fns.sitOnWorldSeat(hit.object);
      return;
    }
    if (Number.isInteger(hit?.object?.userData?.swingSeat)) {
      fns.rideSwing(hit.object.userData.swingSeat);
      return;
    }
    if (Number.isInteger(hit?.object?.userData?.bikeIndex)) {
      fns.rideBike(hit.object.userData.bikeIndex);
      return;
    }
    if (Number.isInteger(hit?.object?.userData?.carIndex)) {
      fns.rideCar(hit.object.userData.carIndex);
      return;
    }
    if (Number.isInteger(hit?.object?.userData?.quadcopterIndex)) {
      fns.rideQuadcopter(hit.object.userData.quadcopterIndex);
      return;
    }
    if (hit?.object?.userData?.coasterTrain) {
      fns.rideCoaster();
      return;
    }
    if (hit?.object?.userData?.gymEquipment) {
      fns.beginGymExercise(String(hit.object.userData.gymEquipment));
      return;
    }
    if (hit?.object?.userData?.forkbotChat) {
      onForkbotChat();
      return;
    }
    if (hit?.object?.userData?.agentBotChat) {
      onAgentBotChat(
        String(hit.object.userData.agentBotChat),
        hit.object.userData.repositoryAgentSession || null,
      );
      return;
    }
    if (hit?.object?.userData?.openForkmeshSong) {
      onOpenForkmeshSong();
      return;
    }
    if (hit?.object?.userData?.createRepository) {
      onCreateRepository({
        provider: String(hit.object.userData.createRepository || ""),
      });
      return;
    }
    if (
      hit?.object?.userData?.interactive === "office-elevator-floor"
    ) {
      if (ctx.officeSceneMode !== "town") {
        ctx.officeFloorHandler?.({
          floorId: hit.object.userData.officeFloorId,
          allowed: hit.object.userData.officeFloorAllowed === true,
        });
      }
      return;
    }
    if (hit?.object?.userData?.officeEnter) {
      onOfficeEnter({
        source: "door",
      });
      return;
    }
    if (hit?.object?.userData?.nodeCabinet) {
      fns.focusNetworkNode(hit.object.userData.nodeCabinet.name);
      onLandmarkSelect("repositories", {
        source: "world",
        nodeCabinet: { ...hit.object.userData.nodeCabinet },
      });
      return;
    }
    if (hit?.object?.userData?.landmark) {
      const id = hit.object.userData.landmark;
      const repository =
        id === "repositories" && hit.object.userData.repositoryPortal
          ? { ...hit.object.userData.repositoryPortal }
          : null;
      const repositorySizeNode =
        id === "repositories" && hit.object.userData.repositorySizeNode
          ? { ...hit.object.userData.repositorySizeNode }
          : null;
      const repositoryStar =
        id === "repositories" && hit.object.userData.repositoryStar
          ? { ...hit.object.userData.repositoryStar }
          : null;
      const repositoryBase =
        id === "repositories" && hit.object.userData.repositoryBase
          ? { ...hit.object.userData.repositoryBase }
          : null;
      const repositoryFediverseFollow =
        id === "repositories" &&
        hit.object.userData.repositoryFediverseFollow
          ? { ...hit.object.userData.repositoryFediverseFollow }
          : null;
      const repositoryIssuePage =
        id === "repositories" && hit.object.userData.repositoryIssuePage
          ? { ...hit.object.userData.repositoryIssuePage }
          : null;
      const repositoryIssueAgentProvider =
        id === "repositories" &&
        hit.object.userData.repositoryIssueAgentProvider
          ? { ...hit.object.userData.repositoryIssueAgentProvider }
          : null;
      const repositoryIssueAgentAssignment =
        id === "repositories" &&
        hit.object.userData.repositoryIssueAgentAssignment
          ? { ...hit.object.userData.repositoryIssueAgentAssignment }
          : null;
      const repositoryPullPage =
        id === "repositories" && hit.object.userData.repositoryPullPage
          ? { ...hit.object.userData.repositoryPullPage }
          : null;
      if (
        !repository &&
        !repositorySizeNode &&
        !repositoryStar &&
        !repositoryBase &&
        !repositoryFediverseFollow &&
        !repositoryIssuePage &&
        !repositoryIssueAgentProvider &&
        !repositoryIssueAgentAssignment &&
        !repositoryPullPage
      ) {
        fns.focusLandmark(id);
      } else if (repository || repositorySizeNode) {
        const target = repository || repositorySizeNode;
        fns.focusRepositoryPortal(target.owner, target.name);
      }
      onLandmarkSelect(id, {
        source: "world",
        mediaSpaceId: String(
          hit.object.userData.mediaSpaceId || "",
        ).slice(0, 40),
        repository,
        repositorySizeNode,
        repositoryStar,
        repositoryBase,
        repositoryFediverseFollow,
        repositoryIssuePage,
        repositoryIssueAgentProvider,
        repositoryIssueAgentAssignment,
        repositoryPullPage,
        graphNode:
          id === "repositories" && hit.object.userData.graphNode
            ? { ...hit.object.userData.graphNode }
            : null,
      });
      return;
    }
  }

  function handlePointerUp(event) {
    finishPointer(event, false);
  }

  // Raycast against the current shared floor so double-click travel stays
  // accurate throughout the Town Square and its regional campuses.
  function groundPointAt(clientX, clientY) {
    pointerCoordinates({ clientX, clientY });
    ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
    ctx.groundPlane.constant = -ctx.currentFloorY;
    const point = ctx.raycaster.ray.intersectPlane(
      ctx.groundPlane,
      new THREE.Vector3(),
    );
    if (!point || !Number.isFinite(point.x) || !Number.isFinite(point.z)) {
      return null;
    }
    if (ctx.officeSceneMode === "lobby") {
      const localPoint = ctx.officeInterior.worldToLocal(point.clone());
      if (
        !officeScenePointIsWalkable(
          ctx.officeCurrentFloorId,
          localPoint.x,
          localPoint.z,
          OFFICE_AVATAR_RADIUS,
        )
      ) {
        return null;
      }
    } else if (!ctx.activeWalkSurfaceContains(point.x, point.z)) {
      return null;
    }
    point.y = ctx.currentFloorY;
    return point;
  }

  function handleDoubleClick(event) {
    if (event.button !== undefined && event.button !== 0) return;
    if (lastGestureDragged) return;
    if (ctx.officeSceneMode === "meeting" || ctx.officeElevatorRide) return;
    // A quick double tap on a bench is still a request to sit on it, not to
    // dash to the patch of ground the bench happens to stand on.
    pointerCoordinates(event);
    ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
    const hits = resolveInstancedHits(
      ctx.raycaster.intersectObjects(ctx.interactive, false),
    );
    if (ctx.officeSceneMode === "lobby") {
      const officeHit = hits.find(
        ({ object }) =>
          objectIsEffectivelyVisible(object) &&
          officeObjectMatchesCurrentFloor(object),
      );
      if (officeHit) {
        // The two click events already performed the object interaction.
        // Suppress only the follow-up dash through that same object/floor.
        event.preventDefault();
        return;
      }
    }
    const benchHit = hits.find(
      ({ object }) =>
        object.userData?.campfireBench && objectIsEffectivelyVisible(object),
    );
    if (benchHit) {
      event.preventDefault();
      fns.sitOnCampfireBench(benchHit.object);
      return;
    }
    // The same rule for the swing set: a double tap on a seat is a request to
    // ride it, not to dash to the ground the swing hangs over.
    const swingHit = hits.find(
      ({ object }) =>
        Number.isInteger(object.userData?.swingSeat) &&
        objectIsEffectivelyVisible(object),
    );
    if (swingHit) {
      event.preventDefault();
      fns.rideSwing(swingHit.object.userData.swingSeat);
      return;
    }
    const bikeHit = hits.find(
      ({ object }) =>
        Number.isInteger(object.userData?.bikeIndex) &&
        objectIsEffectivelyVisible(object),
    );
    if (bikeHit) {
      event.preventDefault();
      fns.rideBike(bikeHit.object.userData.bikeIndex);
      return;
    }
    const quadcopterHit = hits.find(
      ({ object }) =>
        Number.isInteger(object.userData?.quadcopterIndex) &&
        objectIsEffectivelyVisible(object),
    );
    if (quadcopterHit) {
      event.preventDefault();
      fns.rideQuadcopter(quadcopterHit.object.userData.quadcopterIndex);
      return;
    }
    const coasterHit = hits.find(
      ({ object }) =>
        object.userData?.coasterTrain === true &&
        objectIsEffectivelyVisible(object),
    );
    if (coasterHit) {
      event.preventDefault();
      fns.rideCoaster();
      return;
    }
    const gymHit = hits.find(
      ({ object }) =>
        Boolean(object.userData?.gymEquipment) &&
        objectIsEffectivelyVisible(object),
    );
    if (gymHit) {
      event.preventDefault();
      fns.beginGymExercise(String(gymHit.object.userData.gymEquipment));
      return;
    }
    const point = groundPointAt(event.clientX, event.clientY);
    if (!point) return;
    event.preventDefault();
    // Following the avatar again keeps the dash visible; a landmark focus left
    // over from the two selection clicks would pin the camera in place.
    ctx.cameraFocus = null;
    ctx.dashTarget = point;
  }

  function handlePointerCancel(event) {
    finishPointer(event, true);
  }

  function handleWheel(event) {
    const lineHeight = 16;
    const deltaPixels =
      event.deltaY *
      (event.deltaMode === WheelEvent.DOM_DELTA_LINE
        ? lineHeight
        : event.deltaMode === WheelEvent.DOM_DELTA_PAGE
          ? Math.max(1, ctx.renderer.domElement.clientHeight)
          : 1);
    if (!Number.isFinite(deltaPixels) || deltaPixels === 0) return;
    event.preventDefault();
    pointerCoordinates(event);
    ctx.raycaster.setFromCamera(ctx.pointer, ctx.camera);
    const bulletinHit = ctx.raycaster
      .intersectObjects([ctx.bulletinFrame, ctx.bulletinFace, ctx.bulletinScrollUp, ctx.bulletinScrollDown], false)
      .find(({ object }) => objectIsEffectivelyVisible(object));
    if (bulletinHit) {
      fns.scrollWorldBulletin(Math.sign(deltaPixels));
      return;
    }
    const repositoryRecordHit = ctx.repositoryRecordBoardNearby
      ? ctx.raycaster
        .intersectObjects(fns.repositoryRecordWheelTargetList(), false)
        .find(({ object }) => objectIsEffectivelyVisible(object))
      : null;
    if (repositoryRecordHit) {
      fns.changeRepositoryRecordPage(
        String(repositoryRecordHit.object.userData.repositoryRecordPageKind),
        Math.sign(deltaPixels),
      );
      return;
    }
    if (fns.mastodonKioskMaxOffset() > 0) {
      const kioskHit = ctx.raycaster
        .intersectObjects(ctx.mastodonKioskWheelTargets, false)
        .find(({ object }) => objectIsEffectivelyVisible(object));
      if (kioskHit) {
        fns.scrollMastodonKiosk(Math.sign(deltaPixels));
        return;
      }
    }
    const firstPerson = ctx.cameraMode === "first-person";
    const currentZoom = firstPerson ? ctx.firstPersonZoom : ctx.cameraZoom;
    // Third person pulls the camera back as the wheel scrolls down; through
    // the visitor's own eyes that reads backwards, so first person scrolls
    // the other way — wheel down zooms in on what they are looking at.
    setCameraZoom(
      currentZoom * Math.exp(deltaPixels * (firstPerson ? -0.0015 : 0.0015)),
    );
  }

  function handleKeyDown(event) {
    if (
      event.target instanceof HTMLInputElement ||
      event.target instanceof HTMLTextAreaElement ||
      event.target instanceof HTMLSelectElement ||
      event.target instanceof HTMLButtonElement ||
      event.target instanceof HTMLAnchorElement ||
      event.target?.isContentEditable
    ) return;
    if (MOVEMENT_KEYS.has(event.code)) {
      if (!ctx.keys.size && !event.repeat) {
        const now = performance.now();
        const eventAt = Number(event.timeStamp);
        // Event timestamps normally share performance.now()'s time origin.
        // Fall back for older WebViews that report an epoch timestamp.
        const startedAt =
          Number.isFinite(eventAt) &&
          eventAt > 0 &&
          Math.abs(now - eventAt) < 60_000
            ? eventAt
            : now;
        ctx.movementInputStartedAt = ctx.movementInputStartedAt || startedAt;
      }
      ctx.keys.add(event.code);
      event.preventDefault();
    }
    if (SPRINT_KEYS.has(event.code)) {
      ctx.keys.add(event.code);
      event.preventDefault();
    }
    if (
      (ctx.quadcopterRide || ctx.jetpackEquipped) &&
      ["Space", "KeyC", "ControlLeft", "ControlRight"].includes(event.code)
    ) {
      ctx.keys.add(event.code);
      event.preventDefault();
      return;
    }
    if (event.code === "Space") {
      const roofJumpStarted = fns.beginOfficeRoofJump();
      const canJump =
        ctx.officeSceneMode === "town" &&
        ctx.player.position.y <= ctx.currentFloorY + 0.02;
      if (!roofJumpStarted && canJump) {
        ctx.jumpQueued = true;
      }
      event.preventDefault();
    }
    if (event.code === "KeyE" && !event.repeat) {
      let enteredDistrictRide = false;
      for (let index = 0; index < ctx.districtPackages.length; index += 1) {
        const record = ctx.districtPackages[index];
        if (record.instance.interactNearest?.(ctx.player, record.frame) === true) {
          enteredDistrictRide = true;
          break;
        }
      }
      if (
        enteredDistrictRide ||
        fns.toggleNearestCoasterRide() ||
        fns.toggleNearestQuadcopterRide() ||
        fns.toggleNearestBikeRide() ||
        fns.startNearbyZipLineRide()
      ) {
        event.preventDefault();
      }
    }
    if (event.code === "Escape") {
      fns.setActiveAvatarSelection(null);
      if (ctx.cameraMode === "first-person") fns.setCameraMode("third-person");
      else fns.clearFocus();
    }
  }

  function handleKeyUp(event) {
    ctx.keys.delete(event.code);
    if (
      SPRINT_KEYS.has(event.code) ||
      (MOVEMENT_KEYS.has(event.code) &&
       ![...ctx.keys].some((code) => MOVEMENT_KEYS.has(code)))
    ) {
      ctx.keyboardMovementSpeed = fns.baseMoveSpeed();
    }
  }

  function handleWindowBlur() {
    ctx.keys.clear();
    ctx.touchKeys.clear();
    ctx.touchMovement.set(0, 0);
    ctx.touchPointers.clear();
    ctx.keyboardMovementSpeed = fns.baseMoveSpeed();
    ctx.jumpQueued = false;
    ctx.jumpVelocity = 0;
    fns.cancelDash();
    ctx.primaryPointerId = null;
    pointerGestureMoved = false;
    ctx.pinchActive = false;
    pinchStartDistance = 0;
    ctx.renderer.domElement.dataset.dragging = "false";
  }

  return {
    invalidateCanvasRect,
    pointerCoordinates,
    setCameraZoom,
    handlePointerDown,
    decayedPointerEnergy,
    handlePointerMove,
    handlePointerUp,
    handleDoubleClick,
    handlePointerCancel,
    handleWheel,
    handleKeyDown,
    handleKeyUp,
    handleWindowBlur,
  };
}
