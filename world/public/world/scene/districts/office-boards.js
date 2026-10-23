import {
  OFFICE_FLOOR_HEIGHT,
  OFFICE_RECEPTION_DESK,
  officeHexWallPoint,
} from "../../world-office-tower.js";
import {
  worldQaArrowTexture,
  worldQaCardTexture,
} from "../boards-tasks.js";
import { canvasTexture } from "../canvas.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { setShadows } from "../object-utils.js";
import * as resources from "../resources.js";

export async function build({
  THREE,
  animated,
  ctx,
  interactive,
  officeFloorAccent,
  officeInterior,
  placeBillboardOnIsland,
  registerCompactDistrictRoot,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  // Landscaped arrival garden between the bridge and glass office.
  const officeFrontGarden = new THREE.Group();
  officeFrontGarden.name = "forkmesh-office-front-garden";
  const gardenPathMaterial = makeMaterial(THREE, "#d8d3c5", { roughness: 0.96 });
  for (const [x, z, width, depth] of [
    [-31, -140, 49, 5],
    [40, -140, 58, 5],
  ]) {
    const path = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", width, 0.14, depth),
      gardenPathMaterial,
    );
    path.position.set(x, 0.07, z);
    path.receiveShadow = true;
    officeFrontGarden.add(path);
  }
  const gardenFlowerColors = ["#ff668d", "#ffd65a", "#8c6cff", "#64e88d"];
  // Thirty-four shrubs in one instanced draw instead of thirty-four meshes,
  // each of which had its own sphere buffer and material.
  const gardenPlantCount = 34;
  const gardenPlants = new THREE.InstancedMesh(
    resources.primitive(THREE, "SphereGeometry", 1, 12, 8),
    makeSharedMaterial(THREE, "#ffffff", { roughness: 0.86 }),
    gardenPlantCount,
  );
  gardenPlants.name = "office-garden-plant";
  const gardenPlantPose = new THREE.Object3D();
  const gardenPlantColor = new THREE.Color();
  for (let index = 0; index < gardenPlantCount; index += 1) {
    const side = index % 2 ? -1 : 1;
    const x = side * (13 + (index % 6) * 5.1);
    const z = -120 - Math.floor(index / 6) * 7.2;
    gardenPlantPose.position.set(x, 0.72, z);
    gardenPlantPose.scale.setScalar(0.8 + (index % 3) * 0.12);
    gardenPlantPose.updateMatrix();
    gardenPlants.setMatrixAt(index, gardenPlantPose.matrix);
    gardenPlants.setColorAt(
      index,
      gardenPlantColor.set(
        index % 5 ? "#3d9b59" : gardenFlowerColors[index % 4],
      ),
    );
  }
  gardenPlants.computeBoundingSphere();
  officeFrontGarden.add(gardenPlants);

  const drinkingFountain = new THREE.Group();
  drinkingFountain.name = "forkmesh-drinking-fountain";
  drinkingFountain.position.set(25, 0, -145);
  const fountainBody = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.75, 0.92, 2.5, 16),
    makeSharedMaterial(THREE, "#9da8aa", { metalness: 0.72, roughness: 0.3 }),
  );
  fountainBody.position.y = 1.25;
  fountainBody.userData.interactive = "drinking-fountain";
  drinkingFountain.add(fountainBody);
  interactive.push(fountainBody);
  const drinkingStream = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.045, 0.045, 1.15, 10),
    resources.material(THREE, {
      color: "#3aa9d0",
      roughness: 0.18,
      metalness: 0,
      transparent: true,
      opacity: 0.78,
    }, "MeshStandardMaterial"),
  );
  drinkingStream.name = "drinking-fountain-water-stream";
  drinkingStream.position.set(0.42, 3.12, 0);
  drinkingStream.rotation.z = -0.7;
  drinkingStream.visible = false;
  drinkingFountain.add(drinkingStream);
  let drinkingUntil = 0;
  animated.push((time) => {
    drinkingStream.visible = time < drinkingUntil;
    // An idle fountain leaves the stream's matrix settled.
    if (drinkingStream.visible) {
      drinkingStream.scale.y = 0.94 + Math.sin(time * 0.02) * 0.06;
    }
  });
  officeFrontGarden.add(drinkingFountain);
  setShadows(officeFrontGarden);
  world.add(officeFrontGarden);
  registerWorldElement(
    "office-garden", "Office front garden", "Scenery", officeFrontGarden,
  );
  await sceneSlice();
  registerCompactDistrictRoot("office", officeFrontGarden);

  for (const [x, z] of [
    [-20, 4],
    [0, -22],
    [20, 4],
  ]) {
    const lobbyLight = new THREE.PointLight(
      "#b8ffe3",
      4.8,
      40,
      1.7,
    );
    lobbyLight.position.set(x, OFFICE_FLOOR_HEIGHT - 2, z);
    officeInterior.add(lobbyLight);
    const fixture = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 14, 0.08, 0.32),
      officeFloorAccent,
    );
    fixture.position.set(x, OFFICE_FLOOR_HEIGHT - 0.45, z);
    officeInterior.add(fixture);
  }

  const officeGreetingBoard = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 24, 4),
    resources.uniqueMaterial(THREE, {
      map: canvasTexture(THREE, 1200, 200, (context) => {
        context.fillStyle = "#071714";
        context.fillRect(0, 0, 1200, 200);
        context.strokeStyle = "#9ef7c6";
        context.lineWidth = 10;
        context.strokeRect(6, 6, 1188, 188);
        context.fillStyle = "#eafff6";
        context.font = '800 54px "ForkMesh Mono", ui-monospace, monospace';
        context.textAlign = "center";
        context.fillText("WELCOME TO THE FORKMESH OFFICE", 600, 92);
        context.fillStyle = "#8ecdb6";
        context.font = '600 30px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText("Walk up to Noah for World and repository tips", 600, 145);
      }),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  officeGreetingBoard.name = "forkmesh-office-greeting-board";
  const greetingMount = officeHexWallPoint("back-right", 0, 0.5);
  officeGreetingBoard.position.set(greetingMount.x, 6.25, greetingMount.z);
  officeGreetingBoard.rotation.y = greetingMount.rotationY;
  officeInterior.add(officeGreetingBoard);

  const worldQaBoard = new THREE.Group();
  worldQaBoard.name = "forkmesh-world-qa-board";
  worldQaBoard.position.set(
    OFFICE_RECEPTION_DESK.x - OFFICE_RECEPTION_DESK.halfWidth - 0.5,
    3.2,
    OFFICE_RECEPTION_DESK.z + 3.5,
  );
  worldQaBoard.rotation.y = Math.atan2(
    -worldQaBoard.position.x,
    -worldQaBoard.position.z,
  );
  const qaBoardFrame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 8.2, 7.25, 0.38),
    makeSharedMaterial(THREE, "#332318", { roughness: 0.82 }),
  );
  worldQaBoard.add(qaBoardFrame);
  const qaBoardFace = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 7.8, 6.8),
    resources.uniqueMaterial(THREE, {
      map: worldQaCardTexture(THREE),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  qaBoardFace.name = "forkmesh-world-qa-board-face";
  qaBoardFace.position.z = 0.21;
  qaBoardFace.userData.interactive = "world-qa-board";
  worldQaBoard.add(qaBoardFace);
  const qaBoardBase = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 8.7, 0.3, 1.55),
    makeSharedMaterial(THREE, "#332318", { roughness: 0.86 }),
  );
  qaBoardBase.position.y = -3.76;
  worldQaBoard.add(qaBoardBase);
  for (const x of [-3.5, 3.5]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.24, 7.6, 0.24),
      makeSharedMaterial(THREE, "#332318", { roughness: 0.82 }),
    );
    post.position.set(x, -0.25, -0.1);
    worldQaBoard.add(post);
  }
  const qaSwipeCues = new THREE.Group();
  qaSwipeCues.name = "forkmesh-world-qa-swipe-cues";
  for (const cue of [
    { name: "fail", arrow: "←", label: "FAIL", color: "#ff3f46", x: -5.65, y: 0 },
    { name: "pass", arrow: "→", label: "PASS", color: "#2ea043", x: 5.65, y: 0 },
    { name: "unsure", arrow: "↓", label: "UNSURE", color: "#aeb7b2", x: 0, y: -4.82 },
  ]) {
    const arrow = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 3.15, 1.62),
      resources.uniqueMaterial(THREE, {
        map: worldQaArrowTexture(
          THREE,
          cue.arrow,
          cue.label,
          cue.color,
        ),
        transparent: true,
        depthTest: false,
        depthWrite: false,
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    arrow.name = `forkmesh-world-qa-${cue.name}-arrow`;
    arrow.position.set(cue.x, cue.y, 0.3);
    arrow.renderOrder = 48;
    qaSwipeCues.add(arrow);
  }
  qaSwipeCues.visible = true;
  worldQaBoard.add(qaSwipeCues);
  interactive.push(qaBoardFace);
  world.add(worldQaBoard);
  registerWorldElement(
    "qa-board", "QA board", "Boards & kiosks", worldQaBoard,
  );
  await sceneSlice();
  placeBillboardOnIsland(worldQaBoard, {
    circle: "hub",
    feed: "qa-board",
    cooldownMs: 5_000,
  });
  return {
    worldQaBoard,
    qaBoardFrame,
    qaBoardFace,
    qaSwipeCues,
    get drinkingUntil() {
      return drinkingUntil;
    },
    set drinkingUntil(value) {
      drinkingUntil = value;
    },
  };
}
