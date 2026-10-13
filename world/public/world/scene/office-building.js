import {
  OFFICE_DISTANT_DRAW_BUDGET,
  OFFICE_DOOR_WIDTH,
  OFFICE_ELEVATOR_CENTER_X,
  OFFICE_FLOOR_COUNT,
  OFFICE_FLOOR_HEIGHT,
  OFFICE_FRONT_Z,
  OFFICE_HEX_APOTHEM,
  OFFICE_HEX_CORNERS,
  OFFICE_HEX_RADIUS,
  OFFICE_HEX_SIDES,
  officeFloorY,
} from "../world-office-tower.js";
import {
  canvasArtTexture,
  canvasTexture,
  repaintCanvasTexture,
  roundedRect,
} from "./canvas.js";
import {
  OFFICE_BUILD_STAMP_HIDDEN_RETRY_MS,
  OFFICE_BUILD_STAMP_REFRESH_MS,
  OFFICE_DOOR_HEIGHT,
  OFFICE_ELEVATOR_CUT_MARGIN,
  OFFICE_ELEVATOR_HALF_WIDTH,
  OFFICE_HEIGHT,
  OFFICE_ROOF_BARRIER_TOP,
} from "./gameplay-constants.js";
import { OFFICE_EXTERIOR_LOD_DISTANCE } from "./layout.js";
import {
  cloneSharedMesh,
  cloneSharedPlane,
  makeMaterial,
  makeSharedMaterial,
} from "./materials.js";
import { mirrorCommitAgeLabel } from "./mirror-nodes.js";
import { setShadows } from "./object-utils.js";
import { WEATHER_PARTICLE_COUNT } from "./quality.js";
import * as resources from "./resources.js";

export function officeReclaimedWoodTexture(THREE) {
  const texture = canvasArtTexture(THREE, 1024, 1024, (context) => {
    context.fillStyle = "#68452f";
    context.fillRect(0, 0, 1024, 1024);
    const plankColors = ["#6e4931", "#82563a", "#5c3b2b", "#76503a"];
    for (let plank = 0; plank < 10; plank += 1) {
      const y = plank * 103;
      context.fillStyle = plankColors[plank % plankColors.length];
      context.fillRect(0, y, 1024, 100);
      context.strokeStyle = "rgba(35,18,10,0.72)";
      context.lineWidth = 5;
      context.beginPath();
      context.moveTo(0, y + 100);
      context.lineTo(1024, y + 100);
      context.stroke();
      for (let grain = 0; grain < 16; grain += 1) {
        const offset = (grain * 79 + plank * 37) % 1024;
        context.strokeStyle =
          grain % 3 ? "rgba(35,18,10,0.24)" : "rgba(220,157,98,0.18)";
        context.lineWidth = 2 + (grain % 2);
        context.beginPath();
        context.moveTo(offset - 180, y + 18 + (grain % 5) * 14);
        context.bezierCurveTo(
          offset,
          y + 4 + (grain % 4) * 17,
          offset + 140,
          y + 82 - (grain % 3) * 11,
          offset + 310,
          y + 40 + (grain % 4) * 13,
        );
        context.stroke();
      }
    }
    // Chrome ForkMesh cube medallion: the branch graph is the cube's top face,
    // with the F/M side reliefs retained from the supplied mark.
    context.save();
    context.translate(512, 512);
    context.fillStyle = "#071018";
    context.strokeStyle = "#dce8f7";
    context.lineWidth = 15;
    context.beginPath();
    context.moveTo(0, -210);
    context.lineTo(210, -90);
    context.lineTo(210, 140);
    context.lineTo(0, 260);
    context.lineTo(-210, 140);
    context.lineTo(-210, -90);
    context.closePath();
    context.fill();
    context.stroke();
    context.beginPath();
    context.moveTo(-210, -90);
    context.lineTo(0, 32);
    context.lineTo(210, -90);
    context.moveTo(0, 32);
    context.lineTo(0, 260);
    context.stroke();
    context.fillStyle = "#f1f6ff";
    context.font = '900 138px "ForkMesh Favorit", system-ui, sans-serif';
    context.textAlign = "center";
    context.fillText("F", -102, 110);
    context.fillText("M", 105, 112);
    context.strokeStyle = "#c7e8ff";
    context.lineWidth = 18;
    context.lineCap = "round";
    context.beginPath();
    context.moveTo(0, -152);
    context.lineTo(0, -30);
    context.moveTo(0, -92);
    context.lineTo(-94, -38);
    context.moveTo(0, -92);
    context.lineTo(94, -38);
    context.stroke();
    for (const [x, y] of [[0, -152], [-94, -38], [94, -38], [0, -30]]) {
      context.beginPath();
      context.arc(x, y, 21, 0, Math.PI * 2);
      context.fillStyle = "#f5fbff";
      context.fill();
    }
    context.restore();
  });
  texture.colorSpace = THREE.SRGBColorSpace;
  return texture;
}

function officeGuideBoardTexture(THREE) {
  return canvasTexture(THREE, 1024, 704, (context) => {
    context.fillStyle = "#071b15";
    context.fillRect(0, 0, 1024, 704);
    context.strokeStyle = "#78e9b0";
    context.lineWidth = 12;
    context.strokeRect(10, 10, 1004, 684);
    context.fillStyle = "#d9ffea";
    context.font = '800 62px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("OFFICE GUIDE", 58, 96);
    context.fillStyle = "#9bd1b5";
    context.font = '600 33px "ForkMesh Mono", ui-monospace, monospace';
    [
      "1  ENTER THROUGH THE FRONT DOOR",
      "2  CHOOSE #GENERAL ON THIS WALL",
      "3  CLICK A CHAIR TO TAKE A SEAT",
      "4  USE THE TASK BOARD FOR ORG WORK",
    ].forEach((line, index) => context.fillText(line, 58, 196 + index * 92));
    context.fillStyle = "#f7d58a";
    context.font = '700 28px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("MEETINGS AND TASKS STAY INSIDE THE OFFICE.", 58, 600);
  });
}

export function officeDoorStatusTexture(THREE, state = "open") {
  const normalized =
    state === "syncing" ? "syncing" : state === "ready" ? "ready" : "open";
  const label =
    normalized === "syncing"
      ? "SYNCING ACCESS"
      : normalized === "ready"
        ? "ACCESS READY"
        : "";
  const accent = normalized === "syncing" ? "#77d9ff" : "#9ef7c6";
  return canvasTexture(THREE, 768, 120, (context) => {
    context.clearRect(0, 0, 768, 120);
    roundedRect(context, 3, 3, 762, 114, 20);
    context.fillStyle = "rgba(5,20,16,0.92)";
    context.fill();
    context.strokeStyle = accent;
    context.lineWidth = 6;
    context.stroke();
    context.fillStyle = accent;
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '800 42px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(label, 384, 61, 690);
    if (normalized === "syncing") {
      context.textAlign = "left";
      for (let index = 0; index < 3; index += 1) {
        context.beginPath();
        context.arc(665 + index * 22, 61, 6, 0, Math.PI * 2);
        context.fill();
      }
    }
  });
}

function officeBuildStampLabel(stamp, now = Date.now()) {
  const version = String(stamp?.version || "").trim();
  const revision = String(stamp?.revision || "").trim().slice(0, 12);
  const builtAt = Number(stamp?.builtAt) || 0;
  const parts = [];
  if (version) parts.push(`${/^v/i.test(version) ? "" : "v"}${version}`);
  if (revision) parts.push(revision);
  if (builtAt > 0) {
    parts.push(`updated ${mirrorCommitAgeLabel(Math.max(0, now - builtAt))}`);
  }
  return parts.length ? parts.join(" · ") : "build pending";
}

function paintOfficeBuildStamp(context, label) {
  context.fillStyle = "#0b1713";
  context.fillRect(0, 0, 1640, 160);
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.font = '800 72px "ForkMesh Mono", ui-monospace, monospace';
  context.fillStyle = "#9ef7c6";
  context.fillText(String(label || ""), 820, 84, 1580);
}

function officeBuildStampTexture(THREE, label) {
  return canvasTexture(THREE, 1640, 160, (context) =>
    paintOfficeBuildStamp(context, label));
}

function objectShown(object) {
  for (let node = object; node; node = node.parent) {
    if (!node.visible) return false;
  }
  return true;
}

export function createForkMeshOffice(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  group.name = "forkmesh-office-destination";
  const wallThickness = 0.35;
  const doorWidth = OFFICE_DOOR_WIDTH;
  const doorHeight = OFFICE_DOOR_HEIGHT;
  const rooftopY = officeFloorY("rooftop");
  const concrete = makeMaterial(THREE, "#18231f", {
    metalness: 0.08,
    roughness: 0.88,
  });
  const structure = makeMaterial(THREE, "#163f32", {
    metalness: 0.48,
    roughness: 0.42,
  });
  const facade = makeMaterial(THREE, "#9ef7c6", {
    transparent: true,
    opacity: 0.24,
    metalness: 0,
    roughness: 0.62,
    depthWrite: false,
  });
  facade.name = "forkmesh-office-clear-glass";
  const doorMaterial = makeMaterial(THREE, "#c9fff3", {
    transparent: true,
    opacity: 0.3,
    metalness: 0,
    roughness: 0.38,
    depthWrite: false,
  });
  const elevatorBayMinX =
    OFFICE_ELEVATOR_CENTER_X -
    OFFICE_ELEVATOR_HALF_WIDTH -
    OFFICE_ELEVATOR_CUT_MARGIN;
  const elevatorBayMaxX =
    OFFICE_ELEVATOR_CENTER_X +
    OFFICE_ELEVATOR_HALF_WIDTH +
    OFFICE_ELEVATOR_CUT_MARGIN;
  const halfSide = OFFICE_HEX_RADIUS / 2;
  const addSideSegment = (
    side,
    minU,
    maxU,
    height,
    depth,
    y,
    inset,
    material,
  ) => {
    const width = maxU - minU;
    if (width <= 0) return null;
    const mesh = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", width, height, depth),
      material,
    );
    const along = (minU + maxU) / 2;
    const reach = OFFICE_HEX_APOTHEM - inset;
    mesh.position.set(
      side.normalX * reach + side.tangentX * along,
      y,
      side.normalZ * reach + side.tangentZ * along,
    );
    mesh.rotation.y = side.rotationY;
    group.add(mesh);
    return mesh;
  };
  const sideSpans = (side, gaps) => {
    const spans = [];
    let start = -halfSide;
    for (const [minU, maxU] of gaps) {
      spans.push([start, minU]);
      start = maxU;
    }
    spans.push([start, halfSide]);
    return spans.filter(([minU, maxU]) => maxU > minU);
  };
  const elevatorGap = [[elevatorBayMinX, elevatorBayMaxX]];
  const doorGap = [[-doorWidth / 2, doorWidth / 2]];
  const frontSide = OFFICE_HEX_SIDES.find((side) => side.id === "front");
  for (const side of OFFICE_HEX_SIDES) {
    if (side === frontSide) continue;
    const gaps = side.id === "back" ? elevatorGap : [];
    for (const [minU, maxU] of sideSpans(side, gaps)) {
      addSideSegment(
        side,
        minU,
        maxU,
        rooftopY,
        wallThickness,
        rooftopY / 2,
        wallThickness / 2,
        facade,
      );
    }
  }

  for (const [minU, maxU] of sideSpans(frontSide, doorGap)) {
    addSideSegment(
      frontSide,
      minU,
      maxU,
      0.62,
      wallThickness,
      0.31,
      wallThickness / 2,
      concrete,
    );
  }
  addSideSegment(
    frontSide,
    -halfSide,
    halfSide,
    1.18,
    wallThickness,
    OFFICE_HEIGHT - 0.59,
    wallThickness / 2,
    concrete,
  );
  for (const [minU, maxU] of [
    [-halfSide + 0.6, -doorWidth / 2 - 0.55],
    [doorWidth / 2 + 0.55, halfSide - 0.6],
  ]) {
    addSideSegment(
      frontSide,
      minU,
      maxU,
      OFFICE_HEIGHT - 1.34,
      0.12,
      OFFICE_HEIGHT / 2,
      0.03,
      facade,
    );
  }
  for (const x of [-doorWidth / 2 - 0.28, doorWidth / 2 + 0.28]) {
    const mullion = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.34, OFFICE_HEIGHT - 0.62, 0.42),
      structure,
    );
    mullion.position.set(x, OFFICE_HEIGHT / 2, OFFICE_FRONT_Z - wallThickness / 2);
    group.add(mullion);
  }

  for (let level = 1; level < OFFICE_FLOOR_COUNT; level += 1) {
    const baseY = level * OFFICE_FLOOR_HEIGHT;
    if (level < OFFICE_FLOOR_COUNT - 1) {
      addSideSegment(
        frontSide,
        -halfSide + 0.6,
        halfSide - 0.6,
        OFFICE_FLOOR_HEIGHT - 0.72,
        0.14,
        baseY + OFFICE_FLOOR_HEIGHT / 2,
        0.03,
        facade,
      );
    }
    const floorBandMaterial = makeSharedMaterial(
      THREE,
      level === OFFICE_FLOOR_COUNT - 1 ? "#a8e7ff" : "#72efba",
      {
        emissive: level === OFFICE_FLOOR_COUNT - 1
          ? "#3e88ad"
          : "#1d8f69",
        emissiveIntensity: 0.75,
        metalness: 0.38,
        roughness: 0.28,
      },
    );
    for (const side of OFFICE_HEX_SIDES) {
      const gaps = side.id === "back" ? elevatorGap : [];
      for (const [minU, maxU] of sideSpans(side, gaps)) {
        addSideSegment(
          side,
          minU,
          maxU,
          0.16,
          0.5,
          baseY + 0.1,
          -0.08,
          floorBandMaterial,
        );
      }
    }
  }
  const facadeColumnTopY = rooftopY + OFFICE_ROOF_BARRIER_TOP;
  for (const corner of OFFICE_HEX_CORNERS) {
    const facadeColumn = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.62, facadeColumnTopY, 0.62),
      structure,
    );
    facadeColumn.position.set(corner.x, facadeColumnTopY / 2, corner.z);
    facadeColumn.rotation.y = Math.atan2(corner.x, corner.z);
    group.add(facadeColumn);
  }
  // Floor plaques are intentionally hidden outside the Office so the tower facade
  // appears without floor labels.

  const signTexture = canvasTexture(THREE, 1024, 192, (context) => {
    context.fillStyle = "#0b1713";
    context.fillRect(0, 0, 1024, 192);
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 10;
    context.strokeRect(8, 8, 1008, 176);
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '800 72px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#d9ffea";
    context.fillText("FORKMESH OFFICE", 512, 96);
  });
  const sign = cloneSharedPlane(
    THREE,
    8.2,
    1.35,
    resources.uniqueMaterial(THREE, {
      map: signTexture,
      transparent: false,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  sign.scale.set(2.25, 2.25, 1);
  sign.position.set(0, OFFICE_HEIGHT - 3, OFFICE_FRONT_Z + 0.24);
  group.add(sign);

  const buildStampState = {
    version: "",
    revision: "",
    builtAt: 0,
    label: "",
    nextRefreshAt: 0,
  };
  const buildStamp = cloneSharedPlane(
    THREE,
    8.2,
    0.8,
    resources.uniqueMaterial(THREE, {
      map: officeBuildStampTexture(THREE, "build pending"),
      transparent: false,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  buildStamp.name = "forkmesh-office-build-stamp";
  buildStamp.scale.set(2.25, 2.25, 1);
  buildStamp.position.set(
    0,
    OFFICE_HEIGHT - 3 - (1.35 + 0.8) * 2.25 / 2 - 0.2,
    OFFICE_FRONT_Z + 0.24,
  );
  group.add(buildStamp);
  const refreshBuildStamp = (now = Date.now()) => {
    const label = officeBuildStampLabel(buildStampState, now);
    if (label === buildStampState.label) return true;
    if (!objectShown(buildStamp)) return false;
    buildStampState.label = label;
    if (repaintCanvasTexture(buildStamp.material, (context) =>
      paintOfficeBuildStamp(context, label))) return true;
    const previous = buildStamp.material.map;
    buildStamp.material.map = officeBuildStampTexture(THREE, label);
    buildStamp.material.needsUpdate = true;
    previous?.dispose?.();
    return true;
  };
  animated.push((time) => {
    if (time < buildStampState.nextRefreshAt) return;
    buildStampState.nextRefreshAt = time + (
      refreshBuildStamp()
        ? OFFICE_BUILD_STAMP_REFRESH_MS
        : OFFICE_BUILD_STAMP_HIDDEN_RETRY_MS
    );
  });

  const slidingDoors = new THREE.Group();
  slidingDoors.name = "forkmesh-office-sliding-doors";
  slidingDoors.position.z = OFFICE_FRONT_Z + 0.03;
  const doorPanelWidth = doorWidth / 2 - 0.08;
  const doorClosedX = doorWidth / 4;
  const doorOpenX = doorWidth / 2 + doorPanelWidth / 2 + 0.18;
  const doorPanels = [-1, 1].map((side) => {
    const panel = cloneSharedMesh(
      THREE,
      "office-sliding-door-panel",
      () => resources.primitive(THREE, "BoxGeometry", doorPanelWidth, doorHeight, 0.18),
      doorMaterial,
    );
    panel.name =
      `forkmesh-office-sliding-door-${side < 0 ? "left" : "right"}`;
    panel.position.set(
      side * doorClosedX,
      doorHeight / 2,
      0,
    );
    slidingDoors.add(panel);
    return panel;
  });
  group.add(slidingDoors);

  // A status panel rides on the glass but never participates in collision.
  // Access hydration is intentionally background-only; this makes that
  // network activity visible without turning the doorway into a loading gate.
  const doorStatus = cloneSharedPlane(
    THREE,
    4.6,
    0.72,
    resources.uniqueMaterial(THREE, {
      map: officeDoorStatusTexture(THREE, "open"),
      transparent: true,
      toneMapped: false,
      depthWrite: false,
    }, "MeshBasicMaterial"),
  );
  doorStatus.name = "forkmesh-office-door-status";
  doorStatus.position.set(0, 3.25, OFFICE_FRONT_Z + 0.17);
  doorStatus.renderOrder = 4;
  doorStatus.userData.officeDoorStatus = "open";
  doorStatus.visible = false;
  group.add(doorStatus);

  const high = new THREE.Group();
  high.name = "forkmesh-office-high-fidelity";
  high.add(...group.children);
  const highLevel = new THREE.Group();
  highLevel.name = "forkmesh-office-high-level";
  highLevel.add(high);
  const low = new THREE.Group();
  low.name = "forkmesh-office-low-poly";
  low.visible = false;

  const lod = new THREE.LOD();
  lod.name = "forkmesh-office-distance-lod";
  lod.autoUpdate = false;
  lod.addLevel(highLevel, 0);
  lod.addLevel(low, OFFICE_EXTERIOR_LOD_DISTANCE);
  group.add(lod);
  group.position.set(...position);
  group.userData.landmark = "office";
  group.userData.officeDistanceLod = lod;
  group.userData.officeHighFidelity = high;
  group.userData.officeLowPoly = low;
  group.userData.officeDistantDrawCount = 0;
  group.userData.officeDistantDrawBudget = OFFICE_DISTANT_DRAW_BUDGET;
  group.userData.officeSlidingDoors = slidingDoors;
  group.userData.officeDoorPanels = doorPanels;
  group.userData.officeDoorClosedX = doorClosedX;
  group.userData.officeDoorOpenX = doorOpenX;
  group.userData.officeDoorStatus = doorStatus;
  group.userData.officeBuildStamp = buildStamp;
  group.userData.setOfficeBuildStamp = (stamp = {}) => {
    buildStampState.version = String(stamp?.version || "");
    buildStampState.revision = String(stamp?.revision || "");
    buildStampState.builtAt = Math.max(0, Number(stamp?.builtAt) || 0);
    if (!refreshBuildStamp()) buildStampState.nextRefreshAt = 0;
    return officeBuildStampLabel(buildStampState);
  };
  high.traverse((child) => {
    if (!child.isMesh) return;
    child.userData.landmark = "office";
    interactive.push(child);
  });
  setShadows(high);
  // Clear glazing and door panes do not cast or receive opaque shadows.
  high.traverse((child) => {
    if (!child.isMesh || !child.material?.transparent) return;
    child.castShadow = false;
    child.receiveShadow = false;
  });
  sign.castShadow = false;
  buildStamp.castShadow = false;
  doorStatus.castShadow = false;
  doorStatus.receiveShadow = false;
  return group;
}

// A rain or snow preset is a local effect drawn by one 700-point field, and
// the town is WORLD_RADIUS across. The field therefore travels: every frame it
// is wrapped back around the visitor (see followWeather) so the chosen preset
// falls wherever they stand instead of only within half a field of the map
// origin, which is what made the environment appear to change at the centre of
// the map and nowhere else.
// The traveling field has the capacity of the strongest device tier. Lower
// tiers draw (and now follow/integrate) only their active prefix.
export const WEATHER_FIELD_POINTS = WEATHER_PARTICLE_COUNT;
export const WEATHER_FIELD_SPAN = 80;
export const WEATHER_FIELD_HEIGHT = 28;
