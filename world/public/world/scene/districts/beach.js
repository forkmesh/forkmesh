import { sharedGeometry } from "../../world-resource-cache.js";
import { makeLabelSprite } from "../labels.js";
import {
  BEACH_CENTER_X,
  BEACH_CENTER_Z,
  BEACH_RADIUS,
  BEACH_ROAD_HALF_WIDTH,
  BEACH_ROAD_MAX_X,
  BEACH_ROAD_MIN_X,
  BEACH_SCENE_PORTAL_X,
  BEACH_TOWN_PORTAL_X,
} from "../layout.js";
import {
  cloneSharedMesh,
  makeMaterial,
  makeSharedMaterial,
} from "../materials.js";
import { setShadows } from "../object-utils.js";
import { concreteBrickMaterial, projectAssetTexture } from "../town.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
  fns,
}) {
  function createBeachPortal(name, label, action, x, heading) {
    const portal = new THREE.Group();
    portal.name = name;
    portal.position.set(x, 0.22, BEACH_CENTER_Z);
    portal.rotation.y = heading;
    const ring = new THREE.Mesh(
      sharedGeometry(THREE, "TorusGeometry", 4.15, 0.38, 12, 48),
      makeSharedMaterial(THREE, "#77d9ff", {
        emissive: "#1f8fb8",
        emissiveIntensity: 1.05,
      }),
    );
    const face = new THREE.Mesh(
      sharedGeometry(THREE, "CircleGeometry", 3.72, 48),
      makeSharedMaterial(THREE, "#52c7df", {
        emissive: "#176b92",
        transparent: true,
        opacity: 0.48,
        side: THREE.DoubleSide,
      }),
    );
    [ring, face].forEach((surface, index) => {
      surface.name = `${name}-${index ? "face" : "ring"}`;
      surface.position.set(0, 4.25, index ? 0.04 : 0);
      surface.userData.interactive = action;
      ctx.interactive.push(surface);
    });
    const portalLabel = makeLabelSprite(
      THREE, label, "CLICK TO TRAVEL", "#77d9ff",
    );
    portalLabel.name = `${name}-label`;
    portalLabel.position.set(0, 9.25, 0);
    portalLabel.scale.set(6.4, 2.15, 1);
    portal.add(ring, face, portalLabel);
    return portal;
  }

  function addWorldBench({
    name,
    x,
    z,
    heading = 0,
    activity = "sitting in a city park",
  }) {
    const bench = new THREE.Group();
    bench.name = name;
    bench.position.set(x, 0, z);
    bench.rotation.y = heading;
    const wood = makeSharedMaterial(THREE, "#7b5436", { roughness: 0.82 });
    const metal = makeSharedMaterial(THREE, "#30363d", {
      metalness: 0.45,
      roughness: 0.52,
    });
    const seat = cloneSharedMesh(
      THREE,
      "world-bench-seat",
      () => resources.primitive(THREE, "BoxGeometry", 4.2, 0.22, 1.05),
      wood,
    );
    seat.name = `${name}-seat`;
    seat.position.y = 1.08;
    seat.userData.worldSeat = true;
    seat.userData.worldSeatHeading = heading;
    seat.userData.worldSeatActivity = activity;
    bench.add(seat);
    ctx.interactive.push(seat);
    const back = cloneSharedMesh(
      THREE,
      "world-bench-back",
      () => resources.primitive(THREE, "BoxGeometry", 4.2, 1.25, 0.2),
      wood,
    );
    back.position.set(0, 1.72, 0.46);
    bench.add(back);
    for (const xOffset of [-1.55, 1.55]) {
      const leg = cloneSharedMesh(
        THREE,
        "world-bench-leg",
        () => resources.primitive(THREE, "BoxGeometry", 0.22, 1.05, 0.72),
        metal,
      );
      leg.position.set(xOffset, 0.54, 0);
      bench.add(leg);
    }
    setShadows(bench);
    ctx.world.add(bench);
    fns.registerWorldElement("benches", "Town benches", "Scenery", bench);
    return bench;
  }
  function addBeachCar() {
    const car = new THREE.Group();
    car.name = "forkmesh-beach-road-car";
    const bodyMaterial = makeMaterial(THREE, "#238636", {
      metalness: 0.48,
      roughness: 0.32,
    });
    const body = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 3.5, 0.72, 6.2),
      bodyMaterial,
    );
    body.position.y = 1.05;
    car.add(body);
    const cabin = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 3.05, 1.05, 3.1),
      makeSharedMaterial(THREE, "#8ac7df", {
        metalness: 0.18,
        roughness: 0.18,
        transparent: true,
        opacity: 0.78,
      }),
    );
    cabin.position.set(0, 1.78, 0.15);
    car.add(cabin);
    const wheels = [];
    for (const xOffset of [-1.72, 1.72]) {
      for (const zOffset of [-2.05, 2.05]) {
        const wheel = cloneSharedMesh(
          THREE,
          "beach-car-wheel",
          () => resources.primitive(THREE, "CylinderGeometry", 0.58, 0.58, 0.34, 20),
          makeSharedMaterial(THREE, "#101418", { roughness: 0.88 }),
        );
        wheel.rotation.z = Math.PI / 2;
        wheel.position.set(xOffset, 0.62, zOffset);
        car.add(wheel);
        wheels.push(wheel);
      }
    }
    car.position.set(BEACH_CENTER_X - 38, 0.04, BEACH_CENTER_Z);
    car.rotation.y = -Math.PI / 2;
    const hint = makeLabelSprite(
      THREE,
      "DRIVE",
      "CLICK TO ENTER · WASD DRIVE · SHIFT BOOST",
      "#9ef7c6",
    );
    hint.name = "forkmesh-beach-road-car-control-hint";
    hint.position.set(0, 4.35, 0);
    hint.scale.set(6.8, 2.25, 1);
    car.add(hint);
    car.traverse((child) => {
      if (!child.isMesh) return;
      child.userData.carIndex = 0;
      ctx.interactive.push(child);
    });
    setShadows(car);
    ctx.beachScene.add(car);
    fns.registerWorldElement("beach-car", "Beach car", "Vehicles & rides", car);
    ctx.carStates.push({ car, wheels, moving: false });
  }

  return {
    createBeachPortal,
    addWorldBench,
    addBeachCar,
  };
}

export async function build({
  THREE,
  addBeachCar,
  addWorldBench,
  createBeachPortal,
  ctx,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  // The coastal access road and rest area live together in the destination
  // scene. The beach uses only local, preloaded assets so entering it never
  // waits on a remote video host or triggers a large network stall.
  const beachRoadLength = BEACH_ROAD_MAX_X - BEACH_ROAD_MIN_X;
  const beachScene = new THREE.Group();
  ctx.beachScene = beachScene;
  beachScene.name = "forkmesh-beach-scene";
  beachScene.userData.beachScene = true;
  beachScene.visible = false;
  world.add(beachScene);
  // Populated once the Office interior exists; created here so the beach
  // enclosure can keep the cable visible from the sand.
  const zipLine = new THREE.Group();
  zipLine.name = "forkmesh-roof-beach-zip-line";
  world.add(zipLine);
  const beachRoad = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      beachRoadLength,
      0.18,
      BEACH_ROAD_HALF_WIDTH * 2,
    ),
    concreteBrickMaterial(THREE),
  );
  beachRoad.name = "forkmesh-beach-road";
  beachRoad.position.set(
    (BEACH_ROAD_MIN_X + BEACH_ROAD_MAX_X) / 2,
    0.09,
    BEACH_CENTER_Z,
  );
  beachRoad.receiveShadow = true;
  beachRoad.userData.ground = true;
  beachScene.add(beachRoad);

  const beachFoundation = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", BEACH_RADIUS, BEACH_RADIUS, 4.8, 96),
    makeMaterial(THREE, "#7e6850", { roughness: 1 }),
  );
  beachFoundation.name = "forkmesh-beach-foundation";
  beachFoundation.position.set(BEACH_CENTER_X, -2.4, BEACH_CENTER_Z);
  beachFoundation.receiveShadow = true;
  beachScene.add(beachFoundation);
  const beachSand = new THREE.Mesh(
    resources.primitive(THREE, "CircleGeometry", BEACH_RADIUS, 96),
    makeMaterial(THREE, "#d8c49a", { roughness: 1 }),
  );
  beachSand.name = "forkmesh-beach-sand";
  beachSand.rotation.x = -Math.PI / 2;
  beachSand.position.set(BEACH_CENTER_X, 0.035, BEACH_CENTER_Z);
  beachSand.receiveShadow = true;
  beachSand.userData.ground = true;
  beachScene.add(beachSand);
  const beachWater = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 190, 260),
    makeMaterial(THREE, "#2b83a6", {
      metalness: 0.05,
      roughness: 0.24,
      transparent: true,
      opacity: 0.88,
    }),
  );
  beachWater.name = "forkmesh-beach-water";
  beachWater.rotation.x = -Math.PI / 2;
  beachWater.position.set(BEACH_CENTER_X + 116, -0.08, BEACH_CENTER_Z);
  beachScene.add(beachWater);
  const beachHorizonTexture = projectAssetTexture(
    THREE,
    "/world/assets/beach-horizon-v1.webp",
    1,
    1,
  );
  const beachHorizon = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 72, 72, 38, 64, 1, true),
    resources.uniqueMaterial(THREE, {
      map: beachHorizonTexture,
      side: THREE.BackSide,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  beachHorizon.name = "forkmesh-beach-peripheral-horizon";
  beachHorizon.position.set(BEACH_CENTER_X, 18, BEACH_CENTER_Z);
  beachScene.add(beachHorizon);
  registerWorldElement(
    "beach", "Beach & ocean", "Terrain",
    [beachRoad, beachFoundation, beachSand, beachWater, beachHorizon],
  );
  await sceneSlice();

  const beachEntryPortal = createBeachPortal(
    "forkmesh-beach-entry-portal",
    "BEACH",
    "beach-scene-enter",
    BEACH_TOWN_PORTAL_X,
    -Math.PI / 2,
  );
  world.add(beachEntryPortal);
  const beachReturnPortal = createBeachPortal(
    "forkmesh-beach-return-portal",
    "RETURN TO TOWN",
    "beach-scene-exit",
    BEACH_SCENE_PORTAL_X,
    Math.PI / 2,
  );
  beachScene.add(beachReturnPortal);
  registerWorldElement(
    "beach-portals",
    "Beach scene portals",
    "Navigation",
    [beachEntryPortal, beachReturnPortal],
  );
  await sceneSlice();
  addWorldBench({
    name: "forkmesh-swing-park-bench-west",
    x: 46,
    z: -129,
    heading: Math.PI,
    activity: "resting beside the recreation garden",
  });
  addWorldBench({
    name: "forkmesh-swing-park-bench-east",
    x: 46,
    z: -145,
    heading: 0,
    activity: "resting beside the recreation garden",
  });
  const beachBench = addWorldBench({
    name: "forkmesh-beach-bench",
    x: BEACH_CENTER_X + 24,
    z: BEACH_CENTER_Z - 10,
    heading: -Math.PI / 2,
    activity: "sitting at the ForkMesh beach",
  });
  beachScene.add(beachBench);

  const carStates = [];
  ctx.carStates = carStates;
  addBeachCar();
  return {
    zipLine,
  };
}
