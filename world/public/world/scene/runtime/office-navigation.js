import {
  OFFICE_AVATAR_RADIUS,
  OFFICE_DOOR_WIDTH,
  OFFICE_HEX_APOTHEM,
  OFFICE_HEX_SIDES,
  OFFICE_ISLAND_CENTER,
  canAccessOfficeFloor,
  normalizeOfficeFloorAccess,
  officeElevatorCabinContains,
  officeElevatorShellContains,
  officeFloorById,
  officeFloorY,
  officeHexExtent,
  officePointHitsObstacle,
  officePointOutsideObstacles,
  officeTowerShellExit,
} from "../../world-office-tower.js";
import { normalizeSelfWorkBoard, setAvatarWorkBadge } from "../avatars.js";
import {
  BIKE_RIDE_SPEED_MULTIPLIER,
  CAR_RIDE_SPEED_MULTIPLIER,
  OFFICE_DOORWAY_APPROACH_Z,
  OFFICE_DOORWAY_ENTRY_Z,
  OFFICE_DOORWAY_INTERIOR_JOIN_Z,
  OFFICE_DOORWAY_PASSAGE_MIN_Z,
  OFFICE_FLOOR_WARP_DOOR_WIDTH,
  OFFICE_FLOOR_WARP_TRIGGER_Z,
  OFFICE_INTERIOR_EXIT_Z,
  OFFICE_INTERIOR_WALL_LIMIT,
  PLAYER_MAX_SPEED,
  PLAYER_SPEED,
  PLAYER_SPEED_GRADIENT_DOWN_TIME,
  PLAYER_SPEED_GRADIENT_UP_TIME,
  PLAYER_SPRINT_MULTIPLIER,
} from "../gameplay-constants.js";
import {
  REPOSITORY_GROUND_RADIUS,
  REPOSITORY_ISLAND_CENTER_X,
} from "../layout.js";
import { officeScenePointIsWalkable } from "../movement.js";
import { clamp } from "../object-utils.js";

export function functions({
  THREE,
  onOfficeElevatorSound,
  onOfficeEnter,
  onRepositoryDistrictEnter,
  ctx,
  fns,
}) {
  // The repository district loads nothing until a character stands on its
  // ground circle. The scene reports that arrival once; the shell owns every
  // request that follows.
  let repositoryDistrictEntered = false;
  // Plain reusable points for the per-frame Office shell test. The helper is
  // pure arithmetic on tower-local coordinates and never needs a Vector3.
  const officeShellPoint = { x: 0, y: 0, z: 0 };
  const officeShellPreviousPoint = { x: 0, y: 0, z: 0 };
  let keyboardMovementRamp = 0;
  let officeAttendance = { visits: [], leaderboard: [] };
  let officeAttendanceRenderedBucket = 0;


  function baseMoveSpeed() {
    return PLAYER_SPEED * ctx.moveSpeedScale;
  }

  function movementSpeedForInput(input = {}, delta = 0) {
    const sprintScale =
      (input.sprinting ? PLAYER_SPRINT_MULTIPLIER : 1) *
      (input.ridingBike ? BIKE_RIDE_SPEED_MULTIPLIER : 1) *
      (input.ridingCar ? CAR_RIDE_SPEED_MULTIPLIER : 1);
    const topSpeed = PLAYER_MAX_SPEED * ctx.moveSpeedScale * sprintScale;
    const inputStrength = Math.max(0, Number(input.inputStrength) || 0);
    const touchStrength = Math.max(0, Number(input.touchStrength) || 0);
    const rawDelta = Math.max(0, Number(delta) || 0);

    if (input.keyboardActive) {
      const rampRate = 1 / PLAYER_SPEED_GRADIENT_UP_TIME;
      const targetRamp = 1;
      const blendStep = rampRate * rawDelta;
      keyboardMovementRamp = clamp(
        keyboardMovementRamp +
          (targetRamp - keyboardMovementRamp) * Math.min(1, blendStep),
        0,
        1,
      );
      const baseSpeed = baseMoveSpeed() * sprintScale;
      ctx.keyboardMovementSpeed = baseSpeed + (topSpeed - baseSpeed) * keyboardMovementRamp;
      return ctx.keyboardMovementSpeed;
    }

    if (touchStrength > 0) {
      keyboardMovementRamp = 0;
      ctx.keyboardMovementSpeed = topSpeed * inputStrength;
      return ctx.keyboardMovementSpeed;
    }

    const rampRate = 1 / PLAYER_SPEED_GRADIENT_DOWN_TIME;
    const blendStep = rampRate * rawDelta;
    keyboardMovementRamp = clamp(
      keyboardMovementRamp * Math.max(0, 1 - blendStep),
      0,
      1,
    );
    ctx.keyboardMovementSpeed = baseMoveSpeed() * sprintScale;
    return ctx.keyboardMovementSpeed;
  }

  function officeAvatarLocalPosition(
    avatar,
    target = new THREE.Vector3(),
  ) {
    if (!avatar) return target.set(0, 0, 0);
    avatar.getWorldPosition(target);
    ctx.officeInterior.worldToLocal(target);
    return target;
  }

  function applyOfficeAvatarLocalPosition(avatar, localPosition) {
    if (!avatar || !localPosition) return;
    const worldPosition = ctx.officeInterior.localToWorld(localPosition.clone());
    avatar.parent?.worldToLocal?.(worldPosition);
    avatar.position.copy(worldPosition);
  }

  function nearestOfficeWalkablePosition(floorId, localPosition) {
    const origin = localPosition.clone();
    if (
      officeScenePointIsWalkable(
        floorId,
        origin.x,
        origin.z,
        OFFICE_AVATAR_RADIUS,
      )
    ) {
      return origin;
    }
    // Meeting poses cluster around the Marketing table, which is intentionally
    // collidable in the free-walking floor. Search outward in small rings so
    // leaving a room resumes at the closest valid point instead of trapping
    // the continuous player inside that table.
    for (let distance = 0.75; distance <= 24; distance += 0.75) {
      for (let index = 0; index < 24; index += 1) {
        const angle = (index / 24) * Math.PI * 2;
        const x = origin.x + Math.cos(angle) * distance;
        const z = origin.z + Math.sin(angle) * distance;
        if (
          officeScenePointIsWalkable(
            floorId,
            x,
            z,
            OFFICE_AVATAR_RADIUS,
          )
        ) {
          origin.x = x;
          origin.z = z;
          return origin;
        }
      }
    }
    origin.x = 0;
    origin.z = 24;
    return origin;
  }

  // A roof jump is how a visitor leaves the tower, so the interior wall
  // constraint is deliberately not applied to it: the arc has to cross the
  // parapet. Solid furniture is a different matter. Without this, a jumper who
  // held a movement key sailed straight through a round patio table and landed
  // standing inside it, where every later frame rejected both the attempted and
  // the previous position and left them stuck in the wood. Jumping over a table
  // still works — a table stops counting once the jumper's feet clear its top.
  function clearOfficeFurniture(avatar) {
    if (!avatar) return false;
    const position = officeAvatarLocalPosition(avatar);
    const feetAboveFloor =
      position.y - officeFloorY(ctx.officeCurrentFloorId);
    if (
      !officePointHitsObstacle(
        ctx.officeCurrentFloorId,
        position.x,
        position.z,
        OFFICE_AVATAR_RADIUS,
        feetAboveFloor,
      )
    ) {
      return false;
    }
    const clear = officePointOutsideObstacles(
      ctx.officeCurrentFloorId,
      position.x,
      position.z,
      OFFICE_AVATAR_RADIUS,
      feetAboveFloor,
    );
    position.x = clear.x;
    position.z = clear.z;
    applyOfficeAvatarLocalPosition(avatar, position);
    return true;
  }

  function setMovementTuning(tuning = {}) {
    if (tuning.speed !== undefined) {
      const numeric = Number(tuning.speed);
      ctx.moveSpeedScale = Number.isFinite(numeric) && numeric > 0 ? numeric : 1;
    }
    // Keep the live speed within the new ceiling so a lowered cap takes effect
    // immediately rather than only after the player stops and restarts.
    ctx.keyboardMovementSpeed = Math.min(
      ctx.keyboardMovementSpeed,
      PLAYER_MAX_SPEED * ctx.moveSpeedScale,
    );
    return { speed: ctx.moveSpeedScale };
  }

  function cancelDash() {
    ctx.dashTarget = null;
  }

  function setOfficeExitHandler(handler) {
    ctx.officeExitHandler = typeof handler === "function" ? handler : null;
  }

  function setOfficeDoorwayEntryPending(pending = false) {
    ctx.officeDoorwayEntryPending = pending === true;
    return ctx.officeDoorwayEntryPending;
  }

  function setOfficeFloorHandler(handler) {
    ctx.officeFloorHandler = typeof handler === "function" ? handler : null;
  }

  function refreshOfficeFloorWarpDoors() {
    ctx.officeFloorWarpDoors.forEach((doorway) => {
      const allowed = canAccessOfficeFloor(
        ctx.officeFloorAccess,
        doorway.floorId,
      );
      doorway.allowed = allowed;
      doorway.portal.visible = allowed;
      doorway.openLabel.visible = allowed;
      doorway.closedDoor.visible = !allowed;
      doorway.lockedLabel.visible = !allowed;
      const frameMaterial = allowed
        ? doorway.frameMaterials.allowed
        : doorway.frameMaterials.locked;
      doorway.frames.forEach((frame) => {
        frame.material = frameMaterial;
      });
    });
  }

  function setOfficeAccess(payload = {}) {
    ctx.officeFloorAccess = normalizeOfficeFloorAccess(payload);
    // The original elevator keeps its team grants. District packages read
    // only what they need (the organization role) from the same response.
    ctx.lastOfficeAccessPayload =
      payload && typeof payload === "object" ? payload : {};
    for (let index = 0; index < ctx.districtPackages.length; index += 1) {
      ctx.districtPackages[index].instance.setAccess?.(ctx.lastOfficeAccessPayload);
    }
    ctx.officeElevatorButtons.forEach((button) => {
      const allowed = canAccessOfficeFloor(
        ctx.officeFloorAccess,
        button.userData.officeFloorId,
      );
      button.userData.officeFloorAllowed = allowed;
      button.material.color.set(allowed ? "#2d8063" : "#3b3034");
      button.material.emissive?.set?.(allowed ? "#1f9c6a" : "#6e2737");
      button.material.emissiveIntensity = allowed ? 0.9 : 0.2;
    });
    refreshOfficeFloorWarpDoors();
    return { ...ctx.officeFloorAccess };
  }

  function setSelfWorkBoard(board = {}) {
    ctx.selfWorkBoard = normalizeSelfWorkBoard(board);
    [
      ctx.player,
      ctx.officeLobbyPlayer,
      ctx.officeParticipants.get(ctx.officeLocalParticipantId),
    ].forEach((avatar) => {
      if (avatar) {
        setAvatarWorkBadge(THREE, avatar, ctx.selfWorkBoard, ctx.selfWorkBadgeVisible);
      }
    });
    return { ...ctx.selfWorkBoard };
  }

  function setSelfWorkBadgeVisibility(visible = true) {
    const previous = ctx.selfWorkBadgeVisible;
    ctx.selfWorkBadgeVisible = visible === true;
    [
      ctx.player,
      ctx.officeLobbyPlayer,
      ctx.officeParticipants.get(ctx.officeLocalParticipantId),
    ].forEach((avatar) => {
      if (avatar?.userData?.selfWorkBadge) {
        avatar.userData.selfWorkBadge.visible = ctx.selfWorkBadgeVisible;
      }
    });
    return previous;
  }

  function setOfficeAttendance(event = {}) {
    const receivedAt = performance.now();
    const rawAsOfAt = Number(event?.asOfAt);
    const asOfAt =
      Number.isFinite(rawAsOfAt) && rawAsOfAt > 0
        ? rawAsOfAt
        : Date.now();
    const visits = Array.isArray(event?.visits)
      ? event.visits.slice(0, 20).map((visit) => {
          const inAt = Math.max(0, Number(visit?.inAt) || 0);
          const rawOutAt = Math.max(0, Number(visit?.outAt) || 0);
          const outAt = inAt > 0 && rawOutAt >= inAt ? rawOutAt : 0;
          const suppliedDuration = Number(visit?.durationMs);
          const hasSuppliedDuration =
            visit?.durationMs !== null &&
            visit?.durationMs !== undefined &&
            Number.isFinite(suppliedDuration) &&
            suppliedDuration >= 0;
          const durationMs =
            inAt <= 0
              ? null
              : hasSuppliedDuration
                ? suppliedDuration
                : outAt
                  ? outAt - inAt
                  : Math.max(0, asOfAt - inAt);
          return {
            id: String(visit?.id || "").slice(0, 32),
            account: String(visit?.account || "Contributor")
              .replace(/\s+/g, " ")
              .trim()
              .slice(0, 32),
            inAt,
            outAt,
            durationMs,
            floor: String(visit?.floor || "")
              .replace(/\s+/g, " ")
              .trim()
              .slice(0, 32),
          };
        })
      : [];
    const leaderboard = Array.isArray(event?.leaderboard)
      ? event.leaderboard.slice(0, 20).map((member) => ({
          account: String(member?.account || "Contributor")
            .replace(/\s+/g, " ")
            .trim()
            .slice(0, 32),
          longestDurationMs: Math.max(
            0,
            Number(member?.longestDurationMs) || 0,
          ),
          activeDurationMs: Math.max(
            0,
            Number(member?.activeDurationMs) || 0,
          ),
          present: member?.present === true,
          floor: String(member?.floor || "")
            .replace(/\s+/g, " ")
            .trim()
            .slice(0, 32),
        }))
      : [];
    officeAttendance = { visits, leaderboard, asOfAt };
    ctx.officeAttendanceObservedAt = receivedAt;
    officeAttendanceRenderedBucket = 0;
    const previousLeaderboard = ctx.officeAttendanceBoard.material.map;
    const previousClock = ctx.officeClockInBoard.material.map;
    ctx.officeAttendanceBoard.material.map =
      fns.officeAttendanceTexture(officeAttendance);
    ctx.officeAttendanceBoard.material.needsUpdate = true;
    ctx.officeClockInBoard.material.map =
      fns.officeClockInTexture(officeAttendance);
    ctx.officeClockInBoard.material.needsUpdate = true;
    previousLeaderboard?.dispose?.();
    previousClock?.dispose?.();
    return {
      visits: officeAttendance.visits.map((visit) => ({ ...visit })),
      leaderboard: officeAttendance.leaderboard.map(
        (member) => ({ ...member }),
      ),
    };
  }

  function updateOfficeAttendanceClock(time) {
    if (
      ctx.officeSceneMode !== "lobby" ||
      !(
        officeAttendance.leaderboard.some((member) => member.present) ||
        officeAttendance.visits.some(
          (visit) => visit.inAt > 0 && !visit.outAt,
        )
      )
    ) {
      return;
    }
    const elapsedMs = Math.max(0, time - ctx.officeAttendanceObservedAt);
    const bucket = Math.floor(elapsedMs / 30_000);
    if (bucket === officeAttendanceRenderedBucket) return;
    officeAttendanceRenderedBucket = bucket;
    const previousLeaderboard = ctx.officeAttendanceBoard.material.map;
    const previousClock = ctx.officeClockInBoard.material.map;
    ctx.officeAttendanceBoard.material.map =
      fns.officeAttendanceTexture(officeAttendance, elapsedMs);
    ctx.officeAttendanceBoard.material.needsUpdate = true;
    ctx.officeClockInBoard.material.map =
      fns.officeClockInTexture(officeAttendance, elapsedMs);
    ctx.officeClockInBoard.material.needsUpdate = true;
    previousLeaderboard?.dispose?.();
    previousClock?.dispose?.();
  }

  function travelToOfficeFloor(floorId) {
    const floor = officeFloorById(floorId);
    const avatar = ctx.player;
    const localPosition = officeAvatarLocalPosition(avatar);
    if (
      ctx.officeSceneMode === "town" ||
      ctx.officeSceneMode === "meeting" ||
      ctx.officeElevatorRide ||
      !floor ||
      !canAccessOfficeFloor(ctx.officeFloorAccess, floor.id) ||
      !officeElevatorCabinContains(
        localPosition.x,
        localPosition.z,
        OFFICE_AVATAR_RADIUS,
      )
    ) {
      return false;
    }
    if (floor.id === ctx.officeCurrentFloorId) return true;
    ctx.officeElevatorRide = {
      fromY: avatar.position.y,
      toY: officeFloorY(floor.id) + 0.38,
      floorId: floor.id,
      startedAt: performance.now(),
      // The doubled-height tower is a view, not a teleport. Give riders enough
      // time to watch Town and the sky move through the panoramic front glass.
      duration: 1400 + Math.abs(floor.level -
        (officeFloorById(ctx.officeCurrentFloorId)?.level || 0)) * 360,
    };
    fns.lockOfficeElevatorCamera();
    cancelDash();
    ctx.jumpQueued = false;
    ctx.jumpVelocity = 0;
    onOfficeElevatorSound("depart", {
      from: ctx.officeCurrentFloorId,
      to: floor.id,
      duration: ctx.officeElevatorRide.duration,
    });
    return true;
  }

  function warpToOfficeFloor(floorId) {
    const floor = officeFloorById(floorId);
    if (
      ctx.officeSceneMode !== "lobby" ||
      ctx.officeElevatorRide ||
      !floor ||
      !canAccessOfficeFloor(ctx.officeFloorAccess, floor.id)
    ) {
      return false;
    }
    fns.standUpFromOfficeChair();
    cancelDash();
    ctx.officeCurrentFloorId = floor.id;
    // A direct doorway warp does not necessarily cross a camera LOD boundary.
    // Render its selected floor before the next movement frame and camera snap.
    fns.syncOfficeFloorVisibility();
    ctx.currentSpace = `office-${floor.id}`;
    ctx.currentFloorY = officeFloorY(floor.id) + 0.38;
    const destination = new THREE.Vector3(
      -10,
      ctx.currentFloorY,
      -26,
    );
    applyOfficeAvatarLocalPosition(ctx.player, destination);
    ctx.player.rotation.y = Math.PI;
    ctx.officeElevatorCar.position.y = officeFloorY(floor.id);
    fns.renderOfficeMarketingTasks();
    ctx.cameraSnapPending = true;
    ctx.lastPosition.copy(ctx.player.position);
    onOfficeElevatorSound("arrive", {
      floor: floor.id,
      duration: 0,
      warp: true,
    });
    return true;
  }

  function tryOfficeFloorWarpDoorway(avatar, previousPosition) {
    if (
      !avatar ||
      !previousPosition ||
      ctx.officeSceneMode !== "lobby" ||
      ctx.officeCurrentFloorId !== "lobby" ||
      ctx.officeElevatorRide
    ) {
      return false;
    }
    const current = officeAvatarLocalPosition(avatar, ctx.officeWarpCurrentScratch);
    const previous = ctx.officeWarpPreviousScratch.copy(previousPosition);
    avatar.parent?.localToWorld?.(previous);
    ctx.officeInterior.worldToLocal(previous);
    if (
      current.z > OFFICE_FLOOR_WARP_TRIGGER_Z ||
      previous.z <= OFFICE_FLOOR_WARP_TRIGGER_Z ||
      current.z >= previous.z - 1e-5
    ) {
      return false;
    }
    const doorway = ctx.officeFloorWarpDoors.find(
      (candidate) =>
        Math.abs(current.x - candidate.x) <=
        OFFICE_FLOOR_WARP_DOOR_WIDTH / 2 - OFFICE_AVATAR_RADIUS,
    );
    if (
      !doorway ||
      !doorway.allowed ||
      doorway.floorId === ctx.officeCurrentFloorId
    ) {
      return false;
    }
    return warpToOfficeFloor(doorway.floorId);
  }

  // Every flying pose leaves walkPlayer before constrainTownOfficeWalls runs,
  // so the tower shell is enforced here instead. Without it a quadcopter — or a
  // jetpack, or a super jump — carried a visitor straight through the curtain
  // wall and parked them inside Engineering or Infrastructure, floors the
  // elevator refuses to open for them. Returns the face that stopped the
  // flight, or "" when the tower was nowhere near it.
  function constrainFlightToOfficeShell(position, previousPosition, radius) {
    if (ctx.officeSceneMode !== "town" || !position) return "";
    const office = ctx.landmarkObjects.get("office");
    if (!office) return "";
    officeShellPoint.x = position.x - office.position.x;
    officeShellPoint.y = position.y - office.position.y;
    officeShellPoint.z = position.z - office.position.z;
    const previous = previousPosition || position;
    officeShellPreviousPoint.x = previous.x - office.position.x;
    officeShellPreviousPoint.y = previous.y - office.position.y;
    officeShellPreviousPoint.z = previous.z - office.position.z;
    const exit = officeTowerShellExit(
      officeShellPreviousPoint,
      officeShellPoint,
      radius,
    );
    if (!exit) return "";
    position.set(
      exit.x + office.position.x,
      exit.y + office.position.y,
      exit.z + office.position.z,
    );
    return exit.face;
  }

  // Half of the panoramic cabin's glass envelope stands on the campus, past the
  // rectangular tower footprint the outdoor walker collides against. Without
  // the same three panes out here a visitor walks straight through them and
  // ends up inside the car in town mode: no office entry ever ran, so the floor
  // panel is inert and its authorization colours were never applied. The cab is
  // reached the way every other floor is, through the lobby.
  function constrainTownElevatorShell(previousPosition) {
    const office = ctx.landmarkObjects.get("office");
    if (!office) return false;
    const blocked = (x, z) =>
      officeElevatorShellContains(
        x - office.position.x,
        z - office.position.z,
        OFFICE_AVATAR_RADIUS,
      );
    if (!blocked(ctx.player.position.x, ctx.player.position.z)) return false;
    // Only a crossing that starts on clear ground is refused. A pose that
    // already sits inside the panes — a rooftop jump above the shaft, a stale
    // restored position — keeps its freedom to walk back out.
    if (blocked(previousPosition.x, previousPosition.z)) return false;
    if (!blocked(ctx.player.position.x, previousPosition.z)) {
      ctx.player.position.z = previousPosition.z;
    } else if (!blocked(previousPosition.x, ctx.player.position.z)) {
      ctx.player.position.x = previousPosition.x;
    } else {
      ctx.player.position.x = previousPosition.x;
      ctx.player.position.z = previousPosition.z;
    }
    cancelDash();
    return true;
  }

  function constrainTownOfficeWalls(previousPosition) {
    // On an instance the Office drawn beside it is ForkMesh's live one, so
    // its walls and doorway work there too.
    if (ctx.officeSceneMode !== "town") return false;
    const office = ctx.landmarkObjects.get("office");
    if (!office) return false;
    const localX = ctx.player.position.x - office.position.x;
    const localZ = ctx.player.position.z - office.position.z;
    const previousZ = previousPosition.z - office.position.z;
    const doorClearance = OFFICE_DOOR_WIDTH / 2 - OFFICE_AVATAR_RADIUS;
    const movingInward = localZ < previousZ - 1e-5;
    const inDoorwayPassage =
      Math.abs(localX) <= doorClearance &&
      localZ <= OFFICE_DOORWAY_APPROACH_Z &&
      localZ >= OFFICE_DOORWAY_PASSAGE_MIN_Z;
    const readyToCommitEntry =
      inDoorwayPassage &&
      localZ <= OFFICE_DOORWAY_ENTRY_Z &&
      movingInward;
    if (readyToCommitEntry && !ctx.officeDoorwayEntryPending) {
      ctx.officeDoorwayEntryPending = true;
      onOfficeEnter({
        source: "doorway",
      });
    }
    if (ctx.officeSceneMode !== "town") {
      // The lobby now owns this same position and velocity immediately after
      // the full avatar clears the jamb, with no collision or threshold pause.
      return false;
    }

    if (constrainTownElevatorShell(previousPosition)) return true;

    const reach = OFFICE_HEX_APOTHEM + OFFICE_AVATAR_RADIUS;
    if (officeHexExtent(localX, localZ) >= reach) {
      return false;
    }

    const previousX = previousPosition.x - office.position.x;
    // The doorway is a real open tunnel, not a collider with a callback carved
    // into it. Movement continues on every frame while the synchronous mode
    // handoff occurs; a transient rejection simply retries farther inside.
    if (inDoorwayPassage) {
      return false;
    }

    const [exit] = OFFICE_HEX_SIDES.map((side) => ({
      side,
      outside:
        side.normalX * previousX + side.normalZ * previousZ - reach,
    })).sort((left, right) => right.outside - left.outside);
    const depth =
      reach - (exit.side.normalX * localX + exit.side.normalZ * localZ);
    const pushedX = localX + exit.side.normalX * depth;
    const pushedZ = localZ + exit.side.normalZ * depth;
    ctx.player.position.x = office.position.x + pushedX;
    ctx.player.position.z = office.position.z + pushedZ;
    cancelDash();
    return true;
  }

  function constrainOfficeInteriorWalls(avatar, previousPosition) {
    if (!avatar) return false;
    const position = officeAvatarLocalPosition(
      avatar,
      ctx.officeWallPositionScratch,
    );
    const previous = ctx.officeWallPreviousScratch.copy(
      previousPosition || position,
    );
    if (avatar === ctx.player && previousPosition) {
      previous.set(
        previousPosition.x - OFFICE_ISLAND_CENTER[0],
        previousPosition.y - OFFICE_ISLAND_CENTER[1],
        previousPosition.z - OFFICE_ISLAND_CENTER[2],
      );
    }
    const commitPosition = () => {
      applyOfficeAvatarLocalPosition(avatar, position);
    };
    const doorClearance = OFFICE_DOOR_WIDTH / 2 - OFFICE_AVATAR_RADIUS;
    if (
      ctx.officeCurrentFloorId === "lobby" &&
      position.z > OFFICE_INTERIOR_WALL_LIMIT &&
      Math.abs(position.x) > doorClearance &&
      !officeElevatorCabinContains(
        position.x,
        position.z,
        OFFICE_AVATAR_RADIUS,
      )
    ) {
      position.z = OFFICE_INTERIOR_WALL_LIMIT;
      commitPosition();
    }
    const movingOutward = position.z > previous.z + 1e-5;
    if (
      ctx.officeCurrentFloorId === "lobby" &&
      movingOutward &&
      position.z >= OFFICE_INTERIOR_EXIT_Z &&
      Math.abs(position.x) <= doorClearance
    ) {
      if (!ctx.officeExitPending && ctx.officeExitHandler) {
        ctx.officeExitPending = true;
        ctx.officeExitHandler();
      }
      if (ctx.officeSceneMode !== "lobby") {
        // The exit handoff completed. The town walker inherits this exact
        // doorway pose and velocity instead of being teleported outward.
        return false;
      }
      // A missing/rejected controller must not let the avatar walk through
      // the tower shell.
      position.z = OFFICE_INTERIOR_EXIT_Z;
      commitPosition();
      return true;
    }
    // Everyone on a floor stands 0.38 above its slab; only a rooftop jump is
    // ever higher. Furniture the jumper has cleared is not in their way.
    const feetAboveFloor = Math.max(
      0,
      position.y - officeFloorY(ctx.officeCurrentFloorId),
    );
    const walkableAt = (x, z) => {
      const inLobbyDoorway =
        ctx.officeCurrentFloorId === "lobby" &&
        Math.abs(x) <= doorClearance &&
        z >= OFFICE_DOORWAY_INTERIOR_JOIN_Z &&
        z <= OFFICE_DOORWAY_APPROACH_Z + 0.08;
      return (
        inLobbyDoorway ||
        officeScenePointIsWalkable(
          ctx.officeCurrentFloorId,
          x,
          z,
          OFFICE_AVATAR_RADIUS,
          feetAboveFloor,
        )
      );
    };
    if (!walkableAt(position.x, position.z)) {
      const attemptedX = position.x;
      const attemptedZ = position.z;
      if (walkableAt(attemptedX, previous.z)) {
        position.z = previous.z;
      } else if (walkableAt(previous.x, attemptedZ)) {
        position.x = previous.x;
      } else {
        position.x = previous.x;
        position.z = previous.z;
      }
      commitPosition();
      cancelDash();
    }
    return false;
  }

  // The repository catalog, its imports, and every hosted size map are the
  // heaviest reads in the World, and most visits never walk east at all. None
  // of them are requested until a character is actually standing on the
  // repositories ground circle, which is what this reports — exactly once.
  function updateRepositoryDistrictArrival() {
    if (repositoryDistrictEntered) return;
    const distance = Math.hypot(
      ctx.player.position.x - REPOSITORY_ISLAND_CENTER_X,
      ctx.player.position.z,
    );
    if (distance > REPOSITORY_GROUND_RADIUS) return;
    repositoryDistrictEntered = true;
    onRepositoryDistrictEnter();
  }

  // Called by the shell when it begins the deferred catalog read, whether the
  // trigger was the ground circle or the repositories panel. Portals built
  // after this point rise and shimmer instead of popping into the ring.
  function beginRepositoryDistrictReveal() {
    repositoryDistrictEntered = true;
    ctx.repositoryRevealPending = true;
    return true;
  }

  return {
    baseMoveSpeed,
    movementSpeedForInput,
    officeAvatarLocalPosition,
    applyOfficeAvatarLocalPosition,
    nearestOfficeWalkablePosition,
    clearOfficeFurniture,
    setMovementTuning,
    cancelDash,
    setOfficeExitHandler,
    setOfficeDoorwayEntryPending,
    setOfficeFloorHandler,
    setOfficeAccess,
    setSelfWorkBoard,
    setSelfWorkBadgeVisibility,
    setOfficeAttendance,
    updateOfficeAttendanceClock,
    travelToOfficeFloor,
    warpToOfficeFloor,
    tryOfficeFloorWarpDoorway,
    constrainFlightToOfficeShell,
    constrainTownOfficeWalls,
    constrainOfficeInteriorWalls,
    updateRepositoryDistrictArrival,
    beginRepositoryDistrictReveal,
  };
}
