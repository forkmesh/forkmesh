import { createInstancedMirrorSet } from "../../world-resource-cache.js";
import { makeGroundPlaque } from "../arrival.js";
import {
  GYM_HEAVY_WEIGHT_LB,
  GYM_MAX_WEIGHT_LB,
  GYM_POSITION,
  GYM_BENCH_PRESS_DEPTH,
  GYM_BENCH_PRESS_TOP_Y,
  GYM_BENCH_REP_MS,
  SWING_SET_POSITION,
} from "../gameplay-constants.js";
import { worldLayoutId } from "../layout.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { clamp, setShadows } from "../object-utils.js";
import * as resources from "../resources.js";

export async function build({
  THREE,
  ctx,
  frameAnimated,
  instancedMirrorSets,
  interactive,
  onGymState,
  reducedMotion,
  registerWorldElement,
  sceneSlice,
  swingRideAmplitude,
  world,
}) {
  // A wooden swing set beside the Office garden gym: three swings hang from
  // one beam, so up to three visitors can ride at once. Clicking a seat starts
  // the ride and clicking it again hops off; the shell's swing-speed slider
  // scales the pumping, and the regular camera toggle watches the ride in
  // first or third person because both camera modes follow the player's
  // position.
  const SWING_SEAT_COUNT = 3;
  const SWING_SEAT_SPACING = 2.3;
  const SWING_BEAM_HEIGHT = 4.6;
  const SWING_ROPE_LENGTH = 3.4;
  const SWING_SEAT_HALF_THICKNESS = 0.05;
  const SWING_REMOTE_AMPLITUDE = 0.55;
  const SWING_ARM_HOLD_PITCH = 1.15;
  const swingSet = new THREE.Group();
  swingSet.name = "swing-set";
  swingSet.position.set(...SWING_SET_POSITION);
  // Face the nearby gym so the swings and workout equipment read as one
  // compact recreation garden.
  swingSet.rotation.y = -Math.PI / 2;
  const swingFrameMaterial = makeMaterial(THREE, "#6f5136", { roughness: 0.82 });
  const swingFrameWidth = SWING_SEAT_COUNT * SWING_SEAT_SPACING + 1.6;
  const swingLegSpread = 1.7;
  const swingLegLength = Math.hypot(SWING_BEAM_HEIGHT, swingLegSpread);
  [-1, 1].forEach((side) => {
    [-1, 1].forEach((lean) => {
      const leg = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.09, 0.11, swingLegLength, 8),
        swingFrameMaterial,
      );
      leg.position.set(
        (side * swingFrameWidth) / 2,
        SWING_BEAM_HEIGHT / 2,
        (lean * swingLegSpread) / 2,
      );
      leg.rotation.x = -lean * Math.atan2(swingLegSpread, SWING_BEAM_HEIGHT);
      swingSet.add(leg);
    });
  });
  const swingBeam = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.1, 0.1, swingFrameWidth + 0.7, 10),
    swingFrameMaterial,
  );
  swingBeam.rotation.z = Math.PI / 2;
  swingBeam.position.y = SWING_BEAM_HEIGHT;
  swingSet.add(swingBeam);
  const swingRopeMaterial = makeMaterial(THREE, "#d8cdb4", { roughness: 0.9 });
  const swingSeatMaterial = makeMaterial(THREE, "#a8763e", { roughness: 0.6 });
  const swingStates = [];
  for (let seatIndex = 0; seatIndex < SWING_SEAT_COUNT; seatIndex += 1) {
    const pivot = new THREE.Group();
    pivot.position.set(
      (seatIndex - (SWING_SEAT_COUNT - 1) / 2) * SWING_SEAT_SPACING,
      SWING_BEAM_HEIGHT,
      0,
    );
    [-1, 1].forEach((side) => {
      const rope = new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.03, 0.03, SWING_ROPE_LENGTH, 6),
        swingRopeMaterial,
      );
      rope.position.set(side * 0.45, -SWING_ROPE_LENGTH / 2, 0);
      rope.userData.swingSeat = seatIndex;
      interactive.push(rope);
      pivot.add(rope);
    });
    const seat = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 1.05, SWING_SEAT_HALF_THICKNESS * 2, 0.48),
      swingSeatMaterial,
    );
    seat.position.y = -SWING_ROPE_LENGTH;
    seat.userData.swingSeat = seatIndex;
    interactive.push(seat);
    pivot.add(seat);
    swingSet.add(pivot);
    // The rest anchor hangs outside the pivot so occupancy checks and the
    // step-off spot read the seat's still position, not the moving plank.
    const anchor = new THREE.Object3D();
    anchor.position.set(
      pivot.position.x,
      SWING_BEAM_HEIGHT - SWING_ROPE_LENGTH,
      0,
    );
    swingSet.add(anchor);
    swingStates.push({
      pivot,
      seat,
      anchor,
      amplitude: 0,
      // Staggered so idle and remote-ridden swings never move in lockstep.
      phase: seatIndex * 1.3,
      remoteId: "",
    });
  }
  const swingMirror = createInstancedMirrorSet(THREE, swingSet, {
    name: "swing-set-instances",
  });
  swingStates.forEach(({ pivot }) => {
    pivot.children.forEach((part) => swingMirror.add(part));
  });
  instancedMirrorSets.add(swingMirror);
  const swingPendulumOmega = Math.sqrt(9.8 / SWING_ROPE_LENGTH);
  frameAnimated.push((time, delta) => {
    swingStates.forEach((swing, seatIndex) => {
      const target =
        ctx.swingRide?.index === seatIndex
          ? swingRideAmplitude()
          : swing.remoteId
            ? SWING_REMOTE_AMPLITUDE
            : 0;
      swing.amplitude += (target - swing.amplitude) * Math.min(1, delta * 1.4);
      if (swing.amplitude < 0.01 && !target) {
        swing.pivot.rotation.x = 0;
        return;
      }
      // Pumping harder swings a little faster as well as higher; the phase is
      // continuous so speed changes never snap the seat sideways.
      swing.phase += swingPendulumOmega * (0.8 + swing.amplitude * 0.5) * delta;
      swing.pivot.rotation.x = swing.amplitude * Math.sin(swing.phase);
    });
  });
  setShadows(swingSet);
  world.add(swingSet);
  registerWorldElement("swing-set", "Swing set", "Recreation", swingSet);
  await sceneSlice();

  // An open-air gym beside the Town Square. Every station is a real click
  // target; the bench press also opens the shell's weight/rep console.
  const gym = new THREE.Group();
  gym.name = "forkmesh-world-gym";
  gym.position.set(...GYM_POSITION);
  const gymFloor = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 28, 0.22, 18),
    makeSharedMaterial(THREE, "#17202a", { roughness: 0.86 }),
  );
  gymFloor.position.y = 0;
  gymFloor.receiveShadow = true;
  gym.add(gymFloor);
  const gymInset = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 26.8, 0.035, 16.8),
    makeSharedMaterial(THREE, "#253240", { roughness: 0.96 }),
  );
  gymInset.position.y = 0.13;
  gym.add(gymInset);
  // Keep wayfinding physically anchored to the scene. Floating labels are
  // reserved for features that explicitly need screen-facing identification.
  const gymSign = makeGroundPlaque(
    THREE,
    "WORLD GYM",
    "CLICK A STATION TO TRAIN",
    "#ff9b54",
  );
  gymSign.name = "world-gym-entrance-plaque";
  gymSign.position.set(0, 0.14, -8.1);
  gymSign.rotation.y = Math.PI;
  gym.add(gymSign);
  const gymSteel = makeMaterial(THREE, "#a8b3bf", {
    metalness: 0.82,
    roughness: 0.23,
  });
  const gymDark = makeMaterial(THREE, "#1b2632", {
    metalness: 0.42,
    roughness: 0.46,
  });
  const gymAccent = makeMaterial(THREE, "#ff7b54", {
    metalness: 0.36,
    roughness: 0.34,
    emissive: "#8b2e1d",
    emissiveIntensity: 0.28,
  });
  const gymEquipment = new Map();
  const markGymInteractive = (object, kind) => {
    object.userData.gymEquipment = kind;
    interactive.push(object);
    gymEquipment.set(kind, gymEquipment.get(kind) || object);
    return object;
  };

  // Bench press: the shaft is split into a centre and two hinged sleeves so
  // genuinely heavy selections can visibly flex instead of only translating.
  const benchPress = new THREE.Group();
  benchPress.name = "gym-bench-press";
  benchPress.position.set(-7.6, 0.18, -1.4);
  const benchPad = markGymInteractive(
    new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 2.1, 0.32, 5.1), gymAccent),
    "bench",
  );
  benchPad.position.set(0, 1.02, 0.55);
  benchPress.add(benchPad);
  [-0.82, 0.82].forEach((x) => {
    const leg = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.22, 1.02, 0.22),
      gymSteel,
    );
    leg.position.set(x, 0.52, 0.55);
    benchPress.add(leg);
  });
  [-1.25, 1.25].forEach((x) => {
    const upright = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.24, 3.15, 0.32),
      gymSteel,
    );
    upright.position.set(x, 1.68, -1.25);
    benchPress.add(upright);
    const hook = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.45, 0.18, 0.55),
      gymDark,
    );
    hook.position.set(x, 2.94, -1.08);
    benchPress.add(hook);
  });
  const benchBarRig = new THREE.Group();
  benchBarRig.position.set(0, 2.9, -0.78);
  benchPress.add(benchBarRig);
  const benchBarCenter = markGymInteractive(
    new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.075, 0.075, 2.5, 16),
      gymSteel,
    ),
    "bench",
  );
  benchBarCenter.rotation.z = Math.PI / 2;
  benchBarRig.add(benchBarCenter);
  const benchBarSleeves = [];
  const benchPlates = [];
  [-1, 1].forEach((side) => {
    const pivot = new THREE.Group();
    pivot.position.x = side * 1.25;
    benchBarRig.add(pivot);
    const sleeve = markGymInteractive(
      new THREE.Mesh(
        resources.primitive(THREE, "CylinderGeometry", 0.09, 0.09, 1.55, 16),
        gymSteel,
      ),
      "bench",
    );
    sleeve.rotation.z = Math.PI / 2;
    sleeve.position.x = side * 0.775;
    pivot.add(sleeve);
    benchBarSleeves.push({ pivot, side });
    for (let plateIndex = 0; plateIndex < 3; plateIndex += 1) {
      const plate = markGymInteractive(
        new THREE.Mesh(
          resources.primitive(THREE, "CylinderGeometry", 0.56, 0.56, 0.14, 24),
          plateIndex === 0 ? gymAccent : gymDark,
        ),
        "bench",
      );
      plate.rotation.z = Math.PI / 2;
      plate.position.x = side * (0.42 + plateIndex * 0.18);
      pivot.add(plate);
      benchPlates.push(plate);
    }
  });
  gym.add(benchPress);

  const treadmill = new THREE.Group();
  treadmill.name = "gym-treadmill";
  treadmill.position.set(0, 0.18, -3.7);
  const treadmillBelt = markGymInteractive(
    new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 2.6, 0.3, 5.5), gymDark),
    "treadmill",
  );
  treadmillBelt.position.y = 0.34;
  treadmill.add(treadmillBelt);
  const treadmillConsole = markGymInteractive(
    new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 2.4, 1.05, 0.3), gymAccent),
    "treadmill",
  );
  treadmillConsole.position.set(0, 2.3, -2.05);
  treadmill.add(treadmillConsole);
  [-1.05, 1.05].forEach((x) => {
    const rail = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.07, 0.07, 2.35, 10),
      gymSteel,
    );
    rail.position.set(x, 1.25, -1.85);
    treadmill.add(rail);
  });
  gym.add(treadmill);

  const exerciseBike = new THREE.Group();
  exerciseBike.name = "gym-exercise-bike";
  exerciseBike.position.set(6.9, 0.18, -3.4);
  const bikeWheel = markGymInteractive(
    new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", 1.15, 0.13, 12, 30),
      gymAccent,
    ),
    "bike",
  );
  bikeWheel.position.set(0, 1.25, 0.25);
  bikeWheel.rotation.y = Math.PI / 2;
  exerciseBike.add(bikeWheel);
  const bikeFrame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.22, 2.2, 0.22),
    gymSteel,
  );
  bikeFrame.position.set(0, 1.22, 0);
  bikeFrame.rotation.x = -0.35;
  exerciseBike.add(bikeFrame);
  const bikeSeat = markGymInteractive(
    new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 1.1, 0.2, 0.55), gymDark),
    "bike",
  );
  bikeSeat.position.set(0, 2.35, 0.34);
  exerciseBike.add(bikeSeat);
  gym.add(exerciseBike);

  const rower = new THREE.Group();
  rower.name = "gym-rower";
  rower.position.set(6.6, 0.18, 3.7);
  const rowerRail = markGymInteractive(
    new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 1.25, 0.24, 5.6), gymSteel),
    "rower",
  );
  rowerRail.position.y = 0.55;
  rower.add(rowerRail);
  const rowerSeat = markGymInteractive(
    new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 1.45, 0.22, 0.85), gymAccent),
    "rower",
  );
  rowerSeat.position.set(0, 0.86, 1.05);
  rower.add(rowerSeat);
  const rowerHandle = markGymInteractive(
    new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 1.65, 0.14, 0.14), gymDark),
    "rower",
  );
  rowerHandle.position.set(0, 1.45, -1.65);
  rower.add(rowerHandle);
  gym.add(rower);

  const punchingStation = new THREE.Group();
  punchingStation.name = "gym-punching-bag";
  punchingStation.position.set(-0.4, 0.18, 4.8);
  const bagArm = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 3.1, 0.2, 0.2),
    gymSteel,
  );
  bagArm.position.set(0, 4.4, 0);
  punchingStation.add(bagArm);
  const bag = markGymInteractive(
    new THREE.Mesh(
      resources.primitive(THREE, "CapsuleGeometry", 0.72, 2.15, 8, 18),
      gymAccent,
    ),
    "punch",
  );
  bag.position.set(0.8, 2.55, 0);
  punchingStation.add(bag);
  gym.add(punchingStation);

  [
    ["BENCH PRESS", "WEIGHT + REPS", -10.7, 0.14, -3.7, Math.PI / 2],
    ["TREADMILL", "CLICK TO RUN", -3.35, 0.14, -6.35, Math.PI],
    ["EXERCISE BIKE", "CLICK TO RIDE", 8.8, 0.14, -6.35, Math.PI],
    ["ROWER", "CLICK TO ROW", 10.65, 0.14, 5.7, -Math.PI / 2],
    ["PUNCHING BAG", "CLICK TO TRAIN", -3.35, 0.14, 7.0, 0],
  ].forEach(([title, subtitle, x, y, z, rotation]) => {
    const plaque = makeGroundPlaque(THREE, title, subtitle, "#ffad8f");
    plaque.name = `world-gym-station-plaque:${worldLayoutId("", title)}`;
    plaque.position.set(x, y, z);
    plaque.rotation.y = rotation;
    gym.add(plaque);
  });

  const gymState = {
    active: "",
    auto: false,
    weightLb: 135,
    reps: 0,
    repStartedAt: 0,
    activityStartedAt: 0,
    benchBarRig,
    benchBarSleeves,
    benchPlates,
    treadmillBelt,
    bikeWheel,
    rowerSeat,
    rowerHandle,
    bag,
  };
  const gymPublicState = (open = gymState.active === "bench") => ({
    active: gymState.active,
    auto: gymState.auto,
    weightLb: gymState.weightLb,
    reps: gymState.reps,
    heavy: gymState.weightLb >= GYM_HEAVY_WEIGHT_LB,
    open,
  });
  frameAnimated.push((time) => {
    const repElapsed = gymState.repStartedAt
      ? time - gymState.repStartedAt
      : Infinity;
    const repProgress = clamp(repElapsed / GYM_BENCH_REP_MS, 0, 1);
    const lift =
      repProgress < 1 ? Math.sin(repProgress * Math.PI) : 0;
    const heaviness = clamp(
      (gymState.weightLb - GYM_HEAVY_WEIGHT_LB) /
        (GYM_MAX_WEIGHT_LB - GYM_HEAVY_WEIGHT_LB),
      0,
      1,
    );
    const bend = lift * (0.055 + heaviness * 0.15);
    gymState.benchBarRig.position.y = gymState.active === "bench"
      ? GYM_BENCH_PRESS_TOP_Y - lift * GYM_BENCH_PRESS_DEPTH
      : 2.9;
    gymState.benchBarRig.position.x =
      !reducedMotion && heaviness && repProgress < 1
        ? Math.sin(repProgress * Math.PI * 7) * heaviness * 0.045
        : 0;
    gymState.benchBarSleeves.forEach(({ pivot, side }) => {
      pivot.rotation.z = side * bend;
    });
    const plateScale = 0.72 + clamp(gymState.weightLb / 700, 0, 0.72);
    gymState.benchPlates.forEach((plate) => {
      plate.scale.set(plateScale, 1, plateScale);
    });
    if (
      gymState.auto &&
      gymState.active === "bench" &&
      repElapsed >= 1220
    ) {
      gymState.repStartedAt = time;
      gymState.reps += 1;
      onGymState(gymPublicState(true));
    }
    const exerciseTime = Math.max(0, time - gymState.activityStartedAt);
    if (gymState.active === "bike") {
      gymState.bikeWheel.rotation.z = exerciseTime * 0.008;
    }
    if (gymState.active === "rower") {
      const stroke = (Math.sin(exerciseTime * 0.006) + 1) / 2;
      gymState.rowerSeat.position.z = 0.45 + stroke * 1.3;
      gymState.rowerHandle.position.z = -1.75 + stroke * 1.15;
    }
    if (gymState.active === "punch") {
      gymState.bag.rotation.z = Math.sin(exerciseTime * 0.011) * 0.16;
    } else {
      gymState.bag.rotation.z *= 0.9;
    }
  });
  setShadows(gym);
  world.add(gym);
  registerWorldElement("gym", "Outdoor gym", "Recreation", gym);
  return {
    SWING_SEAT_HALF_THICKNESS,
    SWING_ARM_HOLD_PITCH,
    swingSet,
    swingStates,
    gym,
    gymState,
    gymPublicState,
  };
}
