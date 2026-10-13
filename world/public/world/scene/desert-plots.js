import {
  CONTINUOUS_CITY_CORNER_RADIUS,
  CONTINUOUS_CITY_RADIUS,
  DESERT_HEXES,
  DESERT_PLOTS,
  DESERT_PLOT_APOTHEM,
  DESERT_PLOT_SURFACE_Y,
  WORLD_ROAD_CORNER_SEGMENTS,
} from "./layout.js";
import {
  cloneSharedMesh,
  cloneSharedPlane,
  makeMaterial,
  makeSharedMaterial,
} from "./materials.js";
import { setShadows } from "./object-utils.js";
import {
  DESERT_SAND_TILE,
  desertSandTexture,
  roundedHexagonGeometry,
  startWorldSignTexture,
} from "./town.js";
import * as resources from "./resources.js";

export function createDesertPlots(THREE, interactive) {
  const group = new THREE.Group();
  group.name = "forkmesh-desert-plots";
  const sandTexture = desertSandTexture(THREE);
  const sand = new THREE.Mesh(
    roundedHexagonGeometry(
      THREE,
      CONTINUOUS_CITY_RADIUS,
      CONTINUOUS_CITY_CORNER_RADIUS,
      WORLD_ROAD_CORNER_SEGMENTS,
      {
        centers: DESERT_HEXES.map((hex) => [hex.x, -hex.z]),
        uvScale: 1 / DESERT_SAND_TILE,
      },
    ),
    resources.uniqueMaterial(THREE, {
      color: "#f2dfb8",
      map: sandTexture,
      roughness: 1,
      metalness: 0,
    }, "MeshStandardMaterial"),
  );
  sand.name = "forkmesh-desert-plot-sand";
  sand.rotation.x = -Math.PI / 2;
  sand.position.y = DESERT_PLOT_SURFACE_Y;
  sand.receiveShadow = true;
  sand.userData.ground = true;
  group.add(sand);

  const cactusSpots = [
    [0.18, 0.42],
    [0.46, -0.35],
    [0.62, 0.12],
  ];
  const openDesertSpots = [
    [0.35, 0.4],
    [0.55, -0.9],
    [0.7, 1.6],
    [0.45, 2.3],
    [0.8, -2.1],
    [0.62, 0],
    [0.25, -1.5],
    [0.85, 0.9],
  ];
  const cactusPlacements = [];
  DESERT_PLOTS.forEach((plot) => {
    const tangentX = -plot.normalZ;
    const tangentZ = plot.normalX;
    cactusSpots.forEach(([outward, sideways]) => {
      cactusPlacements.push([
        plot.x +
          plot.normalX * outward * DESERT_PLOT_APOTHEM +
          tangentX * sideways * DESERT_PLOT_APOTHEM,
        plot.z +
          plot.normalZ * outward * DESERT_PLOT_APOTHEM +
          tangentZ * sideways * DESERT_PLOT_APOTHEM,
      ]);
    });
  });
  DESERT_HEXES.forEach((hex) => {
    const facing = Math.atan2(hex.normalZ, hex.normalX);
    openDesertSpots.forEach(([reach, turn]) => {
      cactusPlacements.push([
        hex.x + Math.cos(facing + turn) * reach * CONTINUOUS_CITY_RADIUS,
        hex.z + Math.sin(facing + turn) * reach * CONTINUOUS_CITY_RADIUS,
      ]);
    });
  });
  const cactusMaterial = makeMaterial(THREE, "#4f8a4a", { roughness: 0.9 });
  const trunks = new THREE.InstancedMesh(
    resources.primitive(THREE, "CylinderGeometry", 0.9, 1.05, 7, 10),
    cactusMaterial,
    cactusPlacements.length,
  );
  trunks.name = "forkmesh-desert-cacti";
  const arms = new THREE.InstancedMesh(
    resources.primitive(THREE, "CylinderGeometry", 0.55, 0.6, 3.2, 8),
    cactusMaterial,
    cactusPlacements.length * 2,
  );
  arms.name = "forkmesh-desert-cactus-arms";
  const transform = new THREE.Object3D();
  cactusPlacements.forEach(([x, z], cactus) => {
    const scale = 0.8 + ((cactus * 37 + (cactus % 3) * 11) % 5) * 0.12;
    const heading = (cactus * 1.7) % (Math.PI * 2);
    transform.position.set(x, DESERT_PLOT_SURFACE_Y + 3.5 * scale, z);
    transform.rotation.set(0, heading, 0);
    transform.scale.setScalar(scale);
    transform.updateMatrix();
    trunks.setMatrixAt(cactus, transform.matrix);
    [-1, 1].forEach((side, armIndex) => {
      transform.position.set(
        x + Math.cos(heading) * side * 1.6 * scale,
        DESERT_PLOT_SURFACE_Y + (4.2 + armIndex * 1.1) * scale,
        z - Math.sin(heading) * side * 1.6 * scale,
      );
      transform.rotation.set(0, heading, 0);
      transform.updateMatrix();
      arms.setMatrixAt(cactus * 2 + armIndex, transform.matrix);
    });
  });
  trunks.instanceMatrix.needsUpdate = true;
  arms.instanceMatrix.needsUpdate = true;
  trunks.computeBoundingSphere();
  arms.computeBoundingSphere();
  trunks.castShadow = true;
  arms.castShadow = true;
  group.add(trunks, arms);

  const postMaterial = makeSharedMaterial(THREE, "#6b4424", { roughness: 0.92 });
  const boardMaterial = makeSharedMaterial(THREE, "#5a3a1e", { roughness: 0.9 });
  const faceMaterial = resources.uniqueMaterial(THREE, {
    map: startWorldSignTexture(THREE),
    toneMapped: false,
  }, "MeshBasicMaterial");
  const signPlots = DESERT_HEXES.map((_, side) =>
    DESERT_PLOTS.find((plot) => plot.hex === side),
  ).filter(Boolean);
  signPlots.forEach((plot, index) => {
    const sign = new THREE.Group();
    sign.name = `forkmesh-start-world-sign-${index + 1}`;
    const inset = DESERT_PLOT_APOTHEM - 14;
    sign.position.set(
      plot.x - plot.normalX * inset,
      DESERT_PLOT_SURFACE_Y,
      plot.z - plot.normalZ * inset,
    );
    sign.rotation.y = Math.atan2(-plot.normalX, -plot.normalZ);
    [-6.4, 6.4].forEach((offset) => {
      const post = cloneSharedMesh(
        THREE,
        "box:0.7:11:0.7",
        () => resources.primitive(THREE, "BoxGeometry", 0.7, 11, 0.7),
        postMaterial,
      );
      post.position.set(offset, 5.5, -0.4);
      sign.add(post);
    });
    const board = cloneSharedMesh(
      THREE,
      "box:16:8:0.4",
      () => resources.primitive(THREE, "BoxGeometry", 16, 8, 0.4),
      boardMaterial,
    );
    board.name = "forkmesh-start-world-sign-board";
    board.position.set(0, 7, 0);
    board.userData.interactive = "start-world-sign";
    board.userData.plot = index + 1;
    sign.add(board);
    const face = cloneSharedPlane(THREE, 15.4, 7.7, faceMaterial);
    face.name = "forkmesh-start-world-sign-face";
    face.position.set(0, 7, 0.21);
    face.userData.interactive = "start-world-sign";
    face.userData.plot = index + 1;
    sign.add(face);
    interactive.push(board, face);
    setShadows(sign, true, false);
    group.add(sign);
  });
  return group;
}
