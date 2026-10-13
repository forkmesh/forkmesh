import { sharedGeometry, sharedMaterial } from "../world-resource-cache.js";
import { hashNumber } from "../world-avatar-face.js";
import {
  REEF_STEER_SCATTER,
  REEF_STEER_SCHOOL,
  REEF_STEER_TRACK,
  createReefSteering,
  reefColourGroup,
} from "./aquarium-schooling.js";
import {
  OUTFIT_COLORWAYS,
  OUTFIT_STYLE_IDS,
  outfitRandom,
} from "./avatar-outfits.js";
import { mergeMeshesByMaterial } from "./batching.js";
import { canvasArtTexture } from "./canvas.js";
import {
  OFFICE_AQUARIUM_POSITION,
  OFFICE_AQUARIUM_ROTATION_Y,
} from "./gameplay-constants.js";
import { makeMaterial } from "./materials.js";
import { clamp } from "./object-utils.js";
import * as resources from "./resources.js";

export function createOfficeMarineAquarium(THREE, animated) {
  const steps = officeMarineAquariumSteps(THREE, animated);
  let step = steps.next();
  while (!step.done) step = steps.next();
  return step.value;
}

export async function createOfficeMarineAquariumSliced(THREE, animated, pause) {
  const steps = officeMarineAquariumSteps(THREE, animated);
  let step = steps.next();
  while (!step.done) {
    await pause();
    step = steps.next();
  }
  return step.value;
}

export function* officeMarineAquariumSteps(THREE, animated) {
  const group = new THREE.Group();
  group.name = "forkmesh-office-marine-aquarium";
  group.position.set(...OFFICE_AQUARIUM_POSITION);
  group.rotation.y = OFFICE_AQUARIUM_ROTATION_Y;
  const tankLength = 27;
  const tankHeight = 12;
  const tankDepth = 3;
  const AQUARIUM_FEEDING_DURATION_MS = 60_000;
  const AQUARIUM_FEEDING_APPROACH_MS = 4_200;
  const AQUARIUM_FEEDING_RETURN_MS = 6_500;
  const AQUARIUM_GLASS_TAP_REACTION_MS = 2_600;
  const AQUARIUM_SCHOOL_INITIAL_DELAY_MS = 14_000;
  const AQUARIUM_SCHOOL_MIN_IDLE_MS = 24_000;
  const AQUARIUM_SCHOOL_MAX_IDLE_MS = 38_000;
  const AQUARIUM_SCHOOL_DURATION_MS = 11_000;
  const AQUARIUM_SCHOOL_TRANSITION_MS = 1_600;
  const AQUARIUM_SCHOOL_MODES = Object.freeze([
    Object.freeze({ id: "country", label: "COUNTRY" }),
    Object.freeze({ id: "browser", label: "BROWSER" }),
    // Raw user-agent strings never enter the public member directory. This
    // privacy-safe signature combines its already-public coarse browser and
    // operating-system families instead.
    Object.freeze({ id: "agent", label: "AGENT" }),
  ]);
  const AQUARIUM_ANIMATION_MS = 1000 / 20;
  const feedingCenter = new THREE.Vector3(0.68, 10.1, -1.8);
  let feedingStartedAt = -Infinity;
  let feedingActive = false;
  let reducedFeedingPoseApplied = false;
  let backdropOpaque = true;
  let aquariumLightEnabled = true;
  let glassTappedAt = -Infinity;
  let animationActive = false;
  let aquariumInView = true;
  let nextAnimationAt = 0;
  const AQUARIUM_FISH_SPECIES = Object.freeze([
    {
      id: "clownfish",
      sceneName: "forkmesh-office-aquarium-fish-clownfish",
      body: "#ed682b",
      accent: "#fff6df",
      length: 1.38,
      height: 0.52,
      thickness: 0.42,
      tail: "round",
      bands: [0.31, 0.65],
    },
    {
      id: "blue-tang",
      sceneName: "forkmesh-office-aquarium-fish-blue-tang",
      body: "#167ec5",
      accent: "#101c3b",
      length: 1.56,
      height: 0.66,
      thickness: 0.38,
      tail: "fork",
      bands: [],
    },
    {
      id: "yellow-tang",
      sceneName: "forkmesh-office-aquarium-fish-yellow-tang",
      body: "#f4d52d",
      accent: "#fff2a2",
      length: 1.44,
      height: 0.72,
      thickness: 0.34,
      tail: "fork",
      bands: [],
    },
    {
      id: "royal-gramma",
      sceneName: "forkmesh-office-aquarium-fish-royal-gramma",
      body: "#763bbd",
      accent: "#efc42b",
      length: 1.25,
      height: 0.48,
      thickness: 0.36,
      tail: "round",
      bands: [],
    },
    {
      id: "butterflyfish",
      sceneName: "forkmesh-office-aquarium-fish-butterflyfish",
      body: "#f4e8bd",
      accent: "#232a34",
      length: 1.46,
      height: 0.7,
      thickness: 0.32,
      tail: "fork",
      bands: [0.7],
    },
    {
      id: "chromis",
      sceneName: "forkmesh-office-aquarium-fish-chromis",
      body: "#5fc7d0",
      accent: "#dcffff",
      length: 0.8,
      height: 0.31,
      thickness: 0.28,
      tail: "fork",
      bands: [],
    },
  ]);

  function aquariumSeedFraction(seed, index) {
    const value =
      Math.sin((seed + 1) * 12.9898 + (index + 1) * 78.233) *
      43758.5453;
    return value - Math.floor(value);
  }

  function drawAquariumSpeciesMarkings(context, canvas, species) {
    const width = canvas.width;
    const height = canvas.height;
    if (species.id === "clownfish") {
      for (const ratio of species.bands) {
        context.fillStyle = "#211917";
        context.fillRect(width * ratio - 12, 0, 24, height);
        context.fillStyle = "#fff8e6";
        context.fillRect(width * ratio - 7, 0, 14, height);
      }
    } else if (species.id === "blue-tang") {
      context.fillStyle = "#101a3c";
      context.beginPath();
      context.moveTo(width * 0.18, height * 0.22);
      context.bezierCurveTo(
        width * 0.43,
        height * 0.01,
        width * 0.79,
        height * 0.17,
        width * 0.7,
        height * 0.5,
      );
      context.bezierCurveTo(
        width * 0.58,
        height * 0.82,
        width * 0.32,
        height * 0.85,
        width * 0.18,
        height * 0.22,
      );
      context.fill();
      context.fillStyle = "#f0cf35";
      context.fillRect(width * 0.89, 0, width * 0.11, height);
    } else if (species.id === "yellow-tang") {
      const shade = context.createLinearGradient(0, 0, 0, height);
      shade.addColorStop(0, "rgba(255,255,210,0.5)");
      shade.addColorStop(0.52, "rgba(255,255,255,0)");
      shade.addColorStop(1, "rgba(176,112,0,0.38)");
      context.fillStyle = shade;
      context.fillRect(0, 0, width, height);
      context.fillStyle = "rgba(255,250,190,0.7)";
      context.fillRect(width * 0.18, height * 0.47, width * 0.62, 2);
    } else if (species.id === "royal-gramma") {
      const split = context.createLinearGradient(0, 0, width, 0);
      split.addColorStop(0.38, "rgba(118,59,189,0)");
      split.addColorStop(0.55, "#efc42b");
      split.addColorStop(1, "#dfa51d");
      context.fillStyle = split;
      context.fillRect(0, 0, width, height);
      context.fillStyle = "rgba(255,241,167,0.8)";
      for (let index = 0; index < 10; index += 1) {
        context.fillRect(width * 0.42 + index * 6, height * 0.34, 2, 2);
      }
    } else if (species.id === "butterflyfish") {
      context.fillStyle = "#252a30";
      context.fillRect(width * 0.66, 0, width * 0.09, height);
      context.fillStyle = "#e1b62e";
      context.fillRect(width * 0.77, 0, width * 0.23, height);
      context.fillStyle = "#1a2027";
      context.beginPath();
      context.arc(width * 0.2, height * 0.5, height * 0.14, 0, Math.PI * 2);
      context.fill();
    } else {
      const shimmer = context.createLinearGradient(0, 0, width, 0);
      shimmer.addColorStop(0, "rgba(255,255,255,0)");
      shimmer.addColorStop(0.52, "rgba(229,255,255,0.62)");
      shimmer.addColorStop(1, "rgba(255,255,255,0)");
      context.fillStyle = shimmer;
      context.fillRect(0, height * 0.42, width, height * 0.16);
    }
    const scaleGlow = context.createLinearGradient(0, 0, 0, height);
    scaleGlow.addColorStop(0, "rgba(255,255,255,0.22)");
    scaleGlow.addColorStop(0.48, "rgba(255,255,255,0)");
    scaleGlow.addColorStop(1, "rgba(0,22,35,0.2)");
    context.fillStyle = scaleGlow;
    context.fillRect(0, 0, width, height);
  }

  function createAquariumFishTexture(THREE, species) {
    const texture = canvasArtTexture(THREE, 256, 128, (context, canvas) => {
      const gradient = context.createLinearGradient(0, 0, canvas.width, 0);
      gradient.addColorStop(0, species.accent);
      gradient.addColorStop(0.18, species.body);
      gradient.addColorStop(0.76, species.body);
      gradient.addColorStop(1, species.accent);
      context.fillStyle = gradient;
      context.fillRect(0, 0, canvas.width, canvas.height);
      drawAquariumSpeciesMarkings(context, canvas, species);
    });
    texture.wrapS = THREE.RepeatWrapping;
    return texture;
  }

  function createReefFishBodyGeometry(THREE, species) {
    const half = species.length / 2;
    const profile = [
      new THREE.Vector2(0.07, -half),
      new THREE.Vector2(species.height * 0.31, -half * 0.78),
      new THREE.Vector2(species.height * 0.5, -half * 0.3),
      new THREE.Vector2(species.height * 0.54, half * 0.18),
      new THREE.Vector2(species.height * 0.44, half * 0.65),
      new THREE.Vector2(species.height * 0.23, half * 0.91),
      new THREE.Vector2(0.075, half),
    ];
    const geometry = new THREE.LatheGeometry(profile, 12);
    geometry.rotateX(Math.PI / 2);
    geometry.scale(species.thickness / species.height, 1, 1);
    geometry.computeVertexNormals();
    return geometry;
  }

  function createReefFishTailGeometry(THREE, species) {
    const fork = species.tail === "fork" ? species.height * 0.14 : 0;
    const length = species.length * 0.45;
    const height = species.height * 0.74;
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute(
      "position",
      new THREE.Float32BufferAttribute([
        0, 0, 0,
        0, height, -length,
        0, fork, -length * 0.8,
        0, -height, -length,
      ], 3),
    );
    geometry.setIndex([0, 1, 2, 0, 2, 3]);
    geometry.computeVertexNormals();
    return geometry;
  }

  function createReefFishFinGeometry(THREE, width, height) {
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute(
      "position",
      new THREE.Float32BufferAttribute([
        0, 0, -width * 0.5,
        0, height, -width * 0.2,
        0, 0, width * 0.5,
        0, height * 0.72, width * 0.2,
      ], 3),
    );
    geometry.setIndex([0, 1, 3, 1, 2, 3]);
    geometry.computeVertexNormals();
    return geometry;
  }

  function createReefFishDorsalGeometry(THREE, species) {
    return createReefFishFinGeometry(
      THREE,
      species.length * 0.68,
      species.height * 0.68,
    );
  }

  function createReefFishPectoralGeometry(THREE, species) {
    return createReefFishFinGeometry(
      THREE,
      species.length * 0.3,
      species.height * 0.46,
    );
  }

  function createReefFish(THREE, species, instanceIndex) {
    const fish = new THREE.Group();
    fish.name = `${species.sceneName}-${instanceIndex}`;
    const skin = createAquariumFishTexture(THREE, species);
    const bodyMaterial = resources.uniqueMaterial(THREE, {
      color: "#ffffff",
      map: skin,
      roughness: 0.38,
      metalness: 0.02,
      clearcoat: 0.52,
      clearcoatRoughness: 0.28,
      emissive: species.body,
      emissiveIntensity: 0.035,
    }, "MeshPhysicalMaterial");
    const finMaterial = resources.uniqueMaterial(THREE, {
      color: species.body,
      transparent: true,
      opacity: 0.82,
      roughness: 0.34,
      clearcoat: 0.32,
      clearcoatRoughness: 0.2,
      side: THREE.DoubleSide,
      forceSinglePass: true,
      depthWrite: false,
    }, "MeshPhysicalMaterial");
    const body = new THREE.Mesh(
      createReefFishBodyGeometry(THREE, species),
      bodyMaterial,
    );
    body.name = `forkmesh-office-aquarium-fish-${species.id}-body`;
    fish.add(body);
    const tail = new THREE.Mesh(
      createReefFishTailGeometry(THREE, species),
      finMaterial,
    );
    tail.name = `forkmesh-office-aquarium-fish-tail-${species.id}`;
    tail.position.z = -species.length / 2 + 0.04;
    fish.add(tail);
    const dorsalFin = new THREE.Mesh(
      createReefFishDorsalGeometry(THREE, species),
      finMaterial,
    );
    dorsalFin.name = `forkmesh-office-aquarium-fish-dorsal-fin-${species.id}`;
    dorsalFin.position.y = species.height * 0.5;
    dorsalFin.position.z = -species.length * 0.06;
    fish.add(dorsalFin);
    const analFin = new THREE.Mesh(
      createReefFishFinGeometry(
        THREE,
        species.length * 0.46,
        species.height * 0.36,
      ),
      finMaterial,
    );
    analFin.name = `forkmesh-office-aquarium-fish-anal-fin-${species.id}`;
    analFin.position.set(0, -species.height * 0.46, -species.length * 0.08);
    analFin.rotation.z = Math.PI;
    fish.add(analFin);
    const pectoralFins = [];
    for (const side of [-1, 1]) {
      const pectoralFin = new THREE.Mesh(
        createReefFishPectoralGeometry(THREE, species),
        finMaterial,
      );
      pectoralFin.name =
        `forkmesh-office-aquarium-fish-pectoral-fin-${species.id}-${side}`;
      pectoralFin.position.set(
        side * species.thickness * 0.46,
        -species.height * 0.06,
        species.length * 0.16,
      );
      pectoralFin.rotation.z = side * (Math.PI / 2 + 0.22);
      pectoralFin.userData.restZ = pectoralFin.rotation.z;
      pectoralFins.push(pectoralFin);
      fish.add(pectoralFin);
    }
    const eyeWhiteMaterial = sharedMaterial(THREE, "MeshPhysicalMaterial", {
      color: "#eaf7f5",
      roughness: 0.22,
      clearcoat: 0.9,
      clearcoatRoughness: 0.08,
    });
    const irisMaterial = sharedMaterial(THREE, "MeshPhysicalMaterial", {
      color: "#071014",
      roughness: 0.14,
      clearcoat: 1,
      clearcoatRoughness: 0.04,
    });
    const highlightMaterial = sharedMaterial(THREE, "MeshBasicMaterial", {
      color: "#ffffff",
    });
    for (const side of [-1, 1]) {
      const eye = new THREE.Mesh(
        resources.primitive(THREE, "SphereGeometry", species.height * 0.105, 8, 6),
        eyeWhiteMaterial,
      );
      eye.name = `forkmesh-office-aquarium-fish-eye-${species.id}-${side}`;
      eye.position.set(
        side * species.thickness * 0.43,
        species.height * 0.1,
        species.length * 0.36,
      );
      fish.add(eye);
      const iris = new THREE.Mesh(
        resources.primitive(THREE, "SphereGeometry", species.height * 0.067, 6, 5),
        irisMaterial,
      );
      iris.name = `forkmesh-office-aquarium-fish-iris-${species.id}-${side}`;
      iris.position.set(
        side * species.thickness * 0.49,
        species.height * 0.1,
        species.length * 0.372,
      );
      fish.add(iris);
      const highlight = new THREE.Mesh(
        resources.primitive(THREE, "SphereGeometry", species.height * 0.018, 4, 3),
        highlightMaterial,
      );
      highlight.name =
        `forkmesh-office-aquarium-fish-highlight-${species.id}-${side}`;
      highlight.position.set(
        side * species.thickness * 0.535,
        species.height * 0.125,
        species.length * 0.392,
      );
      fish.add(highlight);
      const gill = new THREE.Mesh(
        resources.primitive(
          THREE,
          "TorusGeometry",
          species.height * 0.16,
          species.height * 0.018,
          4,
          10,
          Math.PI * 1.18,
        ),
        irisMaterial,
      );
      gill.name = `forkmesh-office-aquarium-fish-gill-${species.id}-${side}`;
      gill.position.set(
        side * species.thickness * 0.45,
        0,
        species.length * 0.22,
      );
      gill.rotation.y = Math.PI / 2;
      gill.rotation.x = side > 0 ? -0.58 : 0.58;
      fish.add(gill);
    }
    const mouth = new THREE.Mesh(
      resources.primitive(
        THREE,
        "TorusGeometry",
        species.height * 0.055,
        species.height * 0.016,
        4,
        8,
      ),
      irisMaterial,
    );
    mouth.name = `forkmesh-office-aquarium-fish-mouth-${species.id}`;
    mouth.position.set(0, -species.height * 0.08, species.length * 0.505);
    mouth.userData.restZ = mouth.position.z;
    fish.add(mouth);
    const lateralHighlight = new THREE.Mesh(
      sharedGeometry(THREE, "SphereGeometry", 0.5, 8, 5),
      sharedMaterial(THREE, "MeshBasicMaterial", {
        color: "#dfffff",
        transparent: true,
        opacity: 0.16,
        depthWrite: false,
      }),
    );
    lateralHighlight.name =
      `forkmesh-office-aquarium-fish-highlight-${species.id}`;
    lateralHighlight.scale.set(
      species.thickness * 0.035,
      species.height * 0.12,
      species.length * 0.56,
    );
    lateralHighlight.position.x = species.thickness * 0.52;
    fish.add(lateralHighlight);
    return { fish, tail, pectoralFins, mouth };
  }

  // Public-account fish keep an Object3D anchor for their individual routes,
  // but their visible parts are shared instanced geometry. The old cinematic
  // model used sixteen meshes and roughly a thousand triangles per account;
  // this school is 46 triangles per account in three draw submissions.
  function createInstancedUserFish(THREE, count) {
    const school = new THREE.Group();
    school.name = "forkmesh-office-aquarium-user-fish-school";
    const body = new THREE.InstancedMesh(
      resources.primitive(THREE, "SphereGeometry", 1, 6, 4),
      resources.uniqueMaterial(THREE, {
        color: "#ffffff",
        roughness: 0.48,
        metalness: 0.03,
      }, "MeshStandardMaterial"),
      count,
    );
    body.name = "forkmesh-office-aquarium-user-fish-bodies";
    const tail = new THREE.InstancedMesh(
      new THREE.BufferGeometry(),
      resources.uniqueMaterial(THREE, {
        color: "#ffffff",
        transparent: true,
        opacity: 0.9,
        roughness: 0.42,
        side: THREE.DoubleSide,
        forceSinglePass: true,
        depthWrite: false,
      }, "MeshStandardMaterial"),
      count,
    );
    tail.geometry.setAttribute(
      "position",
      new THREE.Float32BufferAttribute([
        0, 0, 0,
        0, 1, -1,
        0, -1, -1,
        0, 0, -0.78,
      ], 3),
    );
    tail.geometry.setIndex([0, 1, 3, 0, 3, 2]);
    tail.geometry.computeVertexNormals();
    tail.name = "forkmesh-office-aquarium-user-fish-tails";
    const fins = new THREE.InstancedMesh(
      createReefFishFinGeometry(THREE, 1, 1),
      resources.uniqueMaterial(THREE, {
        color: "#ffffff",
        transparent: true,
        opacity: 0.84,
        roughness: 0.46,
        side: THREE.DoubleSide,
        forceSinglePass: true,
        depthWrite: false,
      }, "MeshStandardMaterial"),
      count * 4,
    );
    fins.name = "forkmesh-office-aquarium-user-fish-fins";
    for (const mesh of [body, tail, fins]) {
      mesh.instanceMatrix.setUsage?.(THREE.DynamicDrawUsage);
      school.add(mesh);
    }
    return {
      school,
      body,
      tail,
      fins,
      matrix: new THREE.Matrix4(),
      localMatrix: new THREE.Matrix4(),
      bodyColor: new THREE.Color(),
      finColor: new THREE.Color(),
    };
  }

  function createUserFishAnchor(THREE, species, instanceIndex) {
    const fish = new THREE.Group();
    fish.name = `${species.sceneName}-${instanceIndex}`;
    const body = new THREE.Object3D();
    body.scale.set(
      species.thickness / 2,
      species.height / 2,
      species.length / 2,
    );
    const tail = new THREE.Object3D();
    tail.position.z = -species.length / 2 + 0.04;
    tail.scale.set(1, species.height * 0.74, species.length * 0.45);
    const dorsalFin = new THREE.Object3D();
    dorsalFin.position.set(0, species.height * 0.5, -species.length * 0.06);
    dorsalFin.scale.set(1, species.height * 0.68, species.length * 0.68);
    const analFin = new THREE.Object3D();
    analFin.position.set(0, -species.height * 0.46, -species.length * 0.08);
    analFin.scale.set(1, species.height * 0.36, species.length * 0.46);
    analFin.rotation.z = Math.PI;
    const pectoralFins = [];
    for (const side of [-1, 1]) {
      const pectoralFin = new THREE.Object3D();
      pectoralFin.position.set(
        side * species.thickness * 0.46,
        -species.height * 0.06,
        species.length * 0.16,
      );
      pectoralFin.scale.set(1, species.height * 0.46, species.length * 0.3);
      pectoralFin.rotation.z = side * (Math.PI / 2 + 0.22);
      pectoralFin.userData.restZ = pectoralFin.rotation.z;
      pectoralFins.push(pectoralFin);
    }
    const fins = [dorsalFin, analFin, ...pectoralFins];
    fish.add(body, tail, ...fins);
    return { fish, body, tail, pectoralFins, fins };
  }

  function aquariumCylinderBetween(
    THREE,
    start,
    end,
    startRadius,
    endRadius,
    material,
    radialSegments = 10,
  ) {
    const direction = end.clone().sub(start);
    const branch = new THREE.Mesh(
      resources.primitive(
        THREE,
        "CylinderGeometry",
        endRadius,
        startRadius,
        direction.length(),
        radialSegments,
      ),
      material,
    );
    branch.position.copy(start).add(end).multiplyScalar(0.5);
    branch.quaternion.setFromUnitVectors(
      new THREE.Vector3(0, 1, 0),
      direction.normalize(),
    );
    return branch;
  }

  function createAquariumLiveRock(THREE, material, scale, seed) {
    const rock = new THREE.Group();
    for (let index = 0; index < 5; index += 1) {
      const fraction = aquariumSeedFraction(seed, index);
      const mesh = new THREE.Mesh(
        sharedGeometry(THREE, "IcosahedronGeometry", 1, 0),
        material,
      );
      mesh.position.set(
        (fraction - 0.5) * scale * 0.72,
        index * scale * 0.18,
        (aquariumSeedFraction(seed, index + 7) - 0.5) * scale * 0.7,
      );
      mesh.scale.set(
        scale * (0.58 + fraction * 0.34),
        scale * (0.38 + fraction * 0.28),
        scale * (0.7 + fraction * 0.26),
      );
      mesh.rotation.set(
        fraction * 0.48,
        fraction * Math.PI,
        (fraction - 0.5) * 0.46,
      );
      rock.add(mesh);
    }
    return rock;
  }

  function createBranchingCoral(THREE, materials, scale, seed) {
    const coral = new THREE.Group();
    const joints = [
      [0, 0, 0],
      [0, 1.15, 0],
      [-0.06, 1.86, -0.46],
      [0.08, 2.04, 0.48],
      [-0.08, 2.48, -0.72],
      [0.06, 2.72, 0.76],
      [0.02, 2.42, 0.16],
    ];
    for (const [from, to] of [
      [0, 1],
      [1, 2],
      [1, 3],
      [1, 6],
      [2, 4],
      [3, 5],
    ]) {
      const start = new THREE.Vector3(...joints[from]).multiplyScalar(scale);
      const end = new THREE.Vector3(...joints[to]).multiplyScalar(scale);
      coral.add(
        aquariumCylinderBetween(
          THREE,
          start,
          end,
          0.16 * scale,
          0.1 * scale,
          materials.base,
          6,
        ),
      );
      if (to >= 4) {
        const tip = new THREE.Mesh(
          resources.primitive(THREE, "SphereGeometry", 0.145 * scale, 8, 6),
          materials.tip,
        );
        tip.position.copy(end);
        coral.add(tip);
      }
    }
    coral.rotation.y = aquariumSeedFraction(seed, 0) * Math.PI;
    return coral;
  }

  function createPlateCoral(THREE, materials, scale, seed) {
    const coral = new THREE.Group();
    for (let index = 0; index < 4; index += 1) {
      const plate = new THREE.Mesh(
        resources.primitive(
          THREE,
          "CylinderGeometry",
          (1.06 - index * 0.14) * scale,
          (0.9 - index * 0.12) * scale,
          0.1 * scale,
          12,
        ),
        index === 3 ? materials.tip : materials.base,
      );
      plate.position.set(
        (index - 1.5) * 0.16 * scale,
        index * 0.24 * scale,
        (aquariumSeedFraction(seed, index) - 0.5) * 0.28 * scale,
      );
      plate.rotation.set(
        (aquariumSeedFraction(seed, index + 3) - 0.5) * 0.16,
        index * 0.64,
        (index - 1.5) * 0.075,
      );
      coral.add(plate);
    }
    return coral;
  }

  function createBrainCoral(THREE, materials, scale) {
    const coral = new THREE.Group();
    const core = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", scale, 12, 8),
      materials.base,
    );
    core.scale.set(1, 0.62, 0.86);
    coral.add(core);
    for (let index = 0; index < 5; index += 1) {
      const ridge = new THREE.Mesh(
        resources.primitive(
          THREE,
          "TorusGeometry",
          scale * (0.38 + index * 0.085),
          scale * 0.035,
          4,
          12,
        ),
        materials.tip,
      );
      ridge.position.y = scale * (0.12 + index * 0.055);
      ridge.rotation.set(Math.PI / 2, index * 0.41, index * 0.2);
      coral.add(ridge);
    }
    return coral;
  }

  function createSeaFan(THREE, materials, scale, seed) {
    const fan = new THREE.Group();
    const origin = new THREE.Vector3(0, 0, 0);
    for (let index = 0; index < 8; index += 1) {
      const angle = -1.15 + index * 0.23;
      const length = 1.35 + aquariumSeedFraction(seed, index) * 0.6;
      const end = new THREE.Vector3(
        (aquariumSeedFraction(seed, index + 20) - 0.5) * 0.12,
        Math.cos(angle * 0.62) * length * scale,
        Math.sin(angle) * length * scale,
      );
      fan.add(
        aquariumCylinderBetween(
          THREE,
          origin,
          end,
          0.04 * scale,
          0.018 * scale,
          index % 2 ? materials.base : materials.tip,
          5,
        ),
      );
      const crossEnd = end.clone().multiplyScalar(0.78);
      crossEnd.y += 0.2 * scale;
      crossEnd.z += (index % 2 ? 0.18 : -0.18) * scale;
      fan.add(
        aquariumCylinderBetween(
          THREE,
          end.clone().multiplyScalar(0.38),
          crossEnd,
          0.024 * scale,
          0.012 * scale,
          materials.tip,
          4,
        ),
      );
    }
    return fan;
  }

  function createSoftCoral(THREE, materials, scale, seed) {
    const coral = new THREE.Group();
    for (let index = 0; index < 7; index += 1) {
      const length =
        (0.42 + aquariumSeedFraction(seed, index) * 0.52) * scale;
      const lobe = new THREE.Mesh(
        resources.primitive(THREE, "CapsuleGeometry", 0.12 * scale, length, 4, 6),
        index % 3 ? materials.base : materials.tip,
      );
      lobe.position.set(
        (index - 4) * 0.14 * scale,
        length * 0.52,
        (aquariumSeedFraction(seed, index + 8) - 0.5) * 0.42 * scale,
      );
      lobe.rotation.z = (index - 4) * 0.055;
      coral.add(lobe);
    }
    return coral;
  }

  function mergeSwayingParts(THREE, root, waveOf, key) {
    const vertex = new THREE.Vector3();
    const normalMatrix = new THREE.Matrix3();
    const scaleMatrix = new THREE.Matrix4();
    const byMaterial = new Map();
    root.children.slice().forEach((part, childIndex) => {
      if (!part.isMesh || Array.isArray(part.material)) return;
      let list = byMaterial.get(part.material);
      if (!list) byMaterial.set(part.material, (list = []));
      list.push({ part, wave: waveOf(part, childIndex) });
    });
    byMaterial.forEach((list, material) => {
      const positions = [];
      const normals = [];
      const locals = [];
      const pivots = [];
      const rests = [];
      const waves = [];
      const indices = [];
      let base = 0;
      for (const { part, wave } of list) {
        part.updateMatrix();
        const geometry = part.geometry;
        const position = geometry.attributes.position;
        const normal = geometry.attributes.normal;
        const local = wave
          ? scaleMatrix.makeScale(part.scale.x, part.scale.y, part.scale.z)
          : part.matrix;
        normalMatrix.getNormalMatrix(local);
        for (let index = 0; index < position.count; index += 1) {
          vertex.fromBufferAttribute(position, index).applyMatrix4(part.matrix);
          positions.push(vertex.x, vertex.y, vertex.z);
          vertex.fromBufferAttribute(position, index).applyMatrix4(local);
          locals.push(vertex.x, vertex.y, vertex.z);
          vertex.fromBufferAttribute(normal, index).applyMatrix3(normalMatrix).normalize();
          normals.push(vertex.x, vertex.y, vertex.z);
          if (wave) {
            pivots.push(part.position.x, part.position.y, part.position.z);
            rests.push(part.rotation.x, part.rotation.z);
            waves.push(wave[0], wave[1], wave[2], wave[3]);
          } else {
            pivots.push(0, 0, 0);
            rests.push(0, 0);
            waves.push(0, 0, 0, 0);
          }
        }
        const source = geometry.index;
        const count = source ? source.count : position.count;
        for (let corner = 0; corner < count; corner += 1) {
          indices.push(base + (source ? source.getX(corner) : corner));
        }
        base += position.count;
        root.remove(part);
      }
      const geometry = new THREE.BufferGeometry();
      geometry.setAttribute("position", new THREE.Float32BufferAttribute(positions, 3));
      geometry.setAttribute("normal", new THREE.Float32BufferAttribute(normals, 3));
      geometry.setAttribute("swayLocal", new THREE.Float32BufferAttribute(locals, 3));
      geometry.setAttribute("swayPivot", new THREE.Float32BufferAttribute(pivots, 3));
      geometry.setAttribute("swayRest", new THREE.Float32BufferAttribute(rests, 2));
      geometry.setAttribute("swayWave", new THREE.Float32BufferAttribute(waves, 4));
      geometry.setIndex(indices);
      geometry.computeBoundingSphere();
      geometry.boundingSphere.radius *= 1.2;
      const mesh = new THREE.Mesh(geometry, material);
      mesh.name = `${root.name || key}-sway-${byMaterial.size > 1 ? list[0].part.material.id : 0}`;
      mesh.userData.farProxySkip = true;
      root.add(mesh);
    });
  }

  function swayMaterial(material, phase) {
    material.onBeforeCompile = (shader) => {
      shader.uniforms.aquariumSwayPhase = phase;
      shader.vertexShader = shader.vertexShader
        .replace(
          "#include <common>",
          [
            "#include <common>",
            "uniform vec2 aquariumSwayPhase;",
            "attribute vec3 swayLocal;",
            "attribute vec3 swayPivot;",
            "attribute vec2 swayRest;",
            "attribute vec4 swayWave;",
          ].join("\n"),
        )
        .replace(
          "#include <beginnormal_vertex>",
          [
            "float swayX = swayRest.x + sin(aquariumSwayPhase.x + swayWave.x) * swayWave.z;",
            "float swayZ = swayRest.y + sin(aquariumSwayPhase.y + swayWave.y) * swayWave.w;",
            "mat3 aquariumSway = mat3(1.0, 0.0, 0.0, 0.0, cos(swayX), sin(swayX), 0.0, -sin(swayX), cos(swayX))",
            "  * mat3(cos(swayZ), sin(swayZ), 0.0, -sin(swayZ), cos(swayZ), 0.0, 0.0, 0.0, 1.0);",
            "vec3 objectNormal = aquariumSway * vec3( normal );",
            "#ifdef USE_TANGENT",
            "vec3 objectTangent = vec3( tangent.xyz );",
            "#endif",
          ].join("\n"),
        )
        .replace(
          "#include <begin_vertex>",
          "vec3 transformed = swayPivot + aquariumSway * swayLocal;",
        );
    };
    material.customProgramCacheKey = () => "forkmesh-aquarium-sway";
    material.needsUpdate = true;
  }

  function createAquariumAnemone(THREE, materials, scale, seed) {
    const anemone = new THREE.Group();
    const core = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.48 * scale, 12, 8),
      materials.base,
    );
    core.scale.y = 0.34;
    anemone.add(core);
    for (let index = 0; index < 16; index += 1) {
      const angle = (index / 16) * Math.PI * 2;
      const length =
        (0.52 + aquariumSeedFraction(seed, index) * 0.46) * scale;
      const tentacle = new THREE.Mesh(
        resources.primitive(THREE, "CapsuleGeometry", 0.038 * scale, length, 4, 5),
        index % 4 ? materials.base : materials.tip,
      );
      tentacle.position.set(
        Math.cos(angle) * 0.29 * scale,
        length * 0.54,
        Math.sin(angle) * 0.29 * scale,
      );
      tentacle.rotation.set(
        Math.sin(angle) * 0.34,
        0,
        Math.cos(angle) * 0.34,
      );
      anemone.add(tentacle);
    }
    return anemone;
  }

  function createSeaGrass(THREE, materials, scale, seed) {
    const grass = new THREE.Group();
    for (let index = 0; index < 10; index += 1) {
      const length =
        (0.52 + aquariumSeedFraction(seed, index) * 0.68) * scale;
      const blade = new THREE.Mesh(
        resources.primitive(THREE, "CapsuleGeometry", 0.018 * scale, length, 3, 4),
        index % 3 ? materials.base : materials.tip,
      );
      blade.position.set(
        (aquariumSeedFraction(seed, index + 10) - 0.5) * 0.76 * scale,
        length * 0.5,
        (aquariumSeedFraction(seed, index + 20) - 0.5) * 0.42 * scale,
      );
      blade.rotation.z =
        (aquariumSeedFraction(seed, index + 30) - 0.5) * 0.28;
      grass.add(blade);
    }
    return grass;
  }

  function createAquariumStarfish(THREE, material, scale) {
    const starfish = new THREE.Group();
    const center = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.13 * scale, 8, 6),
      material,
    );
    center.scale.y = 0.34;
    starfish.add(center);
    for (let index = 0; index < 5; index += 1) {
      const angle = (index / 5) * Math.PI * 2;
      starfish.add(
        aquariumCylinderBetween(
          THREE,
          new THREE.Vector3(0, 0, 0),
          new THREE.Vector3(
            Math.cos(angle) * 0.42 * scale,
            0.025 * scale,
            Math.sin(angle) * 0.42 * scale,
          ),
          0.105 * scale,
          0.025 * scale,
          material,
          5,
        ),
      );
    }
    return starfish;
  }

  function createAquariumClam(THREE, materials, scale) {
    const clam = new THREE.Group();
    for (const side of [-1, 1]) {
      const shell = new THREE.Mesh(
        resources.primitive(
          THREE,
          "SphereGeometry",
          0.32 * scale,
          10,
          6,
          0,
          Math.PI * 2,
          0,
          Math.PI / 2,
        ),
        side > 0 ? materials.base : materials.tip,
      );
      shell.scale.set(1, 0.45, 0.8);
      shell.position.x = side * 0.08 * scale;
      shell.rotation.z = side * 0.42;
      clam.add(shell);
    }
    return clam;
  }

  function createAquariumDepthTexture(THREE) {
    return canvasArtTexture(THREE, 256, 512, (context, canvas) => {
      const depth = context.createLinearGradient(0, 0, 0, canvas.height);
      depth.addColorStop(0, "#0d6783");
      depth.addColorStop(0.24, "#0a4f70");
      depth.addColorStop(0.64, "#07334f");
      depth.addColorStop(1, "#041b2d");
      context.fillStyle = depth;
      context.fillRect(0, 0, canvas.width, canvas.height);
      const glow = context.createRadialGradient(
        canvas.width * 0.55,
        canvas.height * 0.38,
        8,
        canvas.width * 0.55,
        canvas.height * 0.38,
        canvas.width * 0.72,
      );
      glow.addColorStop(0, "rgba(68,214,228,0.24)");
      glow.addColorStop(1, "rgba(4,20,36,0)");
      context.fillStyle = glow;
      context.fillRect(0, 0, canvas.width, canvas.height);
    });
  }

  function createAquariumCausticTexture(THREE) {
    const texture = canvasArtTexture(THREE, 256, 256, (context, canvas) => {
      context.clearRect(0, 0, canvas.width, canvas.height);
      context.globalCompositeOperation = "screen";
      context.filter = "blur(2px)";
      for (let index = 0; index < 13; index += 1) {
        context.strokeStyle = `rgba(178,248,255,${0.07 + (index % 4) * 0.025})`;
        context.lineWidth = 2 + (index % 3);
        context.beginPath();
        const y = 12 + index * 19;
        context.moveTo(-20, y);
        context.bezierCurveTo(55, y - 24, 92, y + 28, 150, y - 4);
        context.bezierCurveTo(190, y - 26, 222, y + 18, 280, y - 12);
        context.stroke();
      }
      context.filter = "none";
    });
    texture.wrapS = THREE.RepeatWrapping;
    texture.wrapT = THREE.RepeatWrapping;
    texture.repeat.set(1.6, 2.4);
    return texture;
  }
  const frameMaterial = makeMaterial(THREE, "#101d20", {
    metalness: 0.78,
    roughness: 0.22,
  });
  const edgeMaterial = makeMaterial(THREE, "#8fbfbe", {
    metalness: 0.72,
    roughness: 0.18,
    emissive: "#173d43",
    emissiveIntensity: 0.25,
  });
  const glassMaterial = resources.uniqueMaterial(THREE, {
    color: "#c9fbff",
    transparent: true,
    opacity: 0.12,
    metalness: 0,
    roughness: 0.06,
    clearcoat: 1,
    clearcoatRoughness: 0.04,
    depthWrite: false,
  }, "MeshPhysicalMaterial");
  yield;
  const waterMaterial = resources.uniqueMaterial(THREE, {
    color: "#137d9c",
    transparent: true,
    opacity: 0.13,
    roughness: 0.18,
    metalness: 0,
    clearcoat: 0.74,
    clearcoatRoughness: 0.14,
    emissive: "#087ba8",
    emissiveIntensity: 0.23,
    depthWrite: false,
  }, "MeshPhysicalMaterial");
  const sandMaterial = makeMaterial(THREE, "#d99d61", {
    roughness: 0.92,
    emissive: "#5f3017",
    emissiveIntensity: 0.16,
  });
  const backdropMaterial = resources.uniqueMaterial(THREE, {
    color: "#ffffff",
    map: createAquariumDepthTexture(THREE),
    roughness: 0.78,
    emissive: "#06273b",
    emissiveIntensity: 0.34,
  }, "MeshStandardMaterial");
  const bubbleMaterial = makeMaterial(THREE, "#d9ffff", {
    transparent: true,
    opacity: 0.72,
    metalness: 0.12,
    roughness: 0.08,
    emissive: "#75d9ef",
    emissiveIntensity: 0.5,
    depthWrite: false,
  });
  const frame = new THREE.Group();
  frame.name = "forkmesh-office-aquarium-frame";
  const addFramePiece = (width, height, depth, x, y, z, material) => {
    const piece = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", width, height, depth),
      material,
    );
    piece.position.set(x, y, z);
    frame.add(piece);
  };
  addFramePiece(
    tankDepth + 0.32,
    0.5,
    tankLength + 0.6,
    0,
    0.25,
    0,
    frameMaterial,
  );
  addFramePiece(
    tankDepth + 0.32,
    0.44,
    tankLength + 0.6,
    0,
    tankHeight + 0.02,
    0,
    frameMaterial,
  );
  yield;
  for (const x of [-tankDepth / 2, tankDepth / 2]) {
    for (const z of [-tankLength / 2, tankLength / 2]) {
      addFramePiece(
        0.25,
        tankHeight,
        0.25,
        x,
        tankHeight / 2,
        z,
        edgeMaterial,
      );
    }
  }
  group.add(frame);
  const backdrop = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.08, tankHeight - 0.56, tankLength - 0.48),
    backdropMaterial,
  );
  backdrop.name = "forkmesh-office-aquarium-backdrop";
  backdrop.position.set(-tankDepth / 2 + 0.08, tankHeight / 2, 0);
  group.add(backdrop);

  function setBackdropOpaque(value) {
    backdropOpaque = value !== false;
    backdropMaterial.transparent = !backdropOpaque;
    backdropMaterial.opacity = backdropOpaque ? 1 : 0.12;
    backdropMaterial.depthWrite = backdropOpaque;
    backdropMaterial.needsUpdate = true;
    return backdropOpaque;
  }
  const water = new THREE.Mesh(
    resources.primitive(
      THREE,
      "BoxGeometry",
      tankDepth - 0.3,
      tankHeight - 0.62,
      tankLength - 0.5,
    ),
    waterMaterial,
  );
  water.name = "forkmesh-office-aquarium-water";
  water.position.y = tankHeight / 2;
  group.add(water);
  const frontGlass = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.06, tankHeight - 0.48, tankLength - 0.24),
    glassMaterial,
  );
  frontGlass.name = "forkmesh-office-aquarium-front-glass";
  frontGlass.position.set(tankDepth / 2 + 0.01, tankHeight / 2, 0);
  group.add(frontGlass);
  const sand = new THREE.Group();
  sand.name = "forkmesh-office-aquarium-sand";
  const sandBed = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", tankDepth - 0.44, 0.4, tankLength - 0.8),
    sandMaterial,
  );
  sandBed.position.y = 0.55;
  sand.add(sandBed);
  const duneGeometry = resources.primitive(THREE, "DodecahedronGeometry", 1, 0);
  for (const [x, y, z, scale] of [
    [-0.35, 0.62, -8.8, 1.25],
    [0.24, 0.68, -1.6, 1.45],
    [-0.1, 0.6, 7.1, 1.08],
  ]) {
    const dune = new THREE.Mesh(duneGeometry, sandMaterial);
    dune.scale.set(0.74 * scale, 0.13 * scale, 0.96 * scale);
    dune.position.set(x, y, z);
    sand.add(dune);
  }
  yield;
  sand.userData.logoReflectionSkip = true;
  group.add(sand);
  const rubbleMaterial = makeMaterial(THREE, "#8e765d", {
    roughness: 0.98,
  });
  for (let index = 0; index < 14; index += 1) {
    const rubble = new THREE.Mesh(
      resources.primitive(THREE, "IcosahedronGeometry", 0.16 + (index % 3) * 0.04, 0),
      index % 4 === 0 ? sandMaterial : rubbleMaterial,
    );
    rubble.position.set(
      -0.8 + (index % 5) * 0.34,
      0.82 + (index % 2) * 0.05,
      -11 + index * 1.62,
    );
    rubble.scale.set(1, 0.52, 1.3);
    rubble.rotation.y = index * 1.17;
    sand.add(rubble);
  }
  mergeMeshesByMaterial(THREE, sand);
  const starfishMaterial = resources.uniqueMaterial(THREE, {
    color: "#e88442",
    roughness: 0.72,
    clearcoat: 0.12,
  }, "MeshPhysicalMaterial");
  for (const [index, x, z, scale] of [
    [0, 0.58, 8.8, 0.72],
    [1, -0.62, -0.2, 0.54],
  ]) {
    const starfish = createAquariumStarfish(THREE, starfishMaterial, scale);
    starfish.name = `forkmesh-office-aquarium-starfish-${index}`;
    starfish.position.set(x, 0.88, z);
    starfish.rotation.y = index * 1.34;
    sand.add(starfish);
  }
  const clamMaterials = {
    base: resources.uniqueMaterial(THREE, {
      color: "#b974c7",
      roughness: 0.38,
      clearcoat: 0.46,
    }, "MeshPhysicalMaterial"),
    tip: resources.uniqueMaterial(THREE, {
      color: "#75d4c5",
      roughness: 0.34,
      clearcoat: 0.52,
    }, "MeshPhysicalMaterial"),
  };
  const clam = createAquariumClam(THREE, clamMaterials, 0.76);
  clam.name = "forkmesh-office-aquarium-clam";
  clam.position.set(0.36, 0.9, 5.7);
  clam.rotation.y = -0.48;
  sand.add(clam);
  const liveRockMaterial = resources.uniqueMaterial(THREE, {
    color: "#4d3f36",
    roughness: 0.9,
    metalness: 0,
    clearcoat: 0.06,
    emissive: "#151b19",
    emissiveIntensity: 0.22,
  }, "MeshPhysicalMaterial");
  yield;
  const reef = new THREE.Group();
  reef.name = "forkmesh-office-aquarium-live-rock";
  for (const [index, x, y, z, scale] of [
    [0, -0.36, 0.96, -9.4, 1.42],
    [1, 0.2, 1.08, -6.6, 1.24],
    [2, -0.34, 0.88, -3.5, 1.18],
    [3, 0.16, 0.76, -0.4, 0.96],
    [4, -0.28, 0.72, 2.5, 0.82],
    [5, 0.18, 0.68, 5.2, 0.68],
  ]) {
    const rock = createAquariumLiveRock(
      THREE,
      liveRockMaterial,
      scale,
      index + 3,
    );
    rock.position.set(x, y, z);
    reef.add(rock);
  }
  reef.userData.logoReflectionSkip = true;
  group.add(reef);
  const coralMaterialPair = (base, tip, soft = false) => ({
    base: resources.uniqueMaterial(THREE, {
      color: base,
      roughness: soft ? 0.38 : 0.58,
      clearcoat: soft ? 0.42 : 0.14,
      clearcoatRoughness: 0.3,
      emissive: base,
      emissiveIntensity: soft ? 0.12 : 0.08,
    }, "MeshPhysicalMaterial"),
    tip: resources.uniqueMaterial(THREE, {
      color: tip,
      roughness: 0.4,
      clearcoat: 0.34,
      clearcoatRoughness: 0.2,
      emissive: tip,
      emissiveIntensity: 0.16,
    }, "MeshPhysicalMaterial"),
  });
  const staghornMaterials = coralMaterialPair("#d95791", "#ffd1e7");
  const mintStaghornMaterials = coralMaterialPair("#43bfa4", "#c8fff1");
  const plateMaterials = coralMaterialPair("#ac6be0", "#e6c8ff");
  const brainMaterials = coralMaterialPair("#d38c3b", "#ffd67a");
  const fanMaterials = coralMaterialPair("#a84da8", "#f1a9e6", true);
  const softMaterials = coralMaterialPair("#48bfae", "#b9fff0", true);
  const anemoneMaterials = coralMaterialPair("#da76bd", "#ffd8f4", true);
  const grassMaterials = coralMaterialPair("#27977f", "#8fe9c6", true);
  const coralGarden = new THREE.Group();
  coralGarden.name = "forkmesh-office-aquarium-coral-garden";
  coralGarden.userData.logoReflectionSkip = true;
  group.add(coralGarden);
  const corals = [];
  for (const [index, x, y, z, scale] of [
    [0, -0.28, 1.76, -9.4, 1.02],
    [1, 0.28, 1.64, -4.4, 0.9],
    [2, -0.18, 1.36, 2.5, 0.68],
  ]) {
    const staghorn = createBranchingCoral(
      THREE,
      index === 1 ? mintStaghornMaterials : staghornMaterials,
      scale,
      index + 8,
    );
    staghorn.name = `forkmesh-office-aquarium-staghorn-${index}`;
    staghorn.position.set(x, y, z);
    mergeMeshesByMaterial(THREE, staghorn);
    coralGarden.add(staghorn);
  }
  yield;
  for (const [index, x, y, z, scale] of [
    [0, 0.26, 1.62, -6.9, 0.92],
    [1, -0.24, 1.44, 1.1, 0.82],
  ]) {
    const plate = createPlateCoral(THREE, plateMaterials, scale, index + 14);
    plate.name = `forkmesh-office-aquarium-plate-coral-${index}`;
    plate.position.set(x, y, z);
    mergeMeshesByMaterial(THREE, plate);
    coralGarden.add(plate);
  }
  for (const [index, x, y, z, scale] of [
    [0, 0.34, 1.42, -2.7, 0.56],
    [1, -0.24, 1.18, 4.8, 0.44],
  ]) {
    const brain = createBrainCoral(THREE, brainMaterials, scale);
    brain.name = `forkmesh-office-aquarium-brain-coral-${index}`;
    brain.position.set(x, y, z);
    mergeMeshesByMaterial(THREE, brain);
    coralGarden.add(brain);
  }
  for (const [index, x, y, z, scale, phase] of [
    [0, -0.42, 1.58, -8.1, 1.12, 0.4],
    [1, -0.46, 1.34, 0.1, 0.92, 2.1],
  ]) {
    const coral = createSeaFan(THREE, fanMaterials, scale, index + 21);
    coral.name = `forkmesh-office-aquarium-sea-fan-${index}`;
    coral.position.set(x, y, z);
    mergeMeshesByMaterial(THREE, coral);
    corals.push({ coral, phase, amount: 0.045 });
    coralGarden.add(coral);
  }
  for (const [index, x, y, z, scale, phase] of [
    [0, 0.5, 1.58, -5.4, 1.04, 0.8],
    [1, -0.34, 1.38, 2.8, 0.9, 2.4],
    [2, 0.3, 1.14, 6.8, 0.72, 4.2],
  ]) {
    const coral = createSoftCoral(THREE, softMaterials, scale, index + 30);
    coral.name = `forkmesh-office-aquarium-soft-coral-${index}`;
    coral.position.set(x, y, z);
    mergeMeshesByMaterial(THREE, coral);
    corals.push({ coral, phase, amount: 0.065 });
    coralGarden.add(coral);
  }
  const anemoneSwayPhase = { value: new THREE.Vector2() };
  const grassSwayPhase = { value: new THREE.Vector2() };
  swayMaterial(anemoneMaterials.base, anemoneSwayPhase);
  swayMaterial(anemoneMaterials.tip, anemoneSwayPhase);
  swayMaterial(grassMaterials.base, grassSwayPhase);
  swayMaterial(grassMaterials.tip, grassSwayPhase);
  const anemones = [];
  for (const [index, x, y, z, scale, phase] of [
    [0, 0.38, 1.5, -1.2, 0.98, 0.4],
    [1, -0.32, 1.22, 6.1, 0.84, 2.1],
  ]) {
    const anemone = createAquariumAnemone(
      THREE,
      anemoneMaterials,
      scale,
      index + 40,
    );
    anemone.name = `forkmesh-office-aquarium-anemone-${index}`;
    anemone.position.set(x, y, z);
    mergeSwayingParts(THREE, anemone, (part, childIndex) =>
      childIndex === 0
        ? null
        : [phase + childIndex * 0.31, phase + childIndex * 0.27 + Math.PI / 2, 0.055, 0.065],
    "anemone");
    anemones.push({ anemone, phase });
    coralGarden.add(anemone);
  }
  yield;
  const seaGrasses = [];
  for (const [index, x, y, z, scale, phase] of [
    [0, -0.56, 0.88, -10.8, 0.76, 0.2],
    [1, 0.48, 0.88, 3.8, 0.68, 1.8],
    [2, -0.42, 0.88, 8.2, 0.62, 3.6],
  ]) {
    const grass = createSeaGrass(THREE, grassMaterials, scale, index + 50);
    grass.name = `forkmesh-office-aquarium-seagrass-${index}`;
    grass.position.set(x, y, z);
    mergeSwayingParts(THREE, grass, (part, childIndex) =>
      [0, phase + childIndex * 0.22, 0, 0.08],
    "seagrass");
    seaGrasses.push({ grass, phase });
    coralGarden.add(grass);
  }
  // Every public account owns one anonymous fish. The same name-seeded RNG
  // and curated colourways used by the procedural shirts determine its body,
  // accent, species, scale, speed, and route, so the school is stable on every
  // device without ever painting a person's name onto an animal.
  const userFishAnchors = new THREE.Group();
  userFishAnchors.name = "forkmesh-office-aquarium-user-fish-anchors";
  userFishAnchors.userData.logoReflectionSkip = true;
  group.add(userFishAnchors);
  const fishStates = [];
  let userFishInstances = null;
  let fishBodiesVisible = false;
  let fishPopulationKey = "";
  let visitorReaction = 0;
  let visitorReactionTarget = 0;
  let visitorApproachStart = 0;
  let visitorApproachCount = 0;
  let visitorFocusIndex = -1;
  let visitorEncounterActive = false;
  let visitorTargetY = 3;
  let visitorTargetZ = 0;
  let lastAquariumFishTime = 0;
  let aquariumSchoolModeCursor = -1;
  let aquariumSchoolSequence = 0;
  let aquariumSchoolMode = null;
  let aquariumSchoolGroups = [];
  let aquariumSchoolStartedAt = -Infinity;
  let aquariumSchoolEndsAt = -Infinity;
  let nextAquariumSchoolAt = AQUARIUM_SCHOOL_INITIAL_DELAY_MS;
  const REEF_SCHOOL_CONTROL_MODES = Object.freeze([
    "auto",
    "species",
    "colour",
    "scatter",
  ]);
  const REEF_ATTRACT_MS = 4_500;
  const REEF_ATTRACT_RADIUS = 6.5;
  const REEF_STEER_EASE_MS = 1_400;
  const reefSteering = createReefSteering(0);
  reefSteering.setBounds(
    -tankDepth / 2 + 0.42,
    tankDepth / 2 - 0.38,
    1.35,
    tankHeight - 1.3,
    -tankLength / 2 + 0.9,
    tankLength / 2 - 0.9,
  );
  yield;
  const reefSteerVector = new THREE.Vector3();
  const reefGlassTarget = new THREE.Vector3();
  const reefFollowTarget = new THREE.Vector3();
  let reefSchoolControl = "auto";
  let reefSteerBlend = 0;
  let reefSteerSeeded = false;
  let reefGlassResponse = 1;
  let reefFollowEnabled = false;
  let reefFollowTargetActive = false;
  let reefGlassAttractUntil = -Infinity;
  let reefGlassTouchedAt = -Infinity;

  function aquariumUserPalette(name) {
    const rng = outfitRandom(
      "outfit:" + String(name || "guest").trim().toLowerCase(),
    );
    // Keep these two draws in the shirt tailor's order: cut first, colourway
    // second. That makes the fish palette agree with the user's default kit.
    rng.int(OUTFIT_STYLE_IDS.length);
    const [body, accent, trim] =
      OUTFIT_COLORWAYS[rng.int(OUTFIT_COLORWAYS.length)];
    return { rng, body, accent, trim };
  }

  function disposeAquariumFish() {
    while (fishStates.length) {
      const state = fishStates.pop();
      userFishAnchors.remove(state.fish);
    }
    if (userFishInstances) {
      group.remove(userFishInstances.school);
      userFishInstances.school.traverse((child) => {
        child.geometry?.dispose?.();
        child.material?.dispose?.();
      });
      userFishInstances = null;
    }
  }

  // The Office remains a visible World landmark, but its public-member fish
  // are an indoor detail. Keep every instanced fish draw batch off the
  // renderer until the local visitor has entered the Office.
  let fishPlacementPending = false;
  function setFishBodiesVisible(value) {
    fishBodiesVisible = value === true;
    group.userData.fishBodiesVisible = fishBodiesVisible;
    if (!userFishInstances) return fishBodiesVisible;
    if (fishBodiesVisible && fishPlacementPending) placeUserFish();
    const { school, body, tail, fins } = userFishInstances;
    school.visible = fishBodiesVisible;
    body.visible = fishBodiesVisible;
    tail.visible = fishBodiesVisible;
    fins.visible = fishBodiesVisible;
    return fishBodiesVisible;
  }

  function updateInstancedUserFish() {
    if (!userFishInstances) return;
    const { body, tail, fins, matrix, localMatrix } = userFishInstances;
    for (let index = 0; index < fishStates.length; index += 1) {
      const state = fishStates[index];
      state.fish.updateMatrix();
      state.body.updateMatrix();
      matrix.multiplyMatrices(state.fish.matrix, state.body.matrix);
      body.setMatrixAt(index, matrix);
      state.tail.updateMatrix();
      matrix.multiplyMatrices(state.fish.matrix, state.tail.matrix);
      tail.setMatrixAt(index, matrix);
      for (let finIndex = 0; finIndex < state.fins.length; finIndex += 1) {
        state.fins[finIndex].updateMatrix();
        localMatrix.multiplyMatrices(
          state.fish.matrix,
          state.fins[finIndex].matrix,
        );
        fins.setMatrixAt(index * state.fins.length + finIndex, localMatrix);
      }
    }
    body.instanceMatrix.needsUpdate = true;
    tail.instanceMatrix.needsUpdate = true;
    fins.instanceMatrix.needsUpdate = true;
    body.computeBoundingSphere();
    tail.computeBoundingSphere();
    fins.computeBoundingSphere();
  }

  function aquariumSchoolValue(value, placeholders = []) {
    const text = String(value || "").trim().slice(0, 48);
    if (!text) return "";
    const lower = text.toLowerCase();
    if (
      ["hidden", "unknown", "other", "device", ...placeholders].includes(
        lower,
      )
    ) {
      return "";
    }
    return text.toUpperCase();
  }

  function clearAquariumSchoolAssignments() {
    aquariumSchoolGroups = [];
    for (const state of fishStates) {
      state.schoolGroupIndex = -1;
      state.schoolSlot = -1;
      state.schoolGroupSize = 0;
    }
  }

  function aquariumSchoolGroupsForMode(mode) {
    const grouped = new Map();
    for (const state of fishStates) {
      const key = state.schoolKeys?.[mode] || "";
      if (!key) continue;
      if (!grouped.has(key)) grouped.set(key, []);
      grouped.get(key).push(state);
    }
    return [...grouped.entries()]
      .filter(([, states]) => states.length >= 2)
      .sort(
        ([left], [right]) =>
          left.localeCompare(right),
      );
  }

  function aquariumSchoolIdleDelay() {
    const spread =
      AQUARIUM_SCHOOL_MAX_IDLE_MS - AQUARIUM_SCHOOL_MIN_IDLE_MS;
    return (
      AQUARIUM_SCHOOL_MIN_IDLE_MS +
      aquariumSeedFraction(
        fishStates.length + 181,
        aquariumSchoolSequence + 1,
      ) *
        spread
    );
  }

  function scheduleNextAquariumSchool(time, initial = false) {
    const now = Number.isFinite(Number(time)) ? Number(time) : 0;
    nextAquariumSchoolAt =
      now +
      (initial
        ? AQUARIUM_SCHOOL_INITIAL_DELAY_MS
        : aquariumSchoolIdleDelay());
  }

  function finishAquariumSchool(time, schedule = true) {
    aquariumSchoolMode = null;
    aquariumSchoolStartedAt = -Infinity;
    aquariumSchoolEndsAt = -Infinity;
    clearAquariumSchoolAssignments();
    if (schedule) scheduleNextAquariumSchool(time);
  }

  function beginAquariumSchool(time) {
    for (
      let offset = 1;
      offset <= AQUARIUM_SCHOOL_MODES.length;
      offset += 1
    ) {
      const modeIndex =
        (aquariumSchoolModeCursor + offset) %
        AQUARIUM_SCHOOL_MODES.length;
      const mode = AQUARIUM_SCHOOL_MODES[modeIndex];
      const grouped = aquariumSchoolGroupsForMode(mode.id);
      if (!grouped.length) continue;
      aquariumSchoolModeCursor = modeIndex;
      aquariumSchoolMode = mode;
      aquariumSchoolSequence += 1;
      aquariumSchoolStartedAt = time;
      aquariumSchoolEndsAt = time + AQUARIUM_SCHOOL_DURATION_MS;
      const usableLength = tankLength - 4.2;
      const laneLength = usableLength / grouped.length;
      aquariumSchoolGroups = grouped.map(([key, states], index) => {
        const phase =
          aquariumSeedFraction(
            key.length + aquariumSchoolSequence * 17,
            index + 211,
          ) *
          Math.PI *
          2;
        const groupState = {
          key,
          size: states.length,
          phase,
          centerY: 3.1 + (index % 3) * 2.05,
          centerZ: -usableLength / 2 + laneLength * (index + 0.5),
          travel: clamp(laneLength * 0.28, 0.7, 2.2),
        };
        states.forEach((state, slot) => {
          state.schoolGroupIndex = index;
          state.schoolSlot = slot;
          state.schoolGroupSize = states.length;
        });
        return groupState;
      });
      return true;
    }
    scheduleNextAquariumSchool(time);
    return false;
  }

  function updateAquariumSchool(time, animate = true) {
    if (!animate || fishStates.length < 2) return 0;
    const now = Number.isFinite(Number(time)) ? Number(time) : 0;
    if (aquariumSchoolMode && now >= aquariumSchoolEndsAt) {
      finishAquariumSchool(now);
    }
    if (
      !aquariumSchoolMode &&
      !feedingActive &&
      reefSchoolControl === "auto" &&
      now >= nextAquariumSchoolAt
    ) {
      beginAquariumSchool(now);
    }
    if (!aquariumSchoolMode || feedingActive) return 0;
    const elapsed = now - aquariumSchoolStartedAt;
    const remaining = aquariumSchoolEndsAt - now;
    return (
      aquariumSmoothstep(elapsed / AQUARIUM_SCHOOL_TRANSITION_MS) *
      aquariumSmoothstep(remaining / AQUARIUM_SCHOOL_TRANSITION_MS)
    );
  }

  function aquariumSchoolTarget(state, time) {
    const groupState =
      aquariumSchoolGroups[state.schoolGroupIndex];
    if (!groupState || state.schoolSlot < 0) return false;
    const columns = Math.max(
      2,
      Math.ceil(Math.sqrt(state.schoolGroupSize)),
    );
    const rows = Math.ceil(state.schoolGroupSize / columns);
    const column = state.schoolSlot % columns;
    const row = Math.floor(state.schoolSlot / columns);
    const elapsed = Math.max(0, time - aquariumSchoolStartedAt);
    const travelPhase = elapsed * 0.00034 + groupState.phase;
    const heading = Math.cos(travelPhase) >= 0 ? 1 : -1;
    const columnSpacing = Math.min(
      0.72,
      (groupState.travel * 1.35) / columns,
    );
    state.schoolPoint.set(
      Math.sin(elapsed * 0.00062 + groupState.phase) * 0.34 +
        (row % 2 ? 0.11 : -0.11),
      clamp(
        groupState.centerY +
          (row - (rows - 1) / 2) * 0.52 +
          Math.sin(travelPhase * 1.4) * 0.18,
        1.5,
        tankHeight - 1.35,
      ),
      clamp(
        groupState.centerZ +
          Math.sin(travelPhase) * groupState.travel +
          (column - (columns - 1) / 2) * columnSpacing,
        -tankLength / 2 + 0.8,
        tankLength / 2 - 0.8,
      ),
    );
    state.schoolTangent
      .set(
        Math.cos(elapsed * 0.00062 + groupState.phase) * 0.18,
        Math.cos(travelPhase * 1.4) * 0.035,
        heading,
      )
      .normalize();
    return true;
  }

  function getSchoolingState(time = performance.now()) {
    const now = Number.isFinite(Number(time))
      ? Number(time)
      : performance.now();
    if (reefSchoolControl !== "auto") {
      return {
        active: !feedingActive,
        control: reefSchoolControl,
        mode: reefSchoolControl,
        label: reefSchoolControl.toUpperCase(),
        groupCount:
          reefSchoolControl === "scatter" ? 0 : reefSteering.groupCount,
        startedAt: null,
        endsAt: null,
        nextAt: null,
      };
    }
    const active =
      Boolean(aquariumSchoolMode) &&
      !feedingActive &&
      now >= aquariumSchoolStartedAt &&
      now < aquariumSchoolEndsAt;
    return {
      active,
      control: reefSchoolControl,
      mode: active ? aquariumSchoolMode.id : null,
      label: active ? aquariumSchoolMode.label : "STANDBY",
      groupCount: active ? aquariumSchoolGroups.length : 0,
      startedAt: active ? aquariumSchoolStartedAt : null,
      endsAt: active ? aquariumSchoolEndsAt : null,
      nextAt: active ? null : nextAquariumSchoolAt,
    };
  }

  function setUsers(users = []) {
    const normalized = (Array.isArray(users) ? users : [])
      .map((user) => {
        const browser = aquariumSchoolValue(
          user?.browser,
          ["browser"],
        );
        const os = aquariumSchoolValue(user?.os, ["os"]);
        return {
          name: String(user?.name || "").trim().slice(0, 32),
          active: user?.away === true,
          activityBucket: String(user?.activityBucket || ""),
          countryCode: /^[A-Z]{2}$/.test(
            String(user?.countryCode || "").toUpperCase(),
          )
            ? String(user.countryCode).toUpperCase()
            : "",
          browser,
          os,
          agent: browser && os ? `${browser} / ${os}` : "",
        };
      })
      .filter((user) => user.name);
    // Every public account gets one fish. They are still ordered
    // deterministically so an account's route, colour, and scale persist
    // across reloads, but their render parts are instanced into three draws.
    const visibleUsers = [...normalized]
      .sort((left, right) => {
        const leftRecent =
          left.active ||
          ["hour", "5h", "24h", "3d", "5d", "10d"].includes(
            left.activityBucket,
          );
        const rightRecent =
          right.active ||
          ["hour", "5h", "24h", "3d", "5d", "10d"].includes(
            right.activityBucket,
          );
        return (
          Number(rightRecent) - Number(leftRecent) ||
          hashNumber(left.name) - hashNumber(right.name)
        );
      });
    group.userData.accountPopulation = normalized.length;
    group.userData.visibleFish = visibleUsers.length;
    const nextKey = JSON.stringify(visibleUsers);
    if (nextKey === fishPopulationKey) return;
    fishPopulationKey = nextKey;
    finishAquariumSchool(lastAquariumFishTime, false);
    scheduleNextAquariumSchool(lastAquariumFishTime, true);
    disposeAquariumFish();
    const population = visibleUsers.length;
    userFishInstances = createInstancedUserFish(THREE, population);
    setFishBodiesVisible(fishBodiesVisible);
    userFishInstances.school.userData.logoReflectionSkip = true;
    group.add(userFishInstances.school);
    const schoolScale = clamp(
      0.78 - Math.log2(Math.max(1, population) + 1) * 0.055,
      0.32,
      0.7,
    );
    visibleUsers.forEach((user, index) => {
      const { rng, body, accent } = aquariumUserPalette(user.name);
      const speciesIndex = rng.int(AQUARIUM_FISH_SPECIES.length);
      const template = AQUARIUM_FISH_SPECIES[speciesIndex];
      const species = {
        ...template,
        body,
        accent,
        sceneName: "forkmesh-office-aquarium-user-fish",
      };
      const { fish, body: fishBody, tail, pectoralFins, fins } =
        createUserFishAnchor(
        THREE,
        species,
        index,
      );
      const individualScale = schoolScale * rng.range(0.84, 1.12);
      fish.scale.setScalar(individualScale);
      fish.userData.activityLane = "inactive";
      // A live World presence or any non-stale server recency bucket swims
      // above the reef. Stale/unknown accounts graze the quieter bottom lane.
      const recent =
        user.active ||
        ["hour", "5h", "24h", "3d", "5d", "10d"].includes(
          user.activityBucket,
        );
      fish.userData.activityLane = recent ? "active-recent" : "inactive";
      const laneMin = recent ? 6.8 : 1.65;
      const laneHeight = recent ? 3.25 : 2.75;
      const spread = index / population;
      const points = [];
      for (let pointIndex = 0; pointIndex < 5; pointIndex += 1) {
        const phase = (spread + pointIndex / 5) * Math.PI * 2;
        points.push(
          new THREE.Vector3(
            (rng.next() - 0.5) * 1.15,
            laneMin +
              rng.next() * laneHeight +
              Math.sin(phase * 1.7) * 0.24,
            -11.4 + ((index * 4.73 + pointIndex * 5.35) % 22.8),
          ),
        );
      }
      const phase = (spread + rng.next() * 0.08) % 1;
      fishStates.push({
        fish,
        body: fishBody,
        tail,
        pectoralFins,
        fins,
        curve: new THREE.CatmullRomCurve3(
          points,
          true,
          "catmullrom",
          0.42,
        ),
        speed: 0.000018 + rng.next() * 0.000018,
        direction: rng.next() > 0.5 ? 1 : -1,
        phase,
        bank: 0.45 + rng.next() * 0.28,
        tailSpeed: 0.0036 + rng.next() * 0.0014,
        point: new THREE.Vector3(),
        tangent: new THREE.Vector3(),
        feedingPoint: new THREE.Vector3(),
        feedingAhead: new THREE.Vector3(),
        feedingTangent: new THREE.Vector3(),
        feedingRadius: 1.4 + (index % 16) * 0.48,
        feedingHeight: (index % 7 - 3) * 0.42,
        feedingPhase: phase * Math.PI * 2 + index * 0.74,
        feedingSpeed: 0.00105 + (index % 4) * 0.00009,
        feedingDelay: (index % 20) * 180,
        visitorPoint: new THREE.Vector3(),
        visitorTangent: new THREE.Vector3(),
        schoolKeys: {
          country: user.countryCode,
          browser: user.browser,
          agent: user.agent,
        },
        schoolGroupIndex: -1,
        schoolSlot: -1,
        schoolGroupSize: 0,
        schoolPoint: new THREE.Vector3(),
        schoolTangent: new THREE.Vector3(),
        speciesIndex,
        colourGroup: reefColourGroup(body),
      });
      userFishInstances.body.setColorAt(
        index,
        userFishInstances.bodyColor.set(body),
      );
      userFishInstances.tail.setColorAt(
        index,
        userFishInstances.bodyColor,
      );
      userFishInstances.finColor.set(accent);
      for (let finIndex = 0; finIndex < fins.length; finIndex += 1) {
        userFishInstances.fins.setColorAt(
          index * fins.length + finIndex,
          userFishInstances.finColor,
        );
      }
      userFishAnchors.add(fish);
    });
    if (userFishInstances.body.instanceColor) {
      userFishInstances.body.instanceColor.needsUpdate = true;
    }
    if (userFishInstances.tail.instanceColor) {
      userFishInstances.tail.instanceColor.needsUpdate = true;
    }
    if (userFishInstances.fins.instanceColor) {
      userFishInstances.fins.instanceColor.needsUpdate = true;
    }
    if (visitorReactionTarget > 0) selectVisitorFish();
    reefSteerSeeded = false;
    assignReefSteeringGroups();
    fishPlacementPending = true;
    if (fishBodiesVisible) placeUserFish();
  }
  function placeUserFish() {
    fishPlacementPending = false;
    updateAquariumFish(0, false);
  }
  const bubbles = new THREE.Group();
  bubbles.name = "forkmesh-office-aquarium-bubbles";
  const bubbleCount = 14;
  const bubbleInstances = new THREE.InstancedMesh(
    resources.primitive(THREE, "SphereGeometry", 0.1, 8, 6),
    bubbleMaterial,
    bubbleCount,
  );
  bubbleInstances.name = "forkmesh-office-aquarium-bubble-instances";
  bubbleInstances.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  bubbleInstances.boundingSphere = new THREE.Sphere(
    new THREE.Vector3(0.05, 5.9, 7.15),
    5.4,
  );
  const bubblePose = new THREE.Object3D();
  const bubbleStates = [];
  for (let index = 0; index < bubbleCount; index += 1) {
    const phase = index / bubbleCount;
    const speed = 0.000045 + (index % 4) * 0.000009;
    const scale = 0.55 + (index % 3) * 0.22;
    bubbleStates.push({
      x: -0.4 + (index % 5) * 0.22,
      z: 6.3 + (index % 4) * 0.56,
      scale,
      phase,
      speed,
    });
    bubblePose.scale.setScalar(scale);
    bubblePose.updateMatrix();
    bubbleInstances.setMatrixAt(index, bubblePose.matrix);
  }
  bubbles.add(bubbleInstances);
  bubbles.userData.logoReflectionSkip = true;
  group.add(bubbles);
  const food = new THREE.Group();
  food.name = "forkmesh-office-aquarium-food";
  food.visible = false;
  const foodGeometry = resources.primitive(THREE, "SphereGeometry", 0.075, 8, 6);
  const foodMaterial = resources.uniqueMaterial(THREE, {
    color: "#d7a14b",
    emissive: "#80521f",
    emissiveIntensity: 0.66,
    roughness: 0.82,
    metalness: 0,
    transparent: true,
    opacity: 0.96,
  }, "MeshStandardMaterial");
  const foodStates = [];
  const foodPelletCount = 28;
  const foodPellets = new THREE.InstancedMesh(foodGeometry, foodMaterial, foodPelletCount);
  foodPellets.name = "forkmesh-office-aquarium-food-pellets";
  foodPellets.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  foodPellets.boundingSphere = new THREE.Sphere(
    new THREE.Vector3(0.68, tankHeight - 4.4, -1.8),
    8,
  );
  yield;
  food.add(foodPellets);
  for (let index = 0; index < foodPelletCount; index += 1) {
    const pellet = new THREE.Object3D();
    pellet.name = `forkmesh-office-aquarium-food-pellet-${index}`;
    pellet.scale.set(
      0.7 + aquariumSeedFraction(91, index) * 0.8,
      0.65 + aquariumSeedFraction(92, index) * 0.55,
      0.7 + aquariumSeedFraction(93, index) * 0.8,
    );
    foodStates.push({
      pellet,
      x: (aquariumSeedFraction(94, index) - 0.5) * 0.82,
      z: (aquariumSeedFraction(95, index) - 0.5) * 8.4,
      drift: 3.8 + aquariumSeedFraction(96, index) * 3.1,
      phase: aquariumSeedFraction(97, index) * Math.PI * 2,
    });
    food.add(pellet);
  }
  food.userData.logoReflectionSkip = true;
  group.add(food);
  const surfaceGeometry = new THREE.PlaneGeometry(
    tankDepth - 0.42,
    tankLength - 0.72,
    8,
    42,
  );
  surfaceGeometry.rotateX(Math.PI / 2);
  const surfaceMaterial = resources.uniqueMaterial(THREE, {
    color: "#b9f7ff",
    transparent: true,
    opacity: 0.2,
    roughness: 0.08,
    metalness: 0,
    clearcoat: 1,
    clearcoatRoughness: 0.06,
    side: THREE.DoubleSide,
    forceSinglePass: true,
    depthWrite: false,
  }, "MeshPhysicalMaterial");
  const surfaceWavePhase = { value: new THREE.Vector2() };
  surfaceMaterial.onBeforeCompile = (shader) => {
    shader.uniforms.aquariumSurfaceWave = surfaceWavePhase;
    shader.vertexShader = shader.vertexShader
      .replace("#include <common>", "#include <common>\nuniform vec2 aquariumSurfaceWave;")
      .replace(
        "#include <begin_vertex>",
        "#include <begin_vertex>\ntransformed.y += sin(aquariumSurfaceWave.x + transformed.z * 0.72) * 0.035 + cos(aquariumSurfaceWave.y + transformed.x * 2.1 + transformed.z * 0.28) * 0.022;",
      );
  };
  surfaceMaterial.customProgramCacheKey = () => "forkmesh-aquarium-surface-wave";
  const waterSurface = new THREE.Mesh(surfaceGeometry, surfaceMaterial);
  waterSurface.name = "forkmesh-office-aquarium-water-surface";
  waterSurface.position.y = tankHeight - 0.48;
  waterSurface.userData.logoReflectionSkip = true;
  group.add(waterSurface);
  const particleGeometry = new THREE.BufferGeometry();
  const particlePositions = new Float32Array(54 * 3);
  for (let index = 0; index < 54; index += 1) {
    particlePositions[index * 3] =
      -1 + aquariumSeedFraction(70, index) * 2;
    particlePositions[index * 3 + 1] =
      1.2 + aquariumSeedFraction(71, index) * 9.8;
    particlePositions[index * 3 + 2] =
      -12.2 + aquariumSeedFraction(72, index) * 24.4;
  }
  yield;
  particleGeometry.setAttribute(
    "position",
    new THREE.BufferAttribute(particlePositions, 3),
  );
  const particles = new THREE.Points(
    particleGeometry,
    new THREE.PointsMaterial({
      color: "#d7fbff",
      size: 0.045,
      transparent: true,
      opacity: 0.44,
      depthWrite: false,
      blending: THREE.AdditiveBlending,
    }),
  );
  particles.name = "forkmesh-office-aquarium-particles";
  particles.userData.logoReflectionSkip = true;
  group.add(particles);
  const causticTexture = createAquariumCausticTexture(THREE);
  const causticMaterial = resources.uniqueMaterial(THREE, {
    color: "#b9f8ff",
    map: causticTexture,
    transparent: true,
    opacity: 0.18,
    side: THREE.DoubleSide,
    forceSinglePass: true,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
  }, "MeshBasicMaterial");
  const caustics = new THREE.Group();
  caustics.name = "forkmesh-office-aquarium-caustics";
  const sandCaustics = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", tankDepth - 0.5, tankLength - 0.9),
    causticMaterial,
  );
  sandCaustics.rotation.x = -Math.PI / 2;
  sandCaustics.position.y = 0.82;
  caustics.add(sandCaustics);
  const backdropCaustics = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", tankLength - 0.9, tankHeight - 0.9),
    causticMaterial.clone(),
  );
  backdropCaustics.rotation.y = Math.PI / 2;
  backdropCaustics.position.set(-tankDepth / 2 + 0.13, tankHeight / 2, 0);
  caustics.add(backdropCaustics);
  caustics.userData.logoReflectionSkip = true;
  group.add(caustics);
  const topLight = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", tankDepth - 0.32, 0.11, tankLength - 1),
    makeMaterial(THREE, "#55edff", {
      emissive: "#29d4e6",
      emissiveIntensity: 1.3,
      metalness: 0.28,
      roughness: 0.24,
    }),
  );
  topLight.name = "forkmesh-office-aquarium-top-light";
  topLight.position.y = tankHeight - 0.34;
  group.add(topLight);
  const waterLight = new THREE.PointLight("#44e8ff", 3.1, 17, 1.8);
  yield;
  waterLight.name = "forkmesh-office-aquarium-light";
  waterLight.position.set(0.45, 6.6, -1.5);
  group.add(waterLight);
  const spillLight = new THREE.PointLight("#7df0c6", 1.1, 9, 1.9);
  spillLight.name = "forkmesh-office-aquarium-spill-light";
  spillLight.position.set(1.95, 3.2, -2.5);
  group.add(spillLight);
  const sandLight = new THREE.PointLight("#ffd08a", 0.7, 8.5, 2);
  sandLight.name = "forkmesh-office-aquarium-sand-light";
  sandLight.position.set(0.2, 1.7, -2.8);
  group.add(sandLight);

  function setLightEnabled(value, time = performance.now()) {
    aquariumLightEnabled = value !== false;
    updateAquariumAtmosphere(
      Number.isFinite(Number(time)) ? Number(time) : performance.now(),
    );
    return aquariumLightEnabled;
  }

  function getControlState(time = performance.now()) {
    return {
      backdropOpaque,
      lightEnabled: aquariumLightEnabled,
      schooling: getSchoolingState(time),
      schoolMode: reefSchoolControl,
      glassResponse: reefGlassResponse > 0 ? "curious" : "shy",
      followEnabled: reefFollowEnabled,
      glassTouchedAt: Number.isFinite(reefGlassTouchedAt)
        ? reefGlassTouchedAt
        : null,
      glassTappedAt: Number.isFinite(glassTappedAt) ? glassTappedAt : null,
      steerBlend: reefSteerBlend,
    };
  }
  function aquariumSmoothstep(value) {
    const amount = clamp(value, 0, 1);
    return amount * amount * (3 - 2 * amount);
  }

  function aquariumFeedingTarget(state, elapsed, target) {
    const angle = elapsed * state.feedingSpeed + state.feedingPhase;
    target.set(
      feedingCenter.x + Math.sin(angle * 1.7) * 0.22,
      Math.min(
        tankHeight - 1.25,
        feedingCenter.y +
          state.feedingHeight +
          Math.sin(angle * 2.35 + state.feedingPhase) * 0.42,
      ),
      feedingCenter.z + Math.cos(angle) * state.feedingRadius,
    );
    return target;
  }

  function aquariumUnitProgress(value) {
    return ((Number(value) || 0) % 1 + 1) % 1;
  }

  function selectVisitorFish() {
    const count = fishStates.length;
    if (!count) {
      visitorApproachStart = 0;
      visitorApproachCount = 0;
      visitorFocusIndex = -1;
      return;
    }
    visitorApproachStart = Math.floor(Math.random() * count);
    visitorApproachCount =
      1 + Math.floor(Math.random() * Math.min(4, count));
    visitorFocusIndex =
      (visitorApproachStart +
        Math.floor(Math.random() * visitorApproachCount)) %
      count;
  }

  function aquariumFishApproachesVisitor(index) {
    if (!fishStates.length || visitorApproachCount <= 0) return false;
    const offset =
      (index - visitorApproachStart + fishStates.length) %
      fishStates.length;
    return offset < visitorApproachCount;
  }

  function assignReefSteeringGroups() {
    reefSteering.resize(fishStates.length);
    const byColour = reefSchoolControl === "colour";
    const remap = new Int8Array(16).fill(-1);
    let groups = 0;
    for (let index = 0; index < fishStates.length; index += 1) {
      const state = fishStates[index];
      const key = (byColour ? state.colourGroup : state.speciesIndex) & 15;
      if (remap[key] < 0) {
        remap[key] = groups;
        groups += 1;
      }
      reefSteering.setGroup(index, remap[key]);
    }
    reefSteering.setGroupCount(Math.max(1, groups));
    return groups;
  }

  function setSchoolMode(mode, time = performance.now()) {
    const now = Number.isFinite(Number(time)) ? Number(time) : performance.now();
    const next = REEF_SCHOOL_CONTROL_MODES.includes(mode) ? mode : "auto";
    reefSchoolControl = next;
    if (next === "auto") {
      scheduleNextAquariumSchool(now, true);
    } else {
      if (aquariumSchoolMode) finishAquariumSchool(now, false);
      assignReefSteeringGroups();
    }
    return reefSchoolControl;
  }

  function reefSteerMode() {
    if (reefSchoolControl === "scatter") return REEF_STEER_SCATTER;
    if (reefSchoolControl === "auto") return REEF_STEER_TRACK;
    return REEF_STEER_SCHOOL;
  }

  function reefClampTarget(target, point) {
    const bounds = reefSteering.bounds;
    target.set(
      clamp(Number(point?.x) || 0, bounds[0], bounds[1]),
      clamp(Number(point?.y) || 0, bounds[2], bounds[3]),
      clamp(Number(point?.z) || 0, bounds[4], bounds[5]),
    );
    return target;
  }

  function tapGlassAt(point, time = performance.now()) {
    const now = Number.isFinite(Number(time)) ? Number(time) : performance.now();
    reefClampTarget(reefGlassTarget, point);
    reefGlassAttractUntil = now + REEF_ATTRACT_MS;
    reefGlassTouchedAt = now;
    glassTappedAt = now;
    reefSteering.setTarget(
      reefGlassTarget.x,
      reefGlassTarget.y,
      reefGlassTarget.z,
      reefGlassResponse,
      REEF_ATTRACT_RADIUS,
    );
    let nearby = 0;
    for (let index = 0; index < fishStates.length; index += 1) {
      if (
        fishStates[index].fish.position.distanceTo(reefGlassTarget) <
        REEF_ATTRACT_RADIUS
      ) {
        nearby += 1;
      }
    }
    return nearby;
  }

  function setGlassResponse(value) {
    reefGlassResponse = value === "shy" || value === -1 ? -1 : 1;
    return reefGlassResponse > 0 ? "curious" : "shy";
  }

  function setFollowEnabled(value) {
    reefFollowEnabled = value === true;
    if (!reefFollowEnabled) reefFollowTargetActive = false;
    return reefFollowEnabled;
  }

  function setFollowTarget(point) {
    if (!point) {
      reefFollowTargetActive = false;
      return false;
    }
    reefClampTarget(reefFollowTarget, point);
    reefFollowTargetActive = reefFollowEnabled;
    return reefFollowTargetActive;
  }

  function updateReefSteering(time, deltaMs, animate) {
    if (!animate || !fishBodiesVisible || !fishStates.length) {
      reefSteerBlend = 0;
      return 0;
    }
    if (!reefSteerSeeded) return 0;
    const touching = time < reefGlassAttractUntil;
    const following = reefFollowEnabled && reefFollowTargetActive;
    if (touching) {
      reefSteering.setTarget(
        reefGlassTarget.x,
        reefGlassTarget.y,
        reefGlassTarget.z,
        reefGlassResponse,
        REEF_ATTRACT_RADIUS,
      );
    } else if (following) {
      reefSteering.setTarget(
        reefFollowTarget.x,
        reefFollowTarget.y,
        reefFollowTarget.z,
        reefGlassResponse,
        REEF_ATTRACT_RADIUS,
      );
    } else {
      reefSteering.clearTarget();
    }
    const wanted =
      reefSchoolControl !== "auto" || touching || following ? 1 : 0;
    const easing = deltaMs / REEF_STEER_EASE_MS;
    reefSteerBlend = clamp(
      reefSteerBlend + clamp(wanted - reefSteerBlend, -easing, easing),
      0,
      1,
    );
    if (reefSteerBlend <= 0) return 0;
    reefSteering.step(deltaMs / 1000, reefSteerMode(), time);
    return aquariumSmoothstep(reefSteerBlend);
  }

  function tapGlass(time = performance.now()) {
    const now = Number.isFinite(Number(time))
      ? Number(time)
      : performance.now();
    glassTappedAt = now;
    for (const state of fishStates) {
      const progress = aquariumUnitProgress(
        now * state.speed * state.direction + state.phase,
      );
      state.direction *= -1;
      state.phase = aquariumUnitProgress(
        progress - now * state.speed * state.direction,
      );
    }
    selectVisitorFish();
    return fishStates.length;
  }

  function updateAquariumFish(time, animate = true) {
    const motionTime = animate ? time : 0;
    let steerDeltaMs = 0;
    if (animate) {
      const elapsed = clamp(
        motionTime - lastAquariumFishTime,
        0,
        100,
      );
      const easing = 1 - Math.exp(-elapsed / 720);
      visitorReaction +=
        (visitorReactionTarget - visitorReaction) * easing;
      lastAquariumFishTime = motionTime;
      steerDeltaMs = elapsed;
    }
    const tapElapsed = Math.max(0, motionTime - glassTappedAt);
    const tapBlend =
      animate && tapElapsed < AQUARIUM_GLASS_TAP_REACTION_MS
        ? aquariumSmoothstep(
            1 - tapElapsed / AQUARIUM_GLASS_TAP_REACTION_MS,
          )
        : 0;
    const schoolBlend = updateAquariumSchool(motionTime, animate);
    const steerBlend = updateReefSteering(motionTime, steerDeltaMs, animate);
    const steerPosition = reefSteering.position;
    const steerVelocity = reefSteering.velocity;
    for (let index = 0; index < fishStates.length; index += 1) {
      const state = fishStates[index];
      const approachesVisitor =
        animate && aquariumFishApproachesVisitor(index);
      const approachBlend = approachesVisitor ? visitorReaction : 0;
      const reaction = Math.max(approachBlend, tapBlend * 0.22);
      const progress = aquariumUnitProgress(
        motionTime * state.speed * state.direction + state.phase,
      );
      state.curve.getPointAt(progress, state.point);
      state.curve
        .getPointAt(
          aquariumUnitProgress(progress + 0.002 * state.direction),
          state.tangent,
        )
        .sub(state.point)
        .normalize();
      if (
        schoolBlend > 0 &&
        aquariumSchoolTarget(state, motionTime)
      ) {
        state.point.lerp(state.schoolPoint, schoolBlend);
        state.tangent
          .lerp(state.schoolTangent, schoolBlend)
          .normalize();
      }
      const steerAt = index * 3;
      if (steerBlend > 0) {
        reefSteering.setHome(index, state.point.x, state.point.y, state.point.z);
        state.point.lerp(
          reefSteerVector.set(
            steerPosition[steerAt],
            steerPosition[steerAt + 1],
            steerPosition[steerAt + 2],
          ),
          steerBlend,
        );
        reefSteerVector.set(
          steerVelocity[steerAt],
          steerVelocity[steerAt + 1],
          steerVelocity[steerAt + 2],
        );
        if (reefSteerVector.lengthSq() > 0.0025) {
          state.tangent.lerp(reefSteerVector.normalize(), steerBlend).normalize();
        }
      } else if (steerAt < steerPosition.length) {
        reefSteering.place(
          index,
          state.point.x,
          state.point.y,
          state.point.z,
          state.tangent.x * 0.8,
          state.tangent.y * 0.8,
          state.tangent.z * 0.8,
        );
      }
      let feedingBlend = 0;
      if (feedingActive) {
        const elapsed = Math.max(0, time - feedingStartedAt);
        const feedingElapsed = animate ? elapsed : 8_500;
        const approach = aquariumSmoothstep(
          (feedingElapsed - state.feedingDelay) /
            AQUARIUM_FEEDING_APPROACH_MS,
        );
        const returnWindow = aquariumSmoothstep(
          (AQUARIUM_FEEDING_DURATION_MS - feedingElapsed) /
            AQUARIUM_FEEDING_RETURN_MS,
        );
        feedingBlend = animate ? approach * returnWindow : 1;
        aquariumFeedingTarget(
          state,
          feedingElapsed,
          state.feedingPoint,
        );
        aquariumFeedingTarget(
          state,
          feedingElapsed + 28,
          state.feedingAhead,
        );
        state.feedingTangent
          .subVectors(state.feedingAhead, state.feedingPoint)
          .normalize();
        state.tangent
          .lerp(state.feedingTangent, feedingBlend)
          .normalize();
      }
      const fish = state.fish;
      fish.position.copy(state.point);
      // Only a small, randomly selected group acknowledges a nearby visitor.
      // Their eased target stays just behind the glass and is spread around
      // eye level, avoiding the abrupt whole-school shove used previously.
      if (approachBlend > 0.001) {
        const focus = index === visitorFocusIndex;
        const encounterOffset =
          (index - visitorApproachStart + fishStates.length) %
          fishStates.length;
        state.visitorPoint.set(
          focus ? tankDepth / 2 - 0.16 : tankDepth / 2 - 0.48,
          clamp(
            visitorTargetY +
              (encounterOffset - (visitorApproachCount - 1) / 2) * 0.36,
            1.4,
            tankHeight - 1.2,
          ),
          clamp(
            visitorTargetZ +
              (encounterOffset - (visitorApproachCount - 1) / 2) * 0.62,
            -tankLength / 2 + 0.8,
            tankLength / 2 - 0.8,
          ),
        );
        state.visitorTangent
          .subVectors(state.visitorPoint, state.point)
          .normalize();
        state.tangent
          .lerp(state.visitorTangent, approachBlend * 0.78)
          .normalize();
        fish.position.lerp(
          state.visitorPoint,
          approachBlend * (focus ? 0.86 : 0.62),
        );
      }
      if (feedingBlend > 0) {
        fish.position.lerp(state.feedingPoint, feedingBlend);
      }
      if (animate) {
        fish.position.y +=
          Math.sin(motionTime * 0.0017 + state.phase * 11) *
          (0.08 + feedingBlend * 0.035);
      }
      const targetYaw = Math.atan2(state.tangent.x, state.tangent.z);
      const yawDelta = Math.atan2(
        Math.sin(targetYaw - fish.rotation.y),
        Math.cos(targetYaw - fish.rotation.y),
      );
      fish.rotation.y += yawDelta * (animate ? 0.085 : 1);
      fish.rotation.z +=
        (-state.tangent.y * state.bank - fish.rotation.z) *
        (animate ? 0.09 : 1);
      fish.rotation.x = animate
        ? Math.sin(time * 0.0011 + state.phase * 8) * 0.035
        : 0;
      state.tail.rotation.y =
        Math.sin(
          motionTime * state.tailSpeed * (1 + feedingBlend * 0.62) +
            state.phase * 9,
        ) *
        (0.38 + feedingBlend * 0.08 + reaction * 0.12);
      for (
        let finIndex = 0;
        finIndex < state.pectoralFins.length;
        finIndex += 1
      ) {
        const fin = state.pectoralFins[finIndex];
        fin.rotation.z =
          fin.userData.restZ +
          Math.sin(
            motionTime * (0.004 + feedingBlend * 0.0014) +
              state.phase * 7 +
              finIndex * Math.PI,
          ) *
            (0.12 + feedingBlend * 0.035);
      }
    }
    if (fishStates.length && reefSteering.count === fishStates.length) {
      reefSteerSeeded = true;
    }
    updateInstancedUserFish();
  }
  function updateAquariumReef(time) {
    for (let index = 0; index < corals.length; index += 1) {
      const { coral, phase, amount } = corals[index];
      coral.rotation.z = Math.sin(time * 0.00082 + phase) * amount;
      coral.rotation.x = Math.cos(time * 0.00067 + phase) * 0.036;
    }
    anemoneSwayPhase.value.set(
      (time * 0.0015) % (Math.PI * 2),
      (time * 0.0012) % (Math.PI * 2),
    );
    grassSwayPhase.value.set(0, (time * 0.0013) % (Math.PI * 2));
    for (let index = 0; index < anemones.length; index += 1) {
      const state = anemones[index];
      state.anemone.rotation.z = Math.sin(time * 0.0011 + state.phase) * 0.1;
      state.anemone.rotation.x = Math.cos(time * 0.0009 + state.phase) * 0.055;
    }
    for (let index = 0; index < seaGrasses.length; index += 1) {
      const state = seaGrasses[index];
      state.grass.rotation.z = Math.sin(time * 0.0009 + state.phase) * 0.055;
    }
  }

  function updateAquariumBubbles(time) {
    for (let index = 0; index < bubbleStates.length; index += 1) {
      const state = bubbleStates[index];
      const rise = (time * state.speed + state.phase) % 1;
      bubblePose.position.set(
        state.x + Math.sin(time * 0.0014 + state.phase) * 0.09,
        1 + rise * 9.8,
        state.z,
      );
      bubblePose.scale.setScalar(state.scale);
      bubblePose.updateMatrix();
      bubbleInstances.setMatrixAt(index, bubblePose.matrix);
    }
    bubbleInstances.instanceMatrix.needsUpdate = true;
  }

  function updateAquariumAtmosphere(time) {
    surfaceWavePhase.value.set(
      (time * 0.0014) % (Math.PI * 2),
      (time * 0.0011) % (Math.PI * 2),
    );
    particles.position.y = Math.sin(time * 0.00022) * 0.16;
    particles.position.z = Math.cos(time * 0.00017) * 0.12;
    particles.rotation.y = Math.sin(time * 0.00011) * 0.025;
    causticTexture.offset.x = (time * 0.000006) % 1;
    causticTexture.offset.y = (time * -0.000004) % 1;
    const pulse = Math.sin(time * 0.0015);
    const lightAmount = aquariumLightEnabled ? 1 : 0;
    sandCaustics.material.opacity =
      lightAmount * (0.17 + pulse * 0.025);
    backdropCaustics.material.opacity =
      lightAmount * (0.095 + pulse * 0.018);
    waterLight.intensity = lightAmount * (2.7 + pulse * 0.18);
    spillLight.intensity = lightAmount * (0.9 + pulse * 0.07);
    sandLight.intensity = lightAmount * (0.7 + pulse * 0.045);
    topLight.visible = aquariumLightEnabled;
    topLight.material.emissiveIntensity =
      lightAmount * (1.18 + pulse * 0.08);
    waterMaterial.emissiveIntensity =
      lightAmount * (0.23 + pulse * 0.025);
  }

  function updateAquariumFood(time, animate = true) {
    if (!feedingActive) {
      food.visible = false;
      return;
    }
    const elapsed = clamp(
      time - feedingStartedAt,
      0,
      AQUARIUM_FEEDING_DURATION_MS,
    );
    const progress = elapsed / AQUARIUM_FEEDING_DURATION_MS;
    const visualProgress = animate ? progress : 0.14;
    const fade =
      progress < 0.82
        ? 1
        : aquariumSmoothstep((1 - progress) / 0.18);
    feedingCenter.set(0.68, 10.1 - visualProgress * 2.2, -1.8);
    food.visible = true;
    foodMaterial.opacity = 0.96 * fade;
    for (let index = 0; index < foodStates.length; index += 1) {
      const state = foodStates[index];
      const wobble = animate
        ? Math.sin(time * 0.0012 + state.phase) * 0.1
        : Math.sin(state.phase) * 0.1;
      state.pellet.position.set(
        feedingCenter.x + state.x + wobble,
        tankHeight - 0.62 - visualProgress * state.drift - (index % 5) * 0.055,
        feedingCenter.z + state.z + wobble * 0.55,
      );
      state.pellet.rotation.set(
        state.phase + visualProgress * Math.PI * 2,
        state.phase * 0.7 + visualProgress * Math.PI * 3,
        state.phase * 0.4,
      );
      state.pellet.updateMatrix();
      foodPellets.setMatrixAt(index, state.pellet.matrix);
    }
    foodPellets.instanceMatrix.needsUpdate = true;
  }

  function updateFeeding(time, animate = true) {
    const now = Number.isFinite(Number(time)) ? Number(time) : performance.now();
    if (
      feedingActive &&
      now - feedingStartedAt >= AQUARIUM_FEEDING_DURATION_MS
    ) {
      feedingActive = false;
      feedingStartedAt = -Infinity;
      reducedFeedingPoseApplied = false;
      food.visible = false;
      foodMaterial.opacity = 0.96;
      if (!animate) updateAquariumFish(0, true);
    }
    updateAquariumFood(now, animate);
    if (!animate && feedingActive && !reducedFeedingPoseApplied) {
      updateAquariumFish(0, false);
      reducedFeedingPoseApplied = true;
    }
    return feedingActive;
  }

  function feed(time = performance.now()) {
    const now = Number.isFinite(Number(time)) ? Number(time) : performance.now();
    updateFeeding(now, true);
    if (feedingActive) return false;
    if (aquariumSchoolMode) finishAquariumSchool(now);
    feedingStartedAt = now;
    feedingActive = true;
    reducedFeedingPoseApplied = false;
    updateAquariumFood(now, true);
    return true;
  }

  function getFeedingState(time = performance.now()) {
    const now = Number.isFinite(Number(time)) ? Number(time) : performance.now();
    if (
      feedingActive &&
      now - feedingStartedAt >= AQUARIUM_FEEDING_DURATION_MS
    ) {
      updateFeeding(now, true);
    }
    const elapsedMs = feedingActive
      ? clamp(now - feedingStartedAt, 0, AQUARIUM_FEEDING_DURATION_MS)
      : 0;
    return {
      active: feedingActive,
      startedAt: feedingActive ? feedingStartedAt : null,
      endsAt: feedingActive
        ? feedingStartedAt + AQUARIUM_FEEDING_DURATION_MS
        : null,
      elapsedMs,
      remainingMs: feedingActive
        ? AQUARIUM_FEEDING_DURATION_MS - elapsedMs
        : 0,
      durationMs: AQUARIUM_FEEDING_DURATION_MS,
      foodVisible: food.visible,
      foodPosition: feedingCenter.toArray(),
    };
  }

  function initializeAquariumState(time) {
    updateAquariumFish(time);
    updateAquariumReef(time);
    updateAquariumBubbles(time);
    updateAquariumAtmosphere(time);
  }
  initializeAquariumState(0);
  animated.push((time) => {
    if (!animationActive || !aquariumInView || time < nextAnimationAt) return;
    nextAnimationAt =
      nextAnimationAt <= 0 ||
      time - nextAnimationAt > AQUARIUM_ANIMATION_MS * 2
        ? time + AQUARIUM_ANIMATION_MS
        : nextAnimationAt + AQUARIUM_ANIMATION_MS;
    initializeAquariumState(time);
  });
  group.traverse((child) => {
    if (!child.isMesh) return;
    child.castShadow = child.material?.transparent !== true;
    child.receiveShadow = child.material?.transparent !== true;
  });
  return {
    group,
    frontGlass,
    dimensions: Object.freeze({
      length: tankLength,
      height: tankHeight,
      depth: tankDepth,
    }),
    feed,
    tapGlass,
    tapGlassAt,
    setSchoolMode,
    setGlassResponse,
    setFollowEnabled,
    setFollowTarget,
    setInView(value) {
      const next = value !== false;
      if (next && !aquariumInView) nextAnimationAt = 0;
      aquariumInView = next;
      return aquariumInView;
    },
    updateFeeding,
    getFeedingState,
    getSchoolingState,
    setUsers,
    setBackdropOpaque,
    setLightEnabled,
    getControlState,
    setFishBodiesVisible,
    setAnimationActive(value) {
      const next = value === true;
      if (next && !animationActive) nextAnimationAt = 0;
      animationActive = next;
      return animationActive;
    },
    setVisitorProximity(value, target = {}) {
      const next = clamp(Number(value) || 0, 0, 1);
      visitorTargetY = clamp(
        Number(target?.y) || visitorTargetY,
        1.4,
        tankHeight - 1.2,
      );
      visitorTargetZ = clamp(
        Number(target?.z) || visitorTargetZ,
        -tankLength / 2 + 0.8,
        tankLength / 2 - 0.8,
      );
      if (next > 0.08 && !visitorEncounterActive) {
        visitorEncounterActive = true;
        selectVisitorFish();
      } else if (next <= 0.02) {
        visitorEncounterActive = false;
      }
      visitorReactionTarget = next;
    },
  };
}
