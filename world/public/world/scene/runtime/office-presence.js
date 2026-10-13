import { hashNumber } from "../../world-avatar-face.js";
import {
  canAccessOfficeFloor,
  officeFloorById,
  OFFICE_FRONT_Z,
  officeFloorY,
  officeHexHalfWidth,
  officeMeetingStandingPoint,
} from "../../world-office-tower.js";
import {
  applyLegPitch,
  applySeatedLegPose,
  createAvatar,
  seatedAvatarY,
  setAvatarWorkBadge,
  updateAvatarBadge,
} from "../avatars.js";
import {
  MASTODON_KIOSK_REFRESH_MS,
  MASTODON_KIOSK_VISIBLE_TOOTS,
  WORLD_BULLETIN_VISIBLE_EVENTS,
  drawMastodonCountdown,
  drawMastodonLastPost,
  mastodonKioskTexture,
  mastodonKioskVisibleToots,
  worldBulletinTexture,
  worldGeneralChatTexture,
} from "../boards.js";
import { repaintCanvasTexture } from "../canvas.js";
import { makePlayerLabel } from "../labels.js";
import { makeSharedMaterial } from "../materials.js";
import {
  clamp,
  disposeOwnedGeometry,
  disposeOwnedMaterial,
} from "../object-utils.js";
import { officeReclaimedWoodTexture } from "../office-building.js";
import {
  normalizeOfficeMarketingTasks,
  officeMarketingAttendanceTexture,
  officeMarketingDeskNameTexture,
  officeMarketingDeskProofTexture,
  officeMarketingInitiativesTexture,
  officeMarketingTasksBlankTexture,
  officeMarketingTasksTexture,
} from "../office-marketing.js";
import { canvasReadableImageURL } from "../repositories.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  labelLayer,
  ctx,
  fns,
}) {
  let worldBulletinEvents = [];
  let worldBulletinOffset = 0;
  let mastodonKioskSnapshot = null;
  let mastodonKioskOffset = 0;
  let mastodonKioskCountdownKey = "";
  let mastodonKioskLastPostSinceMs = null;
  let mastodonKioskLastPostKey = "";
  const mastodonKioskImages = new Map();
  let officeMarketingDeskInteractives = [];


  function enterOffice({ source = "" } = {}) {
    // A doorway callback is emitted only after the town walker verifies the
    // full avatar crossed the real opening. Trust that physical proof even when
    // a fast dash reaches it one frame before nearestLandmark updates proximity.
    if (
      !["doorway", "saved-view"].includes(source) &&
      ctx.officeZoneState !== "nearby"
    ) {
      return false;
    }
    ctx.selectedLandmark = "office";
    ctx.officeExitPending = false;
    ctx.officeDoorwayEntryPending = false;
    ctx.cameraFocus = null;
    if (source !== "doorway") fns.cancelDash();
    return true;
  }

  function enterOfficeLobby({ floorId = "lobby" } = {}) {
    // The tower is physically part of the World. Entry changes only the active
    // local avatar/floor controls; the campus, Town Square, sky, and other
    // visitors remain rendered around it.
    const enteringFromTown = ctx.officeSceneMode === "town";
    const leavingMeeting = ctx.officeSceneMode === "meeting";
    if (enteringFromTown)
      fns.dismountBike({ relocate: false });
    if (enteringFromTown)
      fns.dismountCar({ relocate: false });
    if (enteringFromTown)
      fns.dismountQuadcopter({ relocate: false });
    if (enteringFromTown && ctx.jetpackEquipped)
      fns.setJetpackEquipped(false);
    if (enteringFromTown)
      fns.stopGymExercise();
    standUpFromOfficeChair();
    const meetingAvatar = leavingMeeting
      ? ctx.officeParticipants.get(ctx.officeLocalParticipantId)
      : null;
    const requestedFloor = officeFloorById(floorId);
    const destinationFloor =
      !enteringFromTown &&
      requestedFloor &&
      canAccessOfficeFloor(ctx.officeFloorAccess, requestedFloor.id)
        ? requestedFloor
        : officeFloorById("lobby");
    ctx.officeSceneMode = "lobby";
    if (ctx.officeAquarium) ctx.officeAquarium.setFishBodiesVisible(true);
    ctx.officeCurrentFloorId = destinationFloor.id;
    ctx.officeElevatorRide = null;
    ctx.selectedLandmark = "office";
    ctx.currentSpace = `office-${destinationFloor.id}`;
    ctx.currentFloorY = officeFloorY(destinationFloor.id) + 0.38;
    ctx.cameraFocus = null;
    ctx.officeInterior.visible = true;
    fns.updateSceneLevelOfDetail(true);
    ctx.officeSlidingDoorOpen = 1;
    if (enteringFromTown) {
      const localPosition = fns.officeAvatarLocalPosition(
        ctx.player,
        new THREE.Vector3(),
      );
      // The town-wall crossing already verified that this is the physical
      // doorway. Preserve its exact X/Z threshold pose across the mode handoff;
      // subsequent lobby motion owns wall and outward-exit collision.
      localPosition.y = ctx.currentFloorY;
      fns.applyOfficeAvatarLocalPosition(ctx.player, localPosition);
    } else if (meetingAvatar) {
      const localPosition = fns.nearestOfficeWalkablePosition(
        destinationFloor.id,
        fns.officeAvatarLocalPosition(meetingAvatar),
      );
      localPosition.y = ctx.currentFloorY;
      fns.applyOfficeAvatarLocalPosition(ctx.player, localPosition);
      setOfficeParticipants([]);
      ctx.officeLocalParticipantId = "";
    } else {
      ctx.player.position.y = ctx.currentFloorY;
    }
    ctx.officeElevatorCar.position.y = officeFloorY(destinationFloor.id);
    ctx.officeElevatorDoors[0].position.x = -4.36;
    ctx.officeElevatorDoors[1].position.x = 4.36;
    ctx.officeLobbyPlayer.visible = false;
    ctx.player.visible = ctx.cameraMode !== "first-person";
    fns.completeStartHereStep("office");
    ctx.lastPosition.copy(ctx.player.position);
    if (!enteringFromTown) ctx.keyboardMovementSpeed = fns.baseMoveSpeed();
    ctx.officeExitPending = false;
    renderOfficeMarketingTasks();
    return true;
  }

  function officeChairTransform(chairId) {
    const chair = ctx.officeChairs.get(String(chairId || ""));
    if (!chair) return null;
    return {
      position: chair.position.clone(),
      yaw: chair.rotation.y,
      floorId: String(chair.userData.officeFloorId || "marketing"),
      seatTopY:
        Number(chair.userData.officeSeatTopY) ||
        ctx.OFFICE_CHAIR_SEAT_TOP_Y,
    };
  }

  function applyOfficeChairSeatPose() {
    const chair = ctx.officeChairs.get(String(ctx.officeChairSeat?.chairId || ""));
    if (!chair) {
      ctx.officeChairSeat = null;
      return false;
    }
    const seatPoint = chair.getWorldPosition(new THREE.Vector3());
    ctx.player.parent?.worldToLocal?.(seatPoint);
    const floorId = String(
      chair.userData.officeFloorId || "marketing",
    );
    const seatTopY =
      Number(chair.userData.officeSeatTopY) ||
      ctx.OFFICE_CHAIR_SEAT_TOP_Y;
    ctx.player.position.set(
      seatPoint.x,
      seatedAvatarY(
        officeFloorY(floorId) + seatTopY,
        ctx.player.scale.x,
      ),
      seatPoint.z,
    );
    ctx.player.rotation.y = chair.rotation.y + Math.PI;
    ctx.player.userData.leftArm.rotation.x = 0;
    ctx.player.userData.rightArm.rotation.x = 0;
    applySeatedLegPose(ctx.player);
    return true;
  }

  function standUpFromOfficeChair() {
    if (!ctx.officeChairSeat) return false;
    ctx.officeChairSeat = null;
    ctx.player.position.y = ctx.currentFloorY;
    applyLegPitch(ctx.player, 0, 0);
    return true;
  }

  function sitOnOfficeChair(chairId) {
    const normalized = String(chairId || "");
    const chair = ctx.officeChairs.get(normalized);
    if (
      ctx.officeSceneMode !== "lobby" ||
      !chair ||
      String(chair.userData.officeFloorId || "marketing") !==
        ctx.officeCurrentFloorId
    ) {
      return false;
    }
    if (ctx.officeChairSeat?.chairId === normalized) {
      standUpFromOfficeChair();
      return true;
    }
    ctx.officeChairSeat = { chairId: normalized };
    fns.cancelDash();
    ctx.cameraFocus = null;
    ctx.jumpVelocity = 0;
    ctx.jumpQueued = false;
    const seated = applyOfficeChairSeatPose();
    ctx.lastPosition.copy(ctx.player.position);
    return seated;
  }

  function applyOfficeParticipantPose(avatar, participant) {
    // The live meeting currently occupies the Marketing story. Keep this
    // explicit so DOM labels can obey floor occlusion; unlike Three.js meshes,
    // HTML overlays are not hidden by an opaque floor slab automatically.
    avatar.userData.officeFloorId = "marketing";
    const seated = participant?.pose === "seated" && participant?.chairId;
    const chair = seated ? officeChairTransform(participant.chairId) : null;
    if (chair) {
      avatar.position.copy(chair.position);
      avatar.position.y = seatedAvatarY(
        chair.position.y + ctx.OFFICE_CHAIR_SEAT_TOP_Y,
        avatar.scale.x,
      );
      avatar.rotation.y = chair.yaw + Math.PI;
      applySeatedLegPose(avatar);
    } else {
      const seed = hashNumber(participant?.id || "office-participant");
      // Attendees report the floor position they actually walked to. Folding
      // that into a box a few units wide around the middle of the room put
      // everyone who stepped away from the table — and everyone who had not
      // moved at all — inside the round tabletop instead of around it.
      const standing = officeMeetingStandingPoint(
        Number(participant?.x) || ((seed % 7) - 3) * 0.55,
        Number(participant?.z) || 4.15 + ((seed >> 3) % 3) * 0.35,
      );
      avatar.position.set(
        standing.x,
        officeFloorY("marketing") + 0.38,
        standing.z,
      );
      avatar.rotation.y = Number(participant?.yaw) || Math.PI;
      applyLegPitch(avatar, 0, 0);
    }
    avatar.userData.leftArm.rotation.x = 0;
    avatar.userData.rightArm.rotation.x = 0;
    avatar.userData.officePresence = { ...participant };
  }

  function setOfficeParticipants(participants = []) {
    participants = (Array.isArray(participants) ? participants : []).filter(
      (participant) =>
        !ctx.administrativelyRemovedAccounts.has(
          String(participant?.name || "").trim().toLowerCase(),
        ),
    );
    const seenOfficeParticipants = new Set();
    participants.forEach((participant) => {
      const id = String(participant?.id || "");
      if (!id) return;
      seenOfficeParticipants.add(id);
      let avatar = ctx.officeParticipants.get(id);
      // Meeting frames carry only a name and account status, so a member in a
      // room would otherwise wear a barer chest than the same member standing
      // in the square: fill in their public joined date and active time.
      const participantIdentity = fns.withMemberFacts({
        id,
        name: String(participant.name || "Office visitor").slice(0, 32),
        flag: "◌",
        browser: "Browser",
        os: "Device",
        status: participant.pose === "seated" ? "seated" : "in meeting",
        accountStatus: participant.accountStatus || "Guest",
        nodes: [],
      });
      if (!avatar) {
        avatar = createAvatar(THREE, participantIdentity, {
          remote: true,
          scale: 0.9,
          instances: ctx.avatarPartInstances,
        });
        ctx.officeInterior.add(avatar);
        ctx.officeParticipants.set(id, avatar);
        fns.registerWorldElement(
          "office-participants",
          "Office meeting participants",
          "Avatars & bots",
          avatar,
          () => ctx.officeParticipants.get(id) === avatar,
        );
        ctx.officeParticipantLabels.set(
          id,
          makePlayerLabel(avatar, labelLayer),
        );
        fns.registerAvatarChestControls(avatar, id);
      } else if (avatar.userData.name !== participantIdentity.name) {
        updateAvatarBadge(THREE, avatar, participantIdentity, true);
        ctx.officeParticipantLabels.get(id).textContent = participantIdentity.name;
      }
      if (id === ctx.officeLocalParticipantId) {
        setAvatarWorkBadge(
          THREE,
          avatar,
          ctx.selfWorkBoard,
          ctx.selfWorkBadgeVisible,
        );
      }
      fns.dressMemberFace(avatar, participantIdentity);
      applyOfficeParticipantPose(avatar, participant);
    });
    ctx.officeParticipants.forEach((avatar, id) => {
      if (seenOfficeParticipants.has(id)) return;
      fns.unregisterAvatarChestControls(avatar);
      ctx.officeInterior.remove(avatar);
      avatar.traverse((child) => {
        disposeOwnedGeometry(child.geometry);
        disposeOwnedMaterial(child.material, true);
      });
      ctx.officeParticipants.delete(id);
      ctx.officeParticipantLabels.get(id)?.remove();
      ctx.officeParticipantLabels.delete(id);
      ctx.officeBubbles.get(id)?.remove();
      ctx.officeBubbles.delete(id);
    });
    ctx.officeLobbyPlayer.visible =
      ctx.officeSceneMode === "meeting" &&
      (!ctx.officeLocalParticipantId ||
        !ctx.officeParticipants.has(ctx.officeLocalParticipantId));
    const localParticipant =
      ctx.officeParticipants.get(ctx.officeLocalParticipantId);
    if (localParticipant && ctx.officeSceneMode === "meeting") {
      localParticipant.visible = ctx.cameraMode !== "first-person";
    }
    renderOfficeMarketingTasks();
  }

  function officeMarketingRoomOccupied() {
    return (
      ctx.officeParticipants.size > 0 ||
      (ctx.officeSceneMode === "lobby" &&
        ctx.officeCurrentFloorId === "marketing")
    );
  }

  function rebuildOfficeMarketingRoster() {
    if (officeMarketingDeskInteractives.length) {
      const stale = new Set(officeMarketingDeskInteractives);
      for (let index = ctx.interactive.length - 1; index >= 0; index -= 1) {
        if (stale.has(ctx.interactive[index])) ctx.interactive.splice(index, 1);
      }
      officeMarketingDeskInteractives = [];
    }
    [...ctx.officeChairs.keys()]
      .filter((chairId) => chairId.startsWith("marketing-desk-chair:"))
      .forEach((chairId) => ctx.officeChairs.delete(chairId));
    ctx.officeMarketingRosterGroup.traverse((child) => {
      if (!child.isMesh) return;
      child.geometry?.dispose?.();
      child.material?.map?.dispose?.();
      child.material?.dispose?.();
    });
    ctx.officeMarketingRosterGroup.clear();
    const members = ctx.officeMarketingTaskSnapshot.members || [];
    const deskSlots = [];
    for (let row = 0; deskSlots.length < members.length && row < 6; row += 1) {
      const z = OFFICE_FRONT_Z - 7.8 - row * 6.8;
      const reach = Math.max(0, officeHexHalfWidth(z + 1.8, 1.2) - 3.7);
      const capacity = Math.floor((reach * 2) / 9) + 1;
      const count = Math.min(capacity, members.length - deskSlots.length);
      for (let column = 0; column < count; column += 1) {
        const x = (column - (count - 1) / 2) * 9;
        if (Math.hypot(x, z) >= 12) deskSlots.push({ x, z });
      }
    }
    members.forEach((member, index) => {
      const slot = deskSlots[index] || deskSlots[deskSlots.length - 1];
      const desk = new THREE.Group();
      desk.name = `forkmesh-office-marketing-desk:${member}`;
      const desktop = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 7.4, 0.34, 3.6),
        resources.uniqueMaterial(THREE, {
          color: "#76503a",
          map: officeReclaimedWoodTexture(THREE),
          roughness: 0.56,
          clearcoat: 0.42,
          clearcoatRoughness: 0.18,
        }, "MeshPhysicalMaterial"),
      );
      desktop.position.y = 1.7;
      desk.add(desktop);
      for (const x of [-3, 3]) {
        const leg = new THREE.Mesh(
          resources.primitive(THREE, "BoxGeometry", 0.22, 1.45, 2.4),
          makeSharedMaterial(THREE, "#10231e", { metalness: 0.38 }),
        );
        leg.position.set(x, 0.8, 0);
        desk.add(leg);
      }
      const nameplate = new THREE.Mesh(
        resources.primitive(THREE, "PlaneGeometry", 2.65, 0.66),
        resources.uniqueMaterial(THREE, {
          map: officeMarketingDeskNameTexture(THREE, member),
          toneMapped: false,
        }, "MeshBasicMaterial"),
      );
      nameplate.name = `forkmesh-office-marketing-desk-plaque:${member}`;
      nameplate.position.set(0, 2.22, -1.35);
      nameplate.rotation.y = Math.PI;
      desk.add(nameplate);
      const memberProofs = ctx.officeMarketingTaskSnapshot.proofs
        .filter((proof) => proof.member === member)
        .sort((left, right) => right.createdAt - left.createdAt);
      const canAddProof =
        ctx.officeMarketingTaskSnapshot.actor === member &&
        ctx.officeMarketingTaskSnapshot.members.includes(member);
      const proofPanel = new THREE.Mesh(
        resources.primitive(THREE, "PlaneGeometry", 2.7, 1.8),
        resources.uniqueMaterial(THREE, {
          map: officeMarketingDeskProofTexture(
            THREE,
            member,
            memberProofs,
            canAddProof,
          ),
          toneMapped: false,
        }, "MeshBasicMaterial"),
      );
      proofPanel.name = `forkmesh-office-marketing-proof-desk:${member}`;
      proofPanel.position.set(0, 2.1, 0.2);
      proofPanel.rotation.x = -Math.PI / 2;
      proofPanel.userData.officeFloorId = "marketing";
      if (canAddProof) {
        proofPanel.userData.interactive = "office-marketing-proof-desk";
        proofPanel.userData.marketingMember = member;
        officeMarketingDeskInteractives.push(proofPanel);
        ctx.interactive.push(proofPanel);
      }
      desk.add(proofPanel);
      // Desks line the front curtain wall and face out across Town Square.
      // Additional rows move inward while retaining a broad central aisle.
      desk.position.set(
        slot.x,
        officeFloorY("marketing"),
        slot.z,
      );
      desk.userData.officeFloorId = "marketing";
      desk.traverse((child) => {
        if (child.isMesh) child.userData.officeFloorId = "marketing";
      });
      ctx.officeMarketingRosterGroup.add(desk);
      const deskChair = new THREE.Group();
      deskChair.name = `forkmesh-office-marketing-desk-chair:${member}`;
      const deskChairId = `marketing-desk-chair:${member}`;
      const deskSeat = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 1.25, 0.2, 1.25),
        makeSharedMaterial(THREE, "#2f6d56", { roughness: 0.72 }),
      );
      deskSeat.position.y = 0.86;
      deskChair.add(deskSeat);
      const deskBack = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 1.25, 1.55, 0.2),
        makeSharedMaterial(THREE, "#347a61", { roughness: 0.7 }),
      );
      deskBack.position.set(0, 1.5, -0.56);
      deskChair.add(deskBack);
      deskChair.position.set(
        desk.position.x,
        officeFloorY("marketing"),
        desk.position.z - 3.2,
      );
      deskChair.userData.officeFloorId = "marketing";
      deskChair.userData.officeChairId = deskChairId;
      deskChair.userData.officeSeatTopY = 0.96;
      deskChair.traverse((child) => {
        if (child.isMesh) child.userData.officeFloorId = "marketing";
        if (!child.isMesh) return;
        child.userData.officeChairId = deskChairId;
        child.userData.interactive = "office-chair";
        officeMarketingDeskInteractives.push(child);
        ctx.interactive.push(child);
      });
      ctx.officeChairs.set(deskChairId, deskChair);
      ctx.officeMarketingRosterGroup.add(deskChair);
    });
    const previous = ctx.officeMarketingAttendanceBoard.material.map;
    ctx.officeMarketingAttendanceBoard.material.map =
      officeMarketingAttendanceTexture(THREE, ctx.officeMarketingTaskSnapshot);
    ctx.officeMarketingAttendanceBoard.material.needsUpdate = true;
    previous?.dispose?.();
  }

  function renderOfficeMarketingTasks() {
    const occupied = officeMarketingRoomOccupied();
    const key = JSON.stringify({
      occupied,
      snapshot: ctx.officeMarketingTaskSnapshot,
    });
    if (ctx.officeMarketingTaskBoard.userData.taskKey === key) {
      return ctx.officeMarketingTaskSnapshot;
    }
    const face = ctx.officeMarketingTaskBoard.userData.face;
    face.material.map?.dispose?.();
    face.material.map = occupied
      ? officeMarketingTasksTexture(THREE, ctx.officeMarketingTaskSnapshot)
      : officeMarketingTasksBlankTexture(THREE);
    face.material.needsUpdate = true;
    ctx.officeMarketingTaskBoard.userData.taskKey = key;
    ctx.officeMarketingTaskBoard.userData.taskState = occupied
      ? ctx.officeMarketingTaskSnapshot.state
      : "vacant";
    ctx.officeMarketingTaskBoard.userData.taskCount = occupied
      ? ctx.officeMarketingTaskSnapshot.tasks.length
      : 0;
    ctx.officeMarketingRosterGroup.visible =
      occupied && ctx.officeMarketingTaskSnapshot.authorized;
    ctx.officeMarketingAttendanceBoard.visible =
      occupied && ctx.officeMarketingTaskSnapshot.authorized;
    ctx.officeMarketingInitiativesBoard.visible =
      occupied && ctx.officeMarketingTaskSnapshot.authorized;
    const initiativesKey = JSON.stringify({
      occupied,
      initiatives: ctx.officeMarketingTaskSnapshot.initiatives,
    });
    if (ctx.officeMarketingInitiativesBoard.userData.initiativesKey !== initiativesKey) {
      const previous = ctx.officeMarketingInitiativesBoard.material.map;
      ctx.officeMarketingInitiativesBoard.material.map =
        officeMarketingInitiativesTexture(THREE, ctx.officeMarketingTaskSnapshot);
      ctx.officeMarketingInitiativesBoard.material.needsUpdate = true;
      ctx.officeMarketingInitiativesBoard.userData.initiativesKey = initiativesKey;
      previous?.dispose?.();
    }
    return ctx.officeMarketingTaskSnapshot;
  }

  function marketingInitiativeWallAction(uv) {
    if (
      !uv ||
      !officeMarketingRoomOccupied() ||
      !ctx.officeMarketingTaskSnapshot.authorized
    ) {
      return null;
    }
    const y = (1 - clamp(Number(uv.y) || 0, 0, 1)) * 1024;
    const row = Math.floor((y - 164) / 98);
    const initiative = ctx.officeMarketingTaskSnapshot.initiatives.slice(0, 8)[row];
    if (!initiative || y < 164 || y > 948) return null;
    return { action: "initiative", href: initiative.href };
  }

  function updateOfficeMarketingTasks(payload = {}) {
    ctx.officeMarketingTaskSnapshot = normalizeOfficeMarketingTasks(payload);
    rebuildOfficeMarketingRoster();
    return renderOfficeMarketingTasks();
  }

  function marketingTaskWallAction(uv) {
    if (
      !uv ||
      !officeMarketingRoomOccupied() ||
      !ctx.officeMarketingTaskSnapshot.authorized
    ) {
      return null;
    }
    const x = clamp(Number(uv.x) || 0, 0, 1) * 2048;
    const y = (1 - clamp(Number(uv.y) || 0, 0, 1)) * 1024;
    const snapshot = ctx.officeMarketingTaskSnapshot;
    if (snapshot.canManage && y >= 164 && y <= 272) {
      if (x < 1010 && snapshot.members.length) {
        const first = snapshot.members.shift();
        snapshot.members.push(first);
        ctx.officeMarketingTaskBoard.userData.taskKey = "";
        renderOfficeMarketingTasks();
        return { action: "select-assignee", assignee: snapshot.members[0] };
      }
      if (x >= 1010) {
        return {
          action: "create",
          assignee: snapshot.members[0] || snapshot.actor,
        };
      }
    }
    const rowsTop = snapshot.canManage ? 298 : 174;
    const row = Math.floor((y - rowsTop) / 112);
    const tasks = snapshot.tasks.slice(0, snapshot.canManage ? 6 : 7);
    const task = tasks[row];
    if (!task || y < rowsTop || y > rowsTop + row * 112 + 96) return null;
    let action = "";
    if (x >= 1110 && x <= 1370 && task.canStartStop) {
      action = task.status === "active" ? "stop" : "start";
    } else if (
      x >= 1405 &&
      x <= 1665 &&
      task.canComplete &&
      task.status !== "done"
    ) {
      action = "complete";
    } else if (x >= 1700 && x <= 1960 && task.canDelete) {
      action = "delete";
    }
    return action ? { action, id: task.id } : null;
  }

  function updateWorldBulletin(events = []) {
    const face = ctx.worldBulletin.getObjectByName("forkmesh-world-bulletin-face");
    if (!face?.material) return false;
    worldBulletinEvents = Array.isArray(events) ? events : [];
    worldBulletinOffset = clamp(
      worldBulletinOffset,
      0,
      Math.max(0, worldBulletinEvents.length - WORLD_BULLETIN_VISIBLE_EVENTS),
    );
    face.material.map?.dispose?.();
    face.material.map = worldBulletinTexture(
      THREE,
      worldBulletinEvents,
      worldBulletinOffset,
    );
    face.material.needsUpdate = true;
    return true;
  }

  function updateWorldGeneralChat(messages = []) {
    const previous = ctx.worldGeneralChatFace.material.map;
    ctx.worldGeneralChatFace.material.map = worldGeneralChatTexture(
      THREE,
      messages,
    );
    ctx.worldGeneralChatFace.material.needsUpdate = true;
    previous?.dispose?.();
    return true;
  }

  function scrollWorldBulletin(direction) {
    const nextOffset = clamp(
      worldBulletinOffset + direction,
      0,
      Math.max(0, worldBulletinEvents.length - WORLD_BULLETIN_VISIBLE_EVENTS),
    );
    if (nextOffset === worldBulletinOffset) return false;
    worldBulletinOffset = nextOffset;
    return updateWorldBulletin(worldBulletinEvents);
  }

  // Header art and avatars come from mastodon.social's media host. They only
  // reach the board texture when they load CORS-clean; otherwise the board
  // keeps its text-only rendering (a tainted canvas cannot feed WebGL).
  function mastodonKioskImage(url) {
    const key = canvasReadableImageURL(url);
    if (!key) return null;
    const cached = mastodonKioskImages.get(key);
    if (cached) return cached.image;
    const record = { image: null };
    mastodonKioskImages.set(key, record);
    const image = new Image();
    image.crossOrigin = "anonymous";
    image.onload = () => {
      if (ctx.disposed) return;
      record.image = image;
      repaintMastodonKiosk();
    };
    image.src = key;
    return null;
  }

  function setMastodonOpenButton(name, href) {
    const button = ctx.mastodonKiosk.getObjectByName(
      `forkmesh-mastodon-kiosk-open-${name}`,
    );
    if (!button) return;
    button.userData.href = href || "";
    button.visible = Boolean(href);
  }

  function repaintMastodonKiosk() {
    const face = ctx.mastodonKiosk.getObjectByName("forkmesh-mastodon-kiosk-face");
    if (!face?.material) return false;
    ctx.stripedUploads.swapMap(
      face.material,
      mastodonKioskTexture(
        THREE,
        mastodonKioskSnapshot,
        mastodonKioskOffset,
        mastodonKioskImage,
      ),
    );
    setMastodonOpenButton("profile", mastodonKioskSnapshot?.profileURL);
    const visibleToots = mastodonKioskVisibleToots(
      mastodonKioskSnapshot,
      mastodonKioskOffset,
    );
    setMastodonOpenButton("toot-0", visibleToots[0]?.url);
    setMastodonOpenButton("toot-1", visibleToots[1]?.url);
    setMastodonOpenButton("toot-2", visibleToots[2]?.url);
    return true;
  }

  function mastodonKioskMaxOffset() {
    return Math.max(
      0,
      (mastodonKioskSnapshot?.toots?.length || 0) -
        MASTODON_KIOSK_VISIBLE_TOOTS,
    );
  }

  function updateMastodonKiosk(snapshot = null) {
    mastodonKioskSnapshot =
      snapshot && typeof snapshot === "object"
        ? {
            ...snapshot,
            toots: Array.isArray(snapshot.toots)
              ? snapshot.toots.filter(Boolean)
              : [],
            replies: Array.isArray(snapshot.replies)
              ? snapshot.replies.filter(Boolean)
              : [],
          }
        : null;
    mastodonKioskOffset = clamp(
      mastodonKioskOffset,
      0,
      mastodonKioskMaxOffset(),
    );
    return repaintMastodonKiosk();
  }

  // The dial is its own small texture: a one-second countdown tick must not
  // rebuild the 1536×4096 board texture, and it repaints the existing dial
  // canvas in place rather than allocating a texture per tick.
  function repaintMastodonCountdown() {
    const dial = ctx.mastodonKiosk.getObjectByName(
      "forkmesh-mastodon-kiosk-countdown",
    );
    return repaintCanvasTexture(dial?.material, (context) =>
      drawMastodonCountdown(
        context,
        ctx.mastodonKioskCountdown.remainingMs,
        ctx.mastodonKioskCountdown.totalMs,
        ctx.mastodonKioskCountdown.loading,
      ),
    );
  }

  function repaintMastodonLastPost() {
    const plate = ctx.mastodonKiosk.getObjectByName(
      "forkmesh-mastodon-kiosk-lastpost",
    );
    return repaintCanvasTexture(plate?.material, (context) =>
      drawMastodonLastPost(context, mastodonKioskLastPostSinceMs),
    );
  }

  function updateMastodonCountdown({
    remainingMs = MASTODON_KIOSK_REFRESH_MS,
    totalMs = MASTODON_KIOSK_REFRESH_MS,
    loading = false,
    lastPostAgoMs = null,
  } = {}) {
    let repainted = false;
    // The last-post plate only changes on a minute boundary (or when a fresh
    // snapshot moves the newest toot), so it keys on whole minutes and skips
    // the second-by-second repaints the sync clock needs.
    const since =
      Number.isFinite(Number(lastPostAgoMs)) && Number(lastPostAgoMs) >= 0
        ? Number(lastPostAgoMs)
        : null;
    const sinceKey = since === null ? "-" : String(Math.floor(since / 60_000));
    if (sinceKey !== mastodonKioskLastPostKey) {
      mastodonKioskLastPostKey = sinceKey;
      mastodonKioskLastPostSinceMs = since;
      repainted = repaintMastodonLastPost() || repainted;
    }
    const total = Math.max(1000, Number(totalMs) || MASTODON_KIOSK_REFRESH_MS);
    const remaining = clamp(Number(remainingMs) || 0, 0, total);
    const key = `${loading ? 1 : 0}:${Math.ceil(remaining / 1000)}:${total}`;
    if (key === mastodonKioskCountdownKey) return repainted;
    mastodonKioskCountdownKey = key;
    ctx.mastodonKioskCountdown = {
      remainingMs: remaining,
      totalMs: total,
      loading: Boolean(loading),
    };
    return repaintMastodonCountdown() || repainted;
  }

  function scrollMastodonKiosk(direction) {
    const nextOffset = clamp(
      mastodonKioskOffset + direction,
      0,
      mastodonKioskMaxOffset(),
    );
    if (nextOffset === mastodonKioskOffset) return false;
    mastodonKioskOffset = nextOffset;
    return repaintMastodonKiosk();
  }

  function enterOfficeMeeting({
    roomName = "general",
    participants = [],
    participantId = "",
  } = {}) {
    // The meeting table stands on the Marketing floor, so joining a room is
    // another way onto that storey: it stays closed to everyone the server
    // did not put on the team.
    if (!canAccessOfficeFloor(ctx.officeFloorAccess, "marketing")) return false;
    enterOfficeLobby({ floorId: "marketing" });
    ctx.officeSceneMode = "meeting";
    ctx.officeCurrentFloorId = "marketing";
    fns.updateSceneLevelOfDetail(true);
    ctx.currentFloorY = officeFloorY("marketing") + 0.38;
    ctx.currentSpace = "office-marketing";
    ctx.officeLobbyPlayer.position.y = ctx.currentFloorY;
    ctx.officeElevatorCar.position.y = officeFloorY("marketing");
    ctx.officeLocalParticipantId = String(participantId || ctx.officeLocalParticipantId);
    setOfficeParticipants(participants);
    ctx.officeLobbyPlayer.visible =
      !ctx.officeLocalParticipantId ||
      !ctx.officeParticipants.has(ctx.officeLocalParticipantId);
    ctx.player.visible = false;
    return true;
  }

  function setOfficeBuildStamp(stamp = {}) {
    return ctx.landmarkObjects.get("office")?.userData.setOfficeBuildStamp?.(stamp) || "";
  }

  function setOfficeSeatState({ participantId, chairId = "", pose = "standing" } = {}) {
    const avatar = ctx.officeParticipants.get(String(participantId || ""));
    if (!avatar) return false;
    applyOfficeParticipantPose(avatar, {
      ...(avatar.userData.officePresence || {}),
      id: String(participantId),
      chairId: String(chairId || ""),
      pose: pose === "seated" ? "seated" : "standing",
    });
    return true;
  }

  function showOfficeBubble(participantId, bubble = {}) {
    const id = String(participantId || "");
    const avatar = ctx.officeParticipants.get(id);
    if (!avatar) return false;
    ctx.officeBubbles.get(id)?.remove();
    const element = document.createElement("div");
    element.className = "world-office-bubble";
    element.dataset.worldOfficeBubble = id;
    element.textContent = String(bubble.text || bubble.fileName || "Shared an attachment")
      .trim()
      .slice(0, 180);
    element.style.left = "50%";
    element.style.top = "76px";
    labelLayer.appendChild(element);
    ctx.officeBubbles.set(id, element);
    window.setTimeout(() => {
      if (ctx.officeBubbles.get(id) !== element) return;
      element.remove();
      ctx.officeBubbles.delete(id);
    }, 7000);
    return true;
  }

  return {
    enterOffice,
    enterOfficeLobby,
    applyOfficeChairSeatPose,
    standUpFromOfficeChair,
    sitOnOfficeChair,
    setOfficeParticipants,
    renderOfficeMarketingTasks,
    marketingInitiativeWallAction,
    updateOfficeMarketingTasks,
    marketingTaskWallAction,
    updateWorldBulletin,
    updateWorldGeneralChat,
    scrollWorldBulletin,
    mastodonKioskMaxOffset,
    updateMastodonKiosk,
    updateMastodonCountdown,
    scrollMastodonKiosk,
    enterOfficeMeeting,
    setOfficeBuildStamp,
    setOfficeSeatState,
    showOfficeBubble,
  };
}
