import { LANDMARKS, WORLD_REGIONS, landmarkById } from "../../world-data.js";
import { territoryWalkActive } from "../movement.js";
import {
  OFFICE_FRONT_Z,
  nextOfficeZoneState,
} from "../../world-office-tower.js";
import {
  BEACH_CENTER_X,
  BEACH_CENTER_Z,
  BEACH_RADIUS,
  BEACH_SCENE_PORTAL_X,
  BEACH_TOWN_PORTAL_X,
} from "../layout.js";

export function functions({
  THREE,
  onLocationChange,
  onMovement,
  onOfficeProximity,
  onRegionChange,
  onWorldPackageAccessRequest,
  ctx,
  fns,
}) {
  let currentRegion = "central";
  let repositoryRecordWheelLayer = null;
  let repositoryRecordWheelTargets = [];


  function nearestLandmark() {
    fns.updateRepositoryDistrictArrival();
    if (
      ![
        "town-square",
        "east",
        "central",
        "west",
      ].includes(ctx.currentSpace)
    ) {
      if (ctx.officeZoneState !== "distant") {
        ctx.officeZoneState = "distant";
        onOfficeProximity(ctx.officeZoneState);
      }
      return;
    }
    let nearest = null;
    let distance = Infinity;
    LANDMARKS.forEach((landmark) => {
      const next = Math.hypot(
        ctx.player.position.x - landmark.position[0],
        ctx.player.position.z - landmark.position[2],
      );
      if (next < distance) {
        nearest = landmark;
        distance = next;
      }
    });
    const office = landmarkById("office");
    // Admission is a doorway, not the column of air above it. Altitude counts
    // toward the entrance distance so a quadcopter hovering over the tower is
    // never "at the door" — that reading is what puts the shell's Enter the
    // Office control one click away from anywhere in the sky.
    const officeEntranceDistance = Math.hypot(
      ctx.player.position.x - office.position[0],
      ctx.player.position.y - ctx.currentFloorY,
      ctx.player.position.z - (office.position[2] + OFFICE_FRONT_Z),
    );
    const nextOfficeState = territoryWalkActive()
      ? "distant"
      : nextOfficeZoneState(ctx.officeZoneState, officeEntranceDistance);
    if (nextOfficeState !== ctx.officeZoneState) {
      ctx.officeZoneState = nextOfficeState;
      if (ctx.officeZoneState === "nearby") fns.loadOfficeTableLogoTexture();
      onOfficeProximity(ctx.officeZoneState);
    }
    let nearestRepository = null;
    let repositoryDistance = Infinity;
    ctx.repositoryPortals.forEach((record) => {
      const next = Math.hypot(
        ctx.player.position.x - record.group.position.x,
        ctx.player.position.z - record.group.position.z,
      );
      if (next < repositoryDistance) {
        nearestRepository = record;
        repositoryDistance = next;
      }
    });
    const nextLocation = repositoryDistance < 4.8
        ? `${nearestRepository.owner}/${nearestRepository.name}`
        : distance < 7.5
          ? nearest.label
          : "Town Square";
    const nextLocationId = repositoryDistance < 4.8
        ? "repositories"
        : distance < 7.5
          ? nearest?.id || ""
          : "";
    if (["campfire", "neighborhood"].includes(nextLocationId)) {
      fns.completeStartHereStep("people");
    } else if (nextLocationId === "fountain") {
      fns.completeStartHereStep("nodes");
    } else if (nextLocationId === "repositories") {
      fns.completeStartHereStep("repositories");
    } else if (nextLocationId === "office") {
      fns.completeStartHereStep("office");
    }
    if (
      Math.hypot(
        ctx.player.position.x - BEACH_CENTER_X,
        ctx.player.position.z - BEACH_CENTER_Z,
      ) < BEACH_RADIUS
    ) {
      fns.completeStartHereStep("explore");
    }
    if (nextLocation !== ctx.currentLocation) {
      ctx.currentLocation = nextLocation;
      onLocationChange(nextLocation, nextLocationId);
    }
    const nextRegion =
      ctx.player.position.x > 12
        ? "east"
        : ctx.player.position.x < -12
          ? "west"
          : "central";
    if (nextRegion !== currentRegion) {
      currentRegion = nextRegion;
      const region = WORLD_REGIONS.find((item) => item.id === currentRegion);
      onRegionChange(region || { id: currentRegion, label: currentRegion });
    }
  }

  function setPackageFloorY(value) {
    const floorY = Number(value);
    if (Number.isFinite(floorY)) ctx.currentFloorY = floorY;
  }

  // District packages move the visitor between their own spaces and Town.
  function setPackageSpace(value) {
    const space = String(value || "").trim();
    if (!space || space === ctx.currentSpace) return;
    const district = ctx.packageSpaces.get(space);
    if (!district && space !== "town-square") return;
    ctx.currentSpace = space;
    ctx.currentLocation = district ? district.label : "Town Square";
    onLocationChange(ctx.currentLocation, district ? district.packageId : "");
  }

  function requestPackageAccess(packageId) {
    if (ctx.packageAccessRequested.has(packageId)) return;
    ctx.packageAccessRequested.add(packageId);
    onWorldPackageAccessRequest(packageId);
  }

  // A district owns every interactive object whose type carries its id as a
  // prefix ("<package-id>-…"), so a click never reaches the wrong package.
  function interactWithDistrictPackage(object) {
    const type = String(object?.userData?.interactive || "");
    if (!type) return false;
    for (let index = 0; index < ctx.districtPackages.length; index += 1) {
      const record = ctx.districtPackages[index];
      if (!type.startsWith(`${record.packageId}-`)) continue;
      const handled =
        record.instance.interact?.(object, record.frame) === true;
      if (handled && object.userData?.worldSeat) fns.sitOnWorldSeat(object);
      return handled;
    }
    return false;
  }

  function updateRepositoryPortalLabels() {
    let nearestKey = "";
    let nearestDistance = Infinity;
    ctx.repositoryPortals.forEach((record, key) => {
      const distance = Math.hypot(
        ctx.player.position.x - record.group.position.x,
        ctx.player.position.z - record.group.position.z,
      );
      if (distance < nearestDistance) {
        nearestDistance = distance;
        nearestKey = key;
      }
    });
    ctx.repositoryPortals.forEach((record, key) => {
      const label = record.group.userData.repositoryLabel;
      if (!label) return;
      label.visible =
        key === ctx.focusedRepositoryKey ||
        record.group === ctx.world.userData.repositorySizeMount ||
        (key === nearestKey && nearestDistance < 9.5);
    });
  }

  function updateRepositoryRecordBoardProximity() {
    const layer = ctx.world.userData.repositoryRecordDeskLayer;
    if (!layer?.parent) {
      ctx.repositoryRecordBoardNearby = false;
      return;
    }
    layer.getWorldPosition(ctx.repositoryRecordLayerPosition);
    ctx.repositoryRecordBoardNearby =
      Math.hypot(
        ctx.player.position.x - ctx.repositoryRecordLayerPosition.x,
        ctx.player.position.z - ctx.repositoryRecordLayerPosition.z,
      ) < 40;
  }

  function repositoryRecordWheelTargetList() {
    const layer = ctx.world.userData.repositoryRecordDeskLayer;
    if (layer !== repositoryRecordWheelLayer) {
      repositoryRecordWheelLayer = layer;
      repositoryRecordWheelTargets = [];
      layer?.traverse((object) => {
        if (
          ["issue", "pull"].includes(
            String(object.userData?.repositoryRecordPageKind || ""),
          ) &&
          ctx.interactive.includes(object)
        ) {
          repositoryRecordWheelTargets.push(object);
        }
      });
    }
    return repositoryRecordWheelTargets;
  }

  function travelToRegion(regionId) {
    const destinations = {
      east: new THREE.Vector3(27, 0, 2),
      central: new THREE.Vector3(0, 0, 10),
      west: new THREE.Vector3(-27, 0, 2),
    };
    const destination = destinations[regionId];
    if (!destination) return false;
    ctx.currentSpace = regionId;
    ctx.currentFloorY = fns.outdoorFloorYAt(destination.x, destination.z);
    destination.y = ctx.currentFloorY;
    ctx.player.position.copy(destination);
    ctx.cameraFocus = null;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
    nearestLandmark();
    onMovement({
      x: destination.x,
      y: destination.y,
      z: destination.z,
      heading: ctx.player.rotation.y,
      activity: `arriving in ${regionId} campus`,
      space: regionId,
      moving: false,
    });
    return true;
  }

  // `relocate: false` is the zip line arriving mid-air: the rider keeps their
  // place on the cable instead of appearing at the portal.
  function setBeachScene(active, { relocate = true } = {}) {
    if (active === ctx.beachSceneActive || ctx.officeSceneMode !== "town") {
      return false;
    }
    if (active) {
      fns.dismountSwing({ relocate: false });
      fns.dismountBike({ relocate: false });
      fns.dismountQuadcopter({ relocate: false });
      if (ctx.jetpackEquipped) fns.setJetpackEquipped(false);
      fns.stopGymExercise();
    }
    fns.dismountCar({ relocate: false });
    fns.standUpFromBench();
    ctx.beachSceneActive = active;
    ctx.currentSpace = active ? "beach" : "town-square";
    ctx.currentLocation = active ? "ForkMesh Beach" : "Town Square";
    if (relocate) {
      const portalX = active
        ? BEACH_SCENE_PORTAL_X + 6
        : BEACH_TOWN_PORTAL_X - 6;
      ctx.currentFloorY = fns.outdoorFloorYAt(portalX, BEACH_CENTER_Z);
      ctx.player.position.set(portalX, ctx.currentFloorY, BEACH_CENTER_Z);
      ctx.player.rotation.y = active ? -Math.PI / 2 : Math.PI / 2;
      ctx.cameraSnapPending = true;
    } else {
      ctx.currentFloorY = fns.outdoorFloorYAt(ctx.player.position.x, ctx.player.position.z);
    }
    ctx.cameraFocus = null;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
    ctx.lastPosition.copy(ctx.player.position);
    fns.updateCompactDistrictResidency(true);
    fns.syncEnclosureSceneVisibility(true);
    if (active) fns.completeStartHereStep("explore");
    else nearestLandmark();
    onLocationChange(ctx.currentLocation, active ? "beach" : "");
    onMovement({
      x: ctx.player.position.x,
      y: ctx.player.position.y,
      z: ctx.player.position.z,
      heading: ctx.player.rotation.y,
      space: ctx.currentSpace,
      moving: false,
      activity: active
        ? "arriving at the ForkMesh beach"
        : "returning to the Town Square",
    });
    return true;
  }

  function focusLandmark(id) {
    const landmark = landmarkById(id);
    const object = ctx.landmarkObjects.get(landmark.id);
    if (!object) return;
    ctx.currentSpace = "town-square";
    ctx.currentFloorY = fns.outdoorFloorYAt(ctx.player.position.x, ctx.player.position.z);
    ctx.player.position.y = ctx.currentFloorY;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
    if (landmark.id === "office") {
      // Office selection is travel, not a detached spectator camera. Dash to
      // the real exterior threshold and keep the chase camera attached.
      const doorway = ctx.officeInterior.localToWorld(
        new THREE.Vector3(0, 0, OFFICE_FRONT_Z + 3.2),
      );
      ctx.selectedLandmark = "office";
      ctx.cameraFocus = null;
      ctx.dashTarget = new THREE.Vector3(
        doorway.x,
        ctx.currentFloorY,
        doorway.z,
      );
      return;
    }
    ctx.cameraFocus = new THREE.Vector3(
      landmark.position[0],
      1.5,
      landmark.position[2],
    );
  }

  return {
    nearestLandmark,
    setPackageFloorY,
    setPackageSpace,
    requestPackageAccess,
    interactWithDistrictPackage,
    updateRepositoryPortalLabels,
    updateRepositoryRecordBoardProximity,
    repositoryRecordWheelTargetList,
    travelToRegion,
    setBeachScene,
    focusLandmark,
  };
}
