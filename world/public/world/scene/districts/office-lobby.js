import {
  OFFICE_DOOR_WIDTH,
  OFFICE_FLOORS,
  OFFICE_FRONT_Z,
  OFFICE_RECEPTION_DESK,
  officeFloorY,
  officeHexWallPoint,
} from "../../world-office-tower.js";
import {
  OFFICE_RECEPTION_GUEST_TIPS,
  OFFICE_RECEPTION_MEMBER_TIPS,
  OFFICE_RECEPTION_RESET_RANGE,
  OFFICE_RECEPTION_TALK_COOLDOWN_MS,
  OFFICE_RECEPTION_TALK_RANGE,
} from "../agent-bots.js";
import { createAvatar } from "../avatars.js";
import {
  OFFICE_FLOOR_WARP_DOOR_HEIGHT,
  OFFICE_FLOOR_WARP_DOOR_SPACING,
  OFFICE_FLOOR_WARP_DOOR_START_X,
  OFFICE_FLOOR_WARP_DOOR_WIDTH,
  OFFICE_FLOOR_WARP_DOOR_Z,
} from "../gameplay-constants.js";
import { makeOfficeWallPlacard } from "../labels.js";
import { makeSharedMaterial } from "../materials.js";
import {
  officeDoorStatusTexture,
  officeReclaimedWoodTexture,
} from "../office-building.js";
import {
  normalizeOfficeMarketingTasks,
  officeMarketingAttendanceTexture,
  officeMarketingInitiativesTexture,
  officeMarketingTasksBlankTexture,
} from "../office-marketing.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
  fns,
}) {
  let officeSlidingDoorPlaced = false;
  let officeTableLogoTexture = null;
  let officeTableLogoTextureLoading = false;
  let officeReceptionWasNear = false;
  let officeReceptionLastTipAt = -Infinity;
  let officeReceptionGuestTipIndex = 0;
  let officeReceptionMemberTipIndex = 0;

  function setOfficeDoorStatus(state = "open") {
    if (!ctx.officeDoorStatus) return "open";
    const normalized =
      state === "syncing" ? "syncing" : state === "ready" ? "ready" : "open";
    if (ctx.officeDoorStatus.userData.officeDoorStatus === normalized) {
      ctx.officeDoorStatus.visible = normalized !== "open";
      return normalized;
    }
    const previous = ctx.officeDoorStatus.material.map;
    ctx.officeDoorStatus.material.map = officeDoorStatusTexture(THREE, normalized);
    ctx.officeDoorStatus.material.needsUpdate = true;
    ctx.officeDoorStatus.userData.officeDoorStatus = normalized;
    ctx.officeDoorStatus.visible = normalized !== "open";
    previous?.dispose?.();
    return normalized;
  }
  function updateOfficeSlidingDoors(_time, delta = 0.016) {
    const localPosition = fns.officeAvatarLocalPosition(
      ctx.player,
      ctx.officeDoorLocalPosition,
    );
    const onEntranceFloor =
      ctx.officeSceneMode === "town" ||
      (ctx.officeSceneMode === "lobby" && ctx.officeCurrentFloorId === "lobby");
    const approaching =
      onEntranceFloor &&
      Math.abs(localPosition.x) <= OFFICE_DOOR_WIDTH / 2 + 3.2 &&
      Math.abs(localPosition.z - OFFICE_FRONT_Z) <= 7.5;
    const target =
      approaching || ctx.officeDoorwayEntryPending || ctx.officeExitPending ? 1 : 0;
    if (Math.abs(target - ctx.officeSlidingDoorOpen) < 1e-4) {
      if (ctx.officeSlidingDoorOpen !== target) {
        ctx.officeSlidingDoorOpen = target;
        officeSlidingDoorPlaced = false;
      }
    } else {
      const travel = Math.min(1, Math.max(0, Number(delta) || 0) * 5.8);
      ctx.officeSlidingDoorOpen +=
        (target - ctx.officeSlidingDoorOpen) * travel;
      officeSlidingDoorPlaced = false;
    }
    if (!officeSlidingDoorPlaced) {
      officeSlidingDoorPlaced = true;
      ctx.officeSlidingDoorPanels.forEach((panel, index) => {
        const side = index === 0 ? -1 : 1;
        panel.position.x =
          side *
          THREE.MathUtils.lerp(
            ctx.officeDoorClosedX,
            ctx.officeDoorOpenX,
            ctx.officeSlidingDoorOpen,
          );
      });
    }
    if (ctx.officeDoorStatus) {
      const syncing =
        ctx.officeDoorStatus.userData.officeDoorStatus === "syncing";
      if (syncing || ctx.officeDoorStatus.material.opacity !== 1) {
        ctx.officeDoorStatus.material.opacity = syncing
          ? 0.74 + (Math.sin(performance.now() * 0.009) + 1) * 0.13
          : 1;
      }
    }
  }

  function officeReceptionDeskDistance(localPosition) {
    const x = Number(localPosition?.x) || 0;
    const z = Number(localPosition?.z) || 0;
    const distanceX = Math.max(
      0,
      Math.abs(x - OFFICE_RECEPTION_DESK.x) - OFFICE_RECEPTION_DESK.halfWidth,
    );
    const distanceZ = Math.max(
      0,
      Math.abs(z - OFFICE_RECEPTION_DESK.z) - OFFICE_RECEPTION_DESK.halfDepth,
    );
    return Math.hypot(distanceX, distanceZ);
  }

  function updateOfficeReceptionGuide(time = performance.now()) {
    const active =
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId === "lobby" &&
      !ctx.officeElevatorRide;
    if (!active) {
      officeReceptionWasNear = false;
      return false;
    }
    const distance = officeReceptionDeskDistance(
      fns.officeAvatarLocalPosition(ctx.player, ctx.officeReceptionLocalPosition),
    );
    if (distance > OFFICE_RECEPTION_RESET_RANGE) {
      officeReceptionWasNear = false;
      return false;
    }
    if (
      distance > OFFICE_RECEPTION_TALK_RANGE ||
      officeReceptionWasNear
    ) {
      return false;
    }
    officeReceptionWasNear = true;
    const now = Number.isFinite(Number(time))
      ? Number(time)
      : performance.now();
    if (
      now - officeReceptionLastTipAt <
      OFFICE_RECEPTION_TALK_COOLDOWN_MS
    ) {
      return false;
    }
    const authenticated = ctx.officeFloorAccess.authenticated === true;
    const tips = authenticated
      ? OFFICE_RECEPTION_MEMBER_TIPS
      : OFFICE_RECEPTION_GUEST_TIPS;
    const index = authenticated
      ? officeReceptionMemberTipIndex
      : officeReceptionGuestTipIndex;
    const message = tips[index % tips.length];
    if (authenticated) {
      officeReceptionMemberTipIndex = (index + 1) % tips.length;
    } else {
      officeReceptionGuestTipIndex = (index + 1) % tips.length;
    }
    const shown = fns.showAvatarChatBubble(
      ctx.officeReceptionNoah,
      message,
      { officeReception: true },
    );
    if (shown) officeReceptionLastTipAt = now;
    return shown;
  }
  function loadOfficeTableLogoTexture() {
    if (officeTableLogoTexture || officeTableLogoTextureLoading || ctx.disposed) {
      return;
    }
    officeTableLogoTextureLoading = true;
    new THREE.TextureLoader().load(
      "/assets/world/marketing-table-cube-inlay.webp",
      (texture) => {
        officeTableLogoTextureLoading = false;
        if (ctx.disposed) {
          texture.dispose();
          return;
        }
        texture.colorSpace = THREE.SRGBColorSpace;
        texture.needsUpdate = true;
        officeTableLogoTexture = texture;
        ctx.officeTableLogoMaterial.map = texture;
        ctx.officeTableLogoMaterial.color.set("#ffffff");
        ctx.officeTableLogoMaterial.needsUpdate = true;
      },
      undefined,
      () => {
        officeTableLogoTextureLoading = false;
      },
    );
  }

  return {
    setOfficeDoorStatus,
    updateOfficeSlidingDoors,
    updateOfficeReceptionGuide,
    loadOfficeTableLogoTexture,
  };
}

export function build({
  THREE,
  addOfficeDeferredFloorDisplay,
  avatarPartInstances,
  identity,
  interactive,
  landmarkObjects,
  officeChairs,
  officeInterior,
  registerAvatarChestControls,
}) {
  const officeBuilding = landmarkObjects.get("office");
  const officeSlidingDoorPanels =
    officeBuilding?.userData?.officeDoorPanels || [];
  const officeDoorStatus =
    officeBuilding?.userData?.officeDoorStatus || null;
  const officeDoorClosedX =
    Number(officeBuilding?.userData?.officeDoorClosedX) ||
    OFFICE_DOOR_WIDTH / 4;
  const officeDoorOpenX =
    Number(officeBuilding?.userData?.officeDoorOpenX) ||
    OFFICE_DOOR_WIDTH * 0.76;

  const officeTable = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 5.9, 5.65, 0.5, 48),
    resources.uniqueMaterial(THREE, {
      color: "#ffffff",
      map: officeReclaimedWoodTexture(THREE),
      roughness: 0.48,
      metalness: 0.04,
      clearcoat: 0.72,
      clearcoatRoughness: 0.16,
    }, "MeshPhysicalMaterial"),
  );
  officeTable.name = "forkmesh-office-marketing-tabletop";
  // Lift the sealed top clear of the floor plane so the green floor can never
  // z-fight through it at shallow camera angles.
  officeTable.position.set(0, officeFloorY("marketing") + 2.1, 0);
  officeInterior.add(officeTable);
  const officeTableLogoMaterial = resources.uniqueMaterial(THREE, {
    color: "#071524",
    toneMapped: false,
  }, "MeshBasicMaterial");
  const officeTableLogo = new THREE.Mesh(
    resources.primitive(THREE, "CircleGeometry", 2.18, 48),
    officeTableLogoMaterial,
  );
  officeTableLogo.name = "forkmesh-office-marketing-cube-inlay";
  officeTableLogo.rotation.x = -Math.PI / 2;
  officeTableLogo.position.set(0, officeFloorY("marketing") + 2.365, 0);
  officeInterior.add(officeTableLogo);
  const officeTableEpoxy = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 2.28, 2.28, 0.09, 48),
    resources.material(THREE, {
      color: "#dff9ff",
      transparent: true,
      opacity: 0.3,
      roughness: 0.03,
      metalness: 0,
      clearcoat: 1,
      clearcoatRoughness: 0.015,
      depthWrite: false,
    }, "MeshPhysicalMaterial"),
  );
  officeTableEpoxy.name = "forkmesh-office-marketing-logo-epoxy";
  officeTableEpoxy.position.set(0, officeFloorY("marketing") + 2.395, 0);
  officeInterior.add(officeTableEpoxy);
  for (const x of [-2.5, 2.5]) {
    for (const z of [-2.5, 2.5]) {
      const tableLeg = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.19, 0.25, 1.6, 12),
        makeSharedMaterial(THREE, "#10231e", { metalness: 0.42 }),
      );
      tableLeg.name = "forkmesh-office-marketing-table-leg";
      tableLeg.position.set(x, officeFloorY("marketing") + 1.2, z);
      officeInterior.add(tableLeg);
    }
  }
  // Meeting chairs are already about a seated avatar's hip height above the
  // office floor (0.4), so their sitters' shins reach the floor unchanged.
  const OFFICE_CHAIR_SEAT_TOP_Y = 0.91;
  const chairTransforms = [
    [-7.4, 0],
    [-5.25, -5.25],
    [0, -7.4],
    [5.25, -5.25],
    [7.4, 0],
    [5.25, 5.25],
    [0, 7.4],
    [-5.25, 5.25],
  ];
  chairTransforms.forEach(([x, z], index) => {
    const chairId = `chair-${index + 1}`;
    const yaw = Math.atan2(-x, -z);
    const chair = new THREE.Group();
    chair.name = `forkmesh-office-${chairId}`;
    const seat = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 1.05, 0.18, 1.05),
      makeSharedMaterial(THREE, "#2f6d56", { roughness: 0.72 }),
    );
    seat.position.y = OFFICE_CHAIR_SEAT_TOP_Y - 0.09;
    chair.add(seat);
    const back = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 1.05, 1.45, 0.18),
      makeSharedMaterial(THREE, "#347a61", { roughness: 0.7 }),
    );
    // The back is on the outside edge, leaving every seat visibly facing the
    // table. The seated avatar uses yaw + PI and therefore faces inward too.
    back.name = `forkmesh-office-${chairId}-back`;
    back.position.set(0, 1.42, -0.48);
    chair.add(back);
    chair.position.set(x, officeFloorY("marketing"), z);
    chair.rotation.y = yaw;
    chair.userData.officeChairId = chairId;
    chair.userData.officeFloorId = "marketing";
    chair.userData.officeSeatTopY = OFFICE_CHAIR_SEAT_TOP_Y;
    chair.traverse((child) => {
      if (!child.isMesh) return;
      child.userData.officeChairId = chairId;
      child.userData.officeFloorId = "marketing";
      child.userData.interactive = "office-chair";
      interactive.push(child);
    });
    officeChairs.set(chairId, chair);
    officeInterior.add(chair);
  });
  const officeMarketingTaskBoard = new THREE.Group();
  officeMarketingTaskBoard.name = "forkmesh-office-marketing-task-board";
  const taskBoardMount = officeHexWallPoint("back-left", 0, 0.55);
  officeMarketingTaskBoard.position.set(
    taskBoardMount.x,
    officeFloorY("marketing") + 6.4,
    taskBoardMount.z,
  );
  officeMarketingTaskBoard.rotation.y = taskBoardMount.rotationY;
  const officeMarketingTaskBoardFrame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 18.5, 11.4, 0.18),
    makeSharedMaterial(THREE, "#315b52", {
      metalness: 0.34,
      roughness: 0.48,
      emissive: "#173f34",
      emissiveIntensity: 0.24,
    }),
  );
  officeMarketingTaskBoardFrame.name =
    "forkmesh-office-marketing-task-board-frame";
  officeMarketingTaskBoardFrame.userData.officeFloorId = "marketing";
  officeMarketingTaskBoardFrame.userData.interactive =
    "office-marketing-task-board";
  interactive.push(officeMarketingTaskBoardFrame);
  officeMarketingTaskBoard.add(officeMarketingTaskBoardFrame);
  const officeMarketingTaskBoardFace = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 18.12, 11.02),
    resources.uniqueMaterial(THREE, {
      map: officeMarketingTasksBlankTexture(THREE),
    }, "MeshBasicMaterial"),
  );
  officeMarketingTaskBoardFace.name =
    "forkmesh-office-marketing-task-board-face";
  officeMarketingTaskBoardFace.position.z = 0.101;
  officeMarketingTaskBoardFace.userData.officeFloorId = "marketing";
  officeMarketingTaskBoardFace.userData.interactive =
    "office-marketing-task-board";
  interactive.push(officeMarketingTaskBoardFace);
  officeMarketingTaskBoard.add(officeMarketingTaskBoardFace);
  officeMarketingTaskBoard.userData.face = officeMarketingTaskBoardFace;
  officeMarketingTaskBoard.userData.taskState = "vacant";
  officeMarketingTaskBoard.userData.taskCount = 0;
  addOfficeDeferredFloorDisplay(
    officeInterior,
    officeMarketingTaskBoard,
    "marketing",
  );
  const officeMarketingInitiativesBoard = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 12.6, 12.6),
    resources.uniqueMaterial(THREE, {
      map: officeMarketingInitiativesTexture(
        THREE,
        normalizeOfficeMarketingTasks({ authorized: false }),
      ),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  officeMarketingInitiativesBoard.name =
    "forkmesh-office-marketing-initiatives-board";
  officeMarketingInitiativesBoard.position.set(
    -8,
    officeFloorY("marketing") + 6.5,
    -OFFICE_FRONT_Z + 0.66,
  );
  officeMarketingInitiativesBoard.userData.officeFloorId = "marketing";
  officeMarketingInitiativesBoard.userData.interactive =
    "office-marketing-initiatives-board";
  interactive.push(officeMarketingInitiativesBoard);
  addOfficeDeferredFloorDisplay(
    officeInterior,
    officeMarketingInitiativesBoard,
    "marketing",
  );
  // Marketing roster furniture is rebuilt from the authoritative team list.
  // It never rides multiplayer presence and disappears when the room is empty.
  const officeMarketingRosterGroup = new THREE.Group();
  officeMarketingRosterGroup.name = "forkmesh-office-marketing-roster-desks";
  officeInterior.add(officeMarketingRosterGroup);
  const officeMarketingAttendanceBoard = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 25, 12.5),
    resources.uniqueMaterial(THREE, {
      map: officeMarketingAttendanceTexture(
        THREE,
        normalizeOfficeMarketingTasks({ authorized: false }),
      ),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  officeMarketingAttendanceBoard.name =
    "forkmesh-office-marketing-attendance-calendar";
  const attendanceCalendarMount = officeHexWallPoint("back-right", 0, 0.52);
  officeMarketingAttendanceBoard.position.set(
    attendanceCalendarMount.x,
    officeFloorY("marketing") + 7,
    attendanceCalendarMount.z,
  );
  officeMarketingAttendanceBoard.rotation.y =
    attendanceCalendarMount.rotationY;
  officeMarketingAttendanceBoard.userData.officeFloorId = "marketing";
  addOfficeDeferredFloorDisplay(
    officeInterior,
    officeMarketingAttendanceBoard,
    "marketing",
  );
  const officeLight = new THREE.PointLight("#ffd8a3", 5.5, 28, 1.6);
  officeLight.position.set(0, officeFloorY("marketing") + 6.2, 0);
  officeInterior.add(officeLight);
  const officeLobbyPlayer = createAvatar(
    THREE,
    {
      ...identity,
      id: `${String(identity?.id || "visitor").slice(0, 24)}-office`,
    },
    { scale: 0.9, instances: avatarPartInstances },
  );
  officeLobbyPlayer.name = "forkmesh-office-lobby-player";
  officeLobbyPlayer.position.set(0, 0.38, 4.62);
  officeLobbyPlayer.rotation.y = Math.PI;
  officeLobbyPlayer.visible = false;
  officeInterior.add(officeLobbyPlayer);
  registerAvatarChestControls(officeLobbyPlayer, identity.id);

  // Staffed reception: Noah is a scene-native avatar, so visitors meet one
  // person at the far-wall desk instead of a modal login wall.
  const officeReception = new THREE.Group();
  officeReception.name = "forkmesh-office-reception";
  const receptionDesk = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      OFFICE_RECEPTION_DESK.halfWidth * 2,
      1.65,
      OFFICE_RECEPTION_DESK.halfDepth * 2,
    ),
    makeSharedMaterial(THREE, "#5b493c", {
      metalness: 0.25,
      roughness: 0.55,
    }),
  );
  receptionDesk.name = "forkmesh-office-reception-desk";
  receptionDesk.position.set(
    OFFICE_RECEPTION_DESK.x,
    1.05,
    OFFICE_RECEPTION_DESK.z,
  );
  officeReception.add(receptionDesk);
  const officeReceptionNoah = createAvatar(
    THREE,
    {
      id: "office-greeter-noah",
      name: "Noah · Welcome desk",
      flag: "◌",
      browser: "Office",
      os: "Welcome desk",
      status: "available",
      accountStatus: "Supporting member",
      nodes: [],
      outfitColor: "ocean",
    },
    { remote: true, scale: 0.94, instances: avatarPartInstances },
  );
  officeReceptionNoah.position.set(
    OFFICE_RECEPTION_DESK.x,
    0.38,
    OFFICE_RECEPTION_DESK.z - 3.5,
  );
  officeReceptionNoah.rotation.y = Math.PI;
  const noahHair = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 1.05, 0.24, 0.86),
    makeSharedMaterial(THREE, "#211b17", { roughness: 0.82 }),
  );
  noahHair.position.set(0, 3.88, -0.08);
  officeReceptionNoah.add(noahHair);
  officeReception.add(officeReceptionNoah);
  const noahNameplate = makeOfficeWallPlacard(
    THREE,
    "NOAH",
    "WELCOME · WORLD & REPOSITORY HELP",
    "#9ef7c6",
    11.2,
    1.35,
  );
  noahNameplate.name = "forkmesh-office-reception-nameplate-noah";
  // The nameplate sits on the desk's front face instead of hovering over Noah.
  noahNameplate.position.set(
    OFFICE_RECEPTION_DESK.x,
    1.2,
    OFFICE_RECEPTION_DESK.z + 2.56,
  );
  officeReception.add(noahNameplate);
  officeInterior.add(officeReception);

  const officeFloorWarpDoors = [];
  const officeFloorWarpHub = new THREE.Group();
  officeFloorWarpHub.name = "forkmesh-office-floor-warp-hub";
  officeFloorWarpHub.position.set(0, 0, 0);
  officeFloorWarpHub.userData.officeFloorId = "lobby";
  OFFICE_FLOORS.forEach((floor, index) => {
    const accent = index % 2 ? "#77d9ff" : "#9ef7c6";
    const doorway = new THREE.Group();
    doorway.name = `forkmesh-office-floor-doorway-${floor.id}`;
    doorway.position.set(
      OFFICE_FLOOR_WARP_DOOR_START_X +
        index * OFFICE_FLOOR_WARP_DOOR_SPACING,
      0,
      OFFICE_FLOOR_WARP_DOOR_Z,
    );
    doorway.userData.officeFloorId = "lobby";
    doorway.userData.officeFloorTargetId = floor.id;

    const frameMaterial = makeSharedMaterial(THREE, "#132d28", {
      emissive: "#236847",
      emissiveIntensity: 0.42,
      metalness: 0.4,
      roughness: 0.36,
    });
    const frameMaterials = {
      allowed: makeSharedMaterial(THREE, "#173d32", {
        emissive: "#1f9c6a",
        emissiveIntensity: 0.72,
        metalness: 0.4,
        roughness: 0.36,
      }),
      locked: makeSharedMaterial(THREE, "#31282b", {
        emissive: "#6e2737",
        emissiveIntensity: 0.2,
        metalness: 0.4,
        roughness: 0.36,
      }),
    };
    const frames = [];
    const frameThickness = 0.24;
    const sideGeometry = resources.primitive(
      THREE,
      "BoxGeometry",
      frameThickness,
      OFFICE_FLOOR_WARP_DOOR_HEIGHT,
      0.4,
    );
    for (const side of [-1, 1]) {
      const jamb = new THREE.Mesh(sideGeometry, frameMaterial);
      frames.push(jamb);
      jamb.position.set(
        side * (OFFICE_FLOOR_WARP_DOOR_WIDTH / 2),
        OFFICE_FLOOR_WARP_DOOR_HEIGHT / 2,
        0,
      );
      doorway.add(jamb);
    }
    const lintel = new THREE.Mesh(
      resources.primitive(
        THREE,
        "BoxGeometry",
        OFFICE_FLOOR_WARP_DOOR_WIDTH + frameThickness,
        frameThickness,
        0.4,
      ),
      frameMaterial,
    );
    lintel.position.set(0, OFFICE_FLOOR_WARP_DOOR_HEIGHT, 0);
    doorway.add(lintel);
    frames.push(lintel);

    const portal = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", OFFICE_FLOOR_WARP_DOOR_WIDTH - 0.38, OFFICE_FLOOR_WARP_DOOR_HEIGHT - 0.36),
      resources.material(THREE, {
        color: accent,
        transparent: true,
        opacity: 0.22,
        side: THREE.DoubleSide,
        forceSinglePass: true,
        depthWrite: false,
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    portal.name = `forkmesh-office-floor-warp-${floor.id}`;
    portal.position.set(0, OFFICE_FLOOR_WARP_DOOR_HEIGHT / 2, 0.23);
    portal.userData.interactive = "office-floor-warp";
    portal.userData.officeFloorId = "lobby";
    portal.userData.officeFloorTargetId = floor.id;
    portal.userData.officeFloorNumber = floor.level + 1;
    doorway.add(portal);
    interactive.push(portal);

    const closedDoor = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", OFFICE_FLOOR_WARP_DOOR_WIDTH - 0.38, OFFICE_FLOOR_WARP_DOOR_HEIGHT - 0.36, 0.24),
      makeSharedMaterial(THREE, "#2b2628", {
        emissive: "#6e2737",
        emissiveIntensity: 0.25,
        metalness: 0.24,
        roughness: 0.72,
      }),
    );
    closedDoor.name = `forkmesh-office-floor-door-${floor.id}`;
    closedDoor.position.set(0, OFFICE_FLOOR_WARP_DOOR_HEIGHT / 2, 0.12);
    closedDoor.userData.interactive = "office-floor-warp";
    closedDoor.userData.officeFloorId = "lobby";
    closedDoor.userData.officeFloorTargetId = floor.id;
    closedDoor.userData.officeFloorNumber = floor.level + 1;
    doorway.add(closedDoor);
    interactive.push(closedDoor);

    const openLabel = makeOfficeWallPlacard(
      THREE,
      `${floor.level + 1} · ${floor.label.toUpperCase()}`,
      floor.id === "lobby" ? "YOU ARE HERE" : "OPEN · WALK THROUGH",
      accent,
      OFFICE_FLOOR_WARP_DOOR_WIDTH - 0.46,
      1.2,
    );
    openLabel.name = `forkmesh-office-floor-warp-label-${floor.id}`;
    openLabel.position.set(0, OFFICE_FLOOR_WARP_DOOR_HEIGHT - 0.72, 0.38);
    openLabel.userData.interactive = "office-floor-warp";
    openLabel.userData.officeFloorId = "lobby";
    openLabel.userData.officeFloorTargetId = floor.id;
    doorway.add(openLabel);
    interactive.push(openLabel);

    const lockedLabel = makeOfficeWallPlacard(
      THREE,
      `${floor.level + 1} · ${floor.label.toUpperCase()}`,
      "CLOSED · ACCESS REQUIRED",
      "#ff7b83",
      OFFICE_FLOOR_WARP_DOOR_WIDTH - 0.46,
      1.2,
    );
    lockedLabel.name = `forkmesh-office-floor-warp-locked-${floor.id}`;
    lockedLabel.position.copy(openLabel.position);
    lockedLabel.userData.interactive = "office-floor-warp";
    lockedLabel.userData.officeFloorId = "lobby";
    lockedLabel.userData.officeFloorTargetId = floor.id;
    doorway.add(lockedLabel);
    interactive.push(lockedLabel);

    officeFloorWarpDoors.push({
      floorId: floor.id,
      x: doorway.position.x,
      frames,
      frameMaterials,
      portal,
      closedDoor,
      openLabel,
      lockedLabel,
      allowed: false,
    });
    portal.visible = false;
    openLabel.visible = false;
    closedDoor.visible = true;
    lockedLabel.visible = true;
    officeFloorWarpHub.add(doorway);
  });
  officeInterior.add(officeFloorWarpHub);
  return {
    officeSlidingDoorPanels,
    officeDoorStatus,
    officeDoorClosedX,
    officeDoorOpenX,
    officeTableLogoMaterial,
    OFFICE_CHAIR_SEAT_TOP_Y,
    officeMarketingTaskBoard,
    officeMarketingInitiativesBoard,
    officeMarketingRosterGroup,
    officeMarketingAttendanceBoard,
    officeLobbyPlayer,
    officeReceptionNoah,
    officeFloorWarpDoors,
  };
}
