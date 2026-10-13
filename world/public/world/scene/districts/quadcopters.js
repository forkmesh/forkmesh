import { createInstancedMirrorSet } from "../../world-resource-cache.js";
import { mergeObjectBatches } from "../batching.js";
import {
  DRONE_COURSE_UNITS_PER_RADIAN,
  coasterDronePoint,
} from "../coaster.js";
import {
  DRONE_COURSE_COARSE_DISTANCE,
  DRONE_COURSE_DETAIL_ENTER_DISTANCE,
  DRONE_COURSE_RING_COUNT,
  DRONE_COURSE_RING_RADIUS,
  DRONE_RACE_ARROWS_PER_TARGET,
  DRONE_RACE_ARROW_SPACING,
  DRONE_RACE_ARROW_TILT,
  DRONE_RACE_GATE_HALF_SIZE,
  DRONE_RACE_GATE_RADIUS,
} from "../gameplay-constants.js";
import { makeLabelSprite } from "../labels.js";
import { WORLD_HEX_TILES } from "../layout.js";
import { cloneSharedMesh, makeMaterial } from "../materials.js";
import { setShadows } from "../object-utils.js";
import { material, primitive } from "../resources.js";

export function droneCoursePoint(angle) {
  const point = coasterDronePoint(angle);
  return [point.x, point.y, point.z];
}

export function droneCourseTangent(angle) {
  const step = 0.0005;
  const after = coasterDronePoint(angle + step);
  const before = coasterDronePoint(angle - step);
  const x = after.x - before.x;
  const y = after.y - before.y;
  const z = after.z - before.z;
  const length = Math.hypot(x, y, z);
  return [x / length, y / length, z / length];
}

function droneCourseGate(index, angle, radius) {
  return {
    index,
    angle,
    radius,
    center: droneCoursePoint(angle),
    normal: droneCourseTangent(angle),
  };
}

export function droneCourseGates() {
  const step = (Math.PI * 2) / DRONE_COURSE_RING_COUNT;
  return {
    rings: Array.from(
      { length: DRONE_COURSE_RING_COUNT },
      (_, index) => droneCourseGate(index, index * step, DRONE_COURSE_RING_RADIUS),
    ),
    start: droneCourseGate(-1, -step / 2, DRONE_RACE_GATE_RADIUS),
  };
}

export function droneRaceArrowAngles() {
  const { rings, start } = droneCourseGates();
  const spacing = DRONE_RACE_ARROW_SPACING / DRONE_COURSE_UNITS_PER_RADIAN;
  const angles = [];
  for (const target of [...rings, start]) {
    for (let step = 0; step < DRONE_RACE_ARROWS_PER_TARGET; step += 1) {
      angles.push(target.angle - (DRONE_RACE_ARROWS_PER_TARGET - step) * spacing);
    }
  }
  return angles;
}

export function functions({
  THREE,
  ctx,
  fns,
}) {
  function createWorldQuadcopterPrototype() {
    const airframe = new THREE.Group();
    airframe.name = "forkmesh-world-quadcopter-prototype";
    const frame = new THREE.Group();
    const frameMaterial = makeMaterial(THREE, "#1f6feb", {
      metalness: 0.62,
      roughness: 0.28,
    });
    const darkMaterial = makeMaterial(THREE, "#161b22", {
      metalness: 0.42,
      roughness: 0.5,
    });
    const lightMaterial = makeMaterial(THREE, "#79c0ff", {
      emissive: "#1f6feb",
      emissiveIntensity: 1.15,
      roughness: 0.22,
    });
    const body = cloneSharedMesh(
      THREE,
      "quadcopter-body",
      () => new THREE.CylinderGeometry(1.25, 1.45, 0.58, 12),
      frameMaterial,
    );
    body.position.y = 0.52;
    frame.add(body);
    const seat = cloneSharedMesh(
      THREE,
      "quadcopter-seat",
      () => new THREE.BoxGeometry(0.82, 0.18, 0.92),
      darkMaterial,
    );
    seat.position.set(0, 0.94, 0);
    frame.add(seat);
    for (const [rotorIndex, [x, z]] of [
      [-2.25, -2.25],
      [2.25, -2.25],
      [-2.25, 2.25],
      [2.25, 2.25],
    ].entries()) {
      const arm = cloneSharedMesh(
        THREE,
        "quadcopter-arm",
        () => new THREE.BoxGeometry(0.18, 0.14, 3.05),
        frameMaterial,
      );
      arm.position.set(x * 0.5, 0.62, z * 0.5);
      arm.rotation.y = Math.atan2(x, z);
      frame.add(arm);
      const motor = cloneSharedMesh(
        THREE,
        "quadcopter-motor",
        () => new THREE.CylinderGeometry(0.32, 0.38, 0.34, 10),
        darkMaterial,
      );
      motor.position.set(x, 0.72, z);
      frame.add(motor);
      const rotor = cloneSharedMesh(
        THREE,
        "quadcopter-rotor",
        () => new THREE.BoxGeometry(3.1, 0.045, 0.16),
        lightMaterial,
      );
      rotor.name = `forkmesh-world-quadcopter-rotor-${rotorIndex + 1}`;
      rotor.position.set(x, 0.96, z);
      rotor.userData.quadcopterRotor = true;
      airframe.add(rotor);
    }
    const landingLight = cloneSharedMesh(
      THREE,
      "quadcopter-landing-light",
      () => new THREE.SphereGeometry(0.22, 10, 7),
      lightMaterial,
    );
    landingLight.position.set(0, 0.34, -1.32);
    frame.add(landingLight);
    airframe.add(
      ...mergeObjectBatches(THREE, frame, {
        name: "forkmesh-world-quadcopter-frame",
      }).children,
    );
    const seatMarker = new THREE.Object3D();
    seatMarker.name = "forkmesh-world-quadcopter-seat";
    seatMarker.position.set(0, 0.94, 0);
    airframe.add(seatMarker);
    const hint = makeLabelSprite(
      THREE,
      "FLY QUADCOPTER",
      "CLICK OR E · WASD · SPACE UP · C DOWN",
      "#79c0ff",
    );
    hint.name = "forkmesh-world-quadcopter-control-hint";
    hint.position.set(0, 4.45, 0);
    hint.scale.set(7.2, 2.4, 1);
    airframe.add(hint);
    return airframe;
  }

  function addWorldQuadcopter(index, position) {
    const quadcopter = ctx.quadcopterPrototype.clone(true);
    quadcopter.name = `forkmesh-world-quadcopter-${index + 1}`;
    quadcopter.position.set(...position);
    const rotors = [];
    quadcopter.traverse((child) => {
      if (!child.isMesh) return;
      child.userData.quadcopterIndex = index;
      if (child.userData.quadcopterRotor) rotors.push(child);
      ctx.interactive.push(child);
    });
    const seat = quadcopter.getObjectByName("forkmesh-world-quadcopter-seat");
    const hint = quadcopter.getObjectByName(
      "forkmesh-world-quadcopter-control-hint",
    );
    setShadows(quadcopter);
    rotors.forEach((rotor) => ctx.quadcopterRotorMirror.add(rotor));
    ctx.world.add(quadcopter);
    fns.registerWorldElement(
      "quadcopter", "Quadcopters", "Vehicles & rides", quadcopter,
    );
    ctx.quadcopterStates.push({
      quadcopter,
      rotors,
      seat,
      hint,
      moving: false,
    });
  }

  function createDroneRaceCourse() {
    const course = new THREE.LOD();
    course.name = "forkmesh-drone-race-course";
    const [town] = WORLD_HEX_TILES;
    course.position.set(town.x, 0, town.z);
    const colors = [
      "#79c0ff", "#9ef7c6", "#f7c96b", "#ff8ab6", "#c7a0ff",
    ];
    const ringGates = droneCourseGates().rings;
    const ringCenters = ringGates.map(
      ({ center }) =>
        new THREE.Vector3(center[0] - town.x, center[1], center[2] - town.z),
    );
    const buildRingBatch = (geometry, name, opacity) => {
      const material = makeMaterial(THREE, "#ffffff", {
        emissive: "#1f6feb",
        emissiveIntensity: 1.35,
        metalness: 0.24,
        roughness: 0.3,
        transparent: opacity < 1,
        opacity,
        depthWrite: opacity >= 1,
      });
      const rings = new THREE.InstancedMesh(
        geometry,
        material,
        DRONE_COURSE_RING_COUNT,
      );
      rings.name = name;
      rings.castShadow = false;
      rings.receiveShadow = false;
      const transform = new THREE.Object3D();
      const normal = new THREE.Vector3(0, 0, 1);
      const tangent = new THREE.Vector3();
      for (let index = 0; index < DRONE_COURSE_RING_COUNT; index += 1) {
        transform.position.copy(ringCenters[index]);
        tangent.set(...ringGates[index].normal);
        transform.quaternion.setFromUnitVectors(normal, tangent);
        transform.scale.set(1, 1, 1);
        transform.updateMatrix();
        rings.setMatrixAt(index, transform.matrix);
        rings.setColorAt(index, new THREE.Color(colors[index % colors.length]));
      }
      rings.instanceMatrix.needsUpdate = true;
      if (rings.instanceColor) rings.instanceColor.needsUpdate = true;
      rings.computeBoundingSphere();
      return rings;
    };
    const detailedRings = buildRingBatch(
      new THREE.TorusGeometry(8, 0.55, 4, 12),
      "forkmesh-drone-race-rings-detailed",
      1,
    );
    const coarseRings = buildRingBatch(
      new THREE.TorusGeometry(8, 0.72, 3, 8),
      "forkmesh-drone-race-rings-coarse",
      0.82,
    );
    course.addLevel(detailedRings, 0, 0.12);
    course.addLevel(coarseRings, DRONE_COURSE_COARSE_DISTANCE, 0.16);
    course.userData.ringCount = DRONE_COURSE_RING_COUNT;
    course.userData.length = DRONE_COURSE_UNITS_PER_RADIAN * Math.PI * 2;
    course.userData.ringMeshes = [detailedRings, coarseRings];
    course.userData.ringColors = colors;
    course.userData.activeRingMaterial = material(
      THREE,
      { color: "#ffffff" },
      "MeshBasicMaterial",
    );
    const cameraCoursePosition = new THREE.Vector3();
    let detailedVisible = true;
    course.update = (activeCamera) => {
      activeCamera.getWorldPosition(cameraCoursePosition);
      course.worldToLocal(cameraCoursePosition);
      let nearestDistance = Infinity;
      ringCenters.forEach((center) => {
        nearestDistance = Math.min(
          nearestDistance,
          cameraCoursePosition.distanceTo(center),
        );
      });
      const boundary = detailedVisible
        ? DRONE_COURSE_COARSE_DISTANCE
        : DRONE_COURSE_DETAIL_ENTER_DISTANCE;
      detailedVisible = nearestDistance <= boundary;
      detailedRings.visible = detailedVisible;
      coarseRings.visible = !detailedVisible;
      course._currentLevel = detailedVisible ? 0 : 1;
      course.userData.detailDistance = nearestDistance;
      course.userData.detailLevel = detailedVisible ? "detailed" : "coarse";
    };
    return course;
  }

  function placeAlongCourse(transform, point, tangent, up) {
    const side = new THREE.Vector3().crossVectors(tangent, up).normalize();
    const lift = new THREE.Vector3().crossVectors(side, tangent).normalize();
    transform.matrix.makeBasis(side, tangent, lift);
    transform.matrix.setPosition(point);
  }

  function createDroneRaceGate() {
    const { start } = droneCourseGates();
    const half = DRONE_RACE_GATE_HALF_SIZE;
    const tiles = 12;
    const tile = (half * 2) / tiles;
    const parts = [
      [-half - 0.7, 0, 1.4, half * 2 + 4.6, 1.4, "#f7c96b"],
      [half + 0.7, 0, 1.4, half * 2 + 4.6, 1.4, "#f7c96b"],
    ];
    for (const [row, y] of [[0, half + tile * 0.5], [1, half + tile * 1.5], [0, -half - tile * 0.5]]) {
      for (let column = 0; column < tiles; column += 1) {
        parts.push([
          -half + tile * (column + 0.5),
          y,
          tile,
          tile,
          0.5,
          (column + row) % 2 ? "#161b22" : "#ffffff",
        ]);
      }
    }
    const gate = new THREE.InstancedMesh(
      primitive(THREE, "BoxGeometry", 1, 1, 1),
      material(THREE, { color: "#ffffff" }),
      parts.length,
    );
    gate.name = "forkmesh-drone-race-start-gate";
    gate.castShadow = false;
    gate.receiveShadow = false;
    const transform = new THREE.Object3D();
    const color = new THREE.Color();
    parts.forEach(([x, y, width, height, depth, tint], index) => {
      transform.position.set(x, y, 0);
      transform.scale.set(width, height, depth);
      transform.updateMatrix();
      gate.setMatrixAt(index, transform.matrix);
      gate.setColorAt(index, color.set(tint));
    });
    gate.instanceMatrix.needsUpdate = true;
    if (gate.instanceColor) gate.instanceColor.needsUpdate = true;
    gate.computeBoundingSphere();
    gate.position.set(...start.center);
    gate.up.set(0, 1, 0);
    gate.lookAt(
      start.center[0] + start.normal[0],
      start.center[1] + start.normal[1],
      start.center[2] + start.normal[2],
    );
    const label = makeLabelSprite(
      THREE,
      "START · FINISH",
      "FLY THROUGH TO TIME A LAP",
      "#f7c96b",
    );
    label.name = "forkmesh-drone-race-start-label";
    label.position.set(0, half + 6.5, 0);
    label.scale.set(12, 4, 1);
    label.visible = false;
    gate.add(label);
    return { gate, label };
  }

  function chevronArmMatrices() {
    const tip = [0, 1.8];
    const tilt = new THREE.Matrix4().makeRotationX(DRONE_RACE_ARROW_TILT);
    const arms = [];
    for (const side of [-1, 1]) {
      const endX = side * 2.4;
      const endY = -0.9;
      const dx = endX - tip[0];
      const dy = endY - tip[1];
      const arm = new THREE.Object3D();
      arm.position.set((tip[0] + endX) / 2, (tip[1] + endY) / 2, 0);
      arm.rotation.z = Math.atan2(dy, dx);
      arm.scale.set(Math.hypot(dx, dy) + 0.5, 0.8, 0.35);
      arm.updateMatrix();
      arms.push(tilt.clone().multiply(arm.matrix));
    }
    return arms;
  }

  function createDroneRaceArrows() {
    const angles = droneRaceArrowAngles();
    const arms = chevronArmMatrices();
    const arrows = new THREE.InstancedMesh(
      primitive(THREE, "BoxGeometry", 1, 1, 1),
      material(THREE, { color: "#ffffff" }, "MeshBasicMaterial"),
      (angles.length + 1) * arms.length,
    );
    arrows.name = "forkmesh-drone-race-arrows";
    arrows.castShadow = false;
    arrows.receiveShadow = false;
    arrows.frustumCulled = false;
    arrows.visible = false;
    const transform = new THREE.Object3D();
    const point = new THREE.Vector3();
    const tangent = new THREE.Vector3();
    const up = new THREE.Vector3(0, 1, 0);
    const color = new THREE.Color("#79c0ff");
    const instance = new THREE.Matrix4();
    const matrices = angles.map((angle, index) => {
      point.set(...droneCoursePoint(angle));
      tangent.set(...droneCourseTangent(angle));
      placeAlongCourse(transform, point, tangent, up);
      arms.forEach((arm, armIndex) => {
        instance.copy(transform.matrix).multiply(arm);
        arrows.setMatrixAt(index * arms.length + armIndex, instance);
        arrows.setColorAt(index * arms.length + armIndex, color);
      });
      return transform.matrix.clone();
    });
    arms.forEach((_, armIndex) => {
      arrows.setMatrixAt(angles.length * arms.length + armIndex, instance.makeScale(0, 0, 0));
      arrows.setColorAt(angles.length * arms.length + armIndex, color);
    });
    arrows.instanceMatrix.needsUpdate = true;
    arrows.instanceColor.needsUpdate = true;
    arrows.userData.baseMatrices = matrices;
    arrows.userData.armMatrices = arms;
    arrows.userData.perTarget = DRONE_RACE_ARROWS_PER_TARGET;
    arrows.userData.guideChevron = angles.length;
    return arrows;
  }

  function createDroneRaceGhost() {
    const ghost = ctx.quadcopterPrototype.clone(true);
    ghost.name = "forkmesh-drone-race-ghost";
    const ghostMaterial = material(
      THREE,
      {
        color: "#b6ffd9",
        transparent: true,
        opacity: 0.55,
        depthWrite: false,
      },
      "MeshBasicMaterial",
    );
    const drop = [];
    ghost.traverse((child) => {
      if (child.isSprite) drop.push(child);
      if (!child.isMesh) return;
      child.material = ghostMaterial;
      child.castShadow = false;
      child.receiveShadow = false;
    });
    drop.forEach((child) => child.removeFromParent());
    ghost.visible = false;
    return ghost;
  }

  return {
    createWorldQuadcopterPrototype,
    addWorldQuadcopter,
    createDroneRaceCourse,
    createDroneRaceGate,
    createDroneRaceArrows,
    createDroneRaceGhost,
  };
}

export async function build({
  THREE,
  addWorldQuadcopter,
  createDroneRaceArrows,
  createDroneRaceCourse,
  createDroneRaceGate,
  createDroneRaceGhost,
  createWorldQuadcopterPrototype,
  ctx,
  instancedMirrorSets,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  // Both rideable quadcopters are deep clones of one prototype. Object3D
  // cloning shares immutable geometry, materials, and sign textures while
  // retaining independent transforms and rotor animation state.
  const quadcopterStates = [];
  ctx.quadcopterStates = quadcopterStates;
  const quadcopterPrototype = createWorldQuadcopterPrototype();
  ctx.quadcopterPrototype = quadcopterPrototype;
  const quadcopterRotorBatches = new THREE.Group();
  quadcopterRotorBatches.name = "forkmesh-world-quadcopter-rotor-batches";
  world.add(quadcopterRotorBatches);
  const quadcopterRotorMirror = createInstancedMirrorSet(
    THREE,
    quadcopterRotorBatches,
    { name: "forkmesh-world-quadcopter-rotors" },
  );
  ctx.quadcopterRotorMirror = quadcopterRotorMirror;
  instancedMirrorSets.add(quadcopterRotorMirror);
  registerWorldElement(
    "quadcopter", "Quadcopters", "Vehicles & rides", quadcopterRotorBatches,
  );
  await sceneSlice();
  addWorldQuadcopter(0, [-34, 0.24, -42]);
  addWorldQuadcopter(1, [34, 0.24, -42]);
  const droneRaceCourse = createDroneRaceCourse();
  world.add(droneRaceCourse);
  const { gate, label } = createDroneRaceGate();
  const arrows = createDroneRaceArrows();
  const ghost = createDroneRaceGhost();
  world.add(gate, arrows, ghost);
  droneRaceCourse.userData.race = { gate, label, arrows, ghost };
  registerWorldElement(
    "drone-race-course",
    "World drone race · 10 rings around the coaster",
    "Vehicles & rides",
    droneRaceCourse,
  );
  return {
  };
}
