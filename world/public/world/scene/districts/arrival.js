import { LANDMARKS } from "../../world-data.js";
import {
  OFFICE_BRIDGE_WIDTH,
  OFFICE_FRONT_Z,
  OFFICE_ISLAND_CENTER,
} from "../../world-office-tower.js";
import { makeArrivalPlaque, makeGroundPlaque } from "../arrival.js";
import { MEMBER_ISLAND_CENTER_Z, WORLD_PATH_WIDTH } from "../layout.js";
import { makeSharedMaterial } from "../materials.js";
import { setShadows } from "../object-utils.js";
import { createForkMeshOffice } from "../office-building.js";
import { createRepositoryDistrict } from "../repositories.js";
import { createFountain } from "../sol-fountain.js";
import {
  START_HERE_STEPS,
  concreteBrickMaterial,
  createLandscapePath,
  startHereMapTexture,
} from "../town.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
}) {
  function completeStartHereStep(stepId) {
    if (!stepId || ctx.startHereCompleted.has(stepId)) return false;
    ctx.startHereCompleted.add(stepId);
    try {
      localStorage.setItem(
        ctx.startHereStorageKey,
        JSON.stringify([...ctx.startHereCompleted].slice(0, START_HERE_STEPS.length)),
      );
    } catch (_) {}
    const previous = ctx.startHereBoardFace.material.map;
    ctx.startHereBoardFace.material.map = startHereMapTexture(
      THREE,
      ctx.startHereCompleted,
    );
    ctx.startHereBoardFace.material.needsUpdate = true;
    previous?.dispose?.();
    return true;
  }

  return {
    completeStartHereStep,
  };
}

export async function build({
  THREE,
  animated,
  interactive,
  landmarkObjects,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  // Registered members and their named campfire benches have a dedicated
  // southern garden. It is the fourth cardinal district, leaving the live
  // mirror cabinets and reward pool unobstructed in the central plaza.
  // The south route used to be two overlapping slabs: the town path stopped
  // at z=88 while a second promenade began at z=78. Besides the visible color
  // seam, their different heights caused the large doubled rectangle seen
  // from above. createTownLandscape now owns one continuous path beneath the
  // circle dirt. The former entrance sign crowded that route, so the clearing
  // and its much larger member total now provide the wayfinding on their own.

  // The first object encountered when walking from the Members Circle toward
  // the centered nodes is a persistent orientation board. Only bounded step
  // identifiers are stored; no movement coordinates or visit history leave
  // the device.
  const startHereStorageKey = "forkmesh.world.start-here.v1";
  let savedStartHereSteps = [];
  try {
    savedStartHereSteps = JSON.parse(
      localStorage.getItem(startHereStorageKey) || "[]",
    );
  } catch (_) {}
  const validStartHereStepIds = new Set(
    START_HERE_STEPS.map((step) => step.id),
  );
  const startHereCompleted = new Set([
    "start",
    ...(Array.isArray(savedStartHereSteps)
      ? savedStartHereSteps.filter((step) => validStartHereStepIds.has(step))
      : []),
  ]);
  const startHereBoard = new THREE.Group();
  startHereBoard.name = "forkmesh-start-here-map";
  // Keep the large orientation map on the far side of even the maximum-sized
  // bench ring, facing back toward the fire and arriving visitors.
  startHereBoard.position.set(0, 0, MEMBER_ISLAND_CENTER_Z + 38);
  startHereBoard.rotation.y = Math.PI;
  const startHereBacking = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 16.4, 10.5, 0.34),
    makeSharedMaterial(THREE, "#161b22", {
      metalness: 0.18,
      roughness: 0.58,
    }),
  );
  startHereBacking.position.y = 6.2;
  startHereBoard.add(startHereBacking);
  const startHereBoardFace = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 16, 10),
    resources.uniqueMaterial(THREE, {
      map: startHereMapTexture(THREE, startHereCompleted),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  startHereBoardFace.name = "forkmesh-start-here-map-face";
  startHereBoardFace.position.set(0, 6.2, 0.19);
  startHereBoardFace.userData.interactive = "start-here-map";
  startHereBoard.add(startHereBoardFace);
  interactive.push(startHereBoardFace);
  for (const x of [-7.2, 7.2]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.34, 6.2, 0.34),
      makeSharedMaterial(THREE, "#30363d", {
        metalness: 0.35,
        roughness: 0.48,
      }),
    );
    post.position.set(x, 3.1, 0);
    startHereBoard.add(post);
  }
  setShadows(startHereBoard);
  world.add(startHereBoard);
  registerWorldElement(
    "start-here-board", "Start Here board", "Boards & kiosks", startHereBoard,
  );
  await sceneSlice();

  const officeEntranceZ =
    OFFICE_ISLAND_CENTER[2] + OFFICE_FRONT_Z;
  const officeBridge = new THREE.Group();
  officeBridge.name = "forkmesh-office-bridge";
  officeBridge.add(
    createLandscapePath(
      THREE,
      "forkmesh-town-path-north-office",
      [0, -OFFICE_BRIDGE_WIDTH / 2],
      [OFFICE_ISLAND_CENTER[0], officeEntranceZ],
      WORLD_PATH_WIDTH,
      concreteBrickMaterial(THREE),
    ),
  );
  world.add(officeBridge);
  registerWorldElement(
    "causeways", "Causeways & bridges", "Terrain", officeBridge,
  );
  await sceneSlice();

  // The room still assigns one of 64 ephemeral slots, but the grid itself is
  // no longer drawn: the plaques at the front edge carry the arrival story and
  // the lawn reads as open ground.
  const arrivalBox = new THREE.Group();
  arrivalBox.name = "world-arrival-box";
  const arrivalPlaque = makeArrivalPlaque(THREE);
  // Just past the grid's front edge (cells end at z ≈ 17.4), facing inward
  // toward the Town Square center as visitors leave the welcome grid.
  arrivalPlaque.position.set(0, 0, 15.2);
  arrivalPlaque.rotation.y = Math.PI;
  setShadows(arrivalPlaque);
  arrivalBox.add(arrivalPlaque);
  const songPlaque = makeGroundPlaque(
    THREE,
    "WATCH FORKMESH SONG",
    "CLICK TO OPEN ON YOUTUBE",
    "#9ef7c6",
  );
  songPlaque.name = "forkmesh-song-plaque";
  songPlaque.position.set(4.8, 0, 15.4);
  songPlaque.rotation.y = Math.PI;
  songPlaque.traverse((child) => {
    if (!child.isMesh) return;
    child.userData.openForkmeshSong = true;
    interactive.push(child);
  });
  arrivalBox.add(songPlaque);
  world.add(arrivalBox);
  registerWorldElement(
    "arrival-box", "Arrival stats box", "Boards & kiosks", arrivalBox,
  );
  await sceneSlice();

  const landmarkFactories = {
    fountain: createFountain,
    repositories: createRepositoryDistrict,
    office: createForkMeshOffice,
  };
  LANDMARKS.forEach((landmark) => {
    // The campfire owns a map spot but no district factory: its group and
    // bench circle are built below and registered under their own layout id.
    const factory = landmarkFactories[landmark.id];
    if (!factory) return;
    const object = factory(
      THREE,
      landmark.position,
      interactive,
      animated,
    );
    landmarkObjects.set(landmark.id, object);
    world.add(object);
    registerWorldElement(
      `landmark-${landmark.id}`,
      landmark.label,
      "Districts",
      object,
    );
    // Repository portals and their import kiosk form one structural district.
    // Keep its shared origin fixed so a stale saved landmark transform cannot
    // split the kiosk and apron away from the live portal ring.
  });
  return {
    startHereStorageKey,
    startHereCompleted,
    startHereBoard,
    startHereBoardFace,
    officeBridge,
    arrivalBox,
    arrivalPlaque,
  };
}
