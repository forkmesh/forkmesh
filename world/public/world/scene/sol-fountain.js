import { makeMaterial, makeSharedMaterial } from "./materials.js";
import { setShadows } from "./object-utils.js";
import { createRewardTreasurySign, rewardPoolRimTexture } from "./treasury.js";
import * as resources from "./resources.js";

export function createFountain(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const nodeYardConcrete = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 1, 1, 0.14, 72),
    makeMaterial(THREE, "#b9b9b0", { roughness: 0.91 }),
  );
  nodeYardConcrete.name = "reward-node-concrete-plaza";
  nodeYardConcrete.position.y = 0.07;
  nodeYardConcrete.scale.set(14, 1, 14);
  nodeYardConcrete.receiveShadow = true;
  group.add(nodeYardConcrete);
  group.userData.nodeYardConcrete = nodeYardConcrete;
  const darkStone = makeMaterial(THREE, "#1d3b31", { roughness: 0.74 });
  const water = makeMaterial(THREE, "#48d9b1", {
    roughness: 0.2,
    metalness: 0.08,
    transparent: true,
    opacity: 0.72,
    emissive: "#20ae83",
    emissiveIntensity: 0.38,
  });
  const sunMat = makeMaterial(THREE, "#ffd66e", {
    roughness: 0.32,
    emissive: "#ffae3f",
    emissiveIntensity: 1.6,
  });

  const rimSide = resources.uniqueMaterial(THREE, {
    map: rewardPoolRimTexture(THREE),
    roughness: 0.66,
    metalness: 0.06,
  }, "MeshStandardMaterial");
  const rimCap = makeMaterial(THREE, "#1d5240", { roughness: 0.7 });
  const foundation = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 4.9, 5.25, 0.62, 48),
    [rimSide, rimCap, rimCap],
  );
  foundation.position.y = 0.31;
  group.add(foundation);
  const pool = new THREE.Mesh(resources.primitive(THREE, "CylinderGeometry", 4.4, 4.4, 0.16, 48), water);
  pool.position.y = 0.56;
  group.add(pool);
  const island = new THREE.Mesh(resources.primitive(THREE, "CylinderGeometry", 1.25, 1.65, 1.1, 28), darkStone);
  island.position.y = 1.05;
  group.add(island);
  const sun = new THREE.Mesh(resources.primitive(THREE, "IcosahedronGeometry", 1.15, 2), sunMat);
  sun.name = "reward-pool-fireball-sun";
  sun.position.y = 6.7;
  sun.userData.baseY = sun.position.y;
  group.add(sun);
  const fireShell = new THREE.Mesh(
    resources.primitive(THREE, "IcosahedronGeometry", 1.58, 2),
    resources.material(THREE, {
      color: "#ff7a1a",
      transparent: true,
      opacity: 0.22,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  fireShell.name = "reward-pool-fireball-corona";
  fireShell.position.copy(sun.position);
  group.add(fireShell);

  // Hidden until the first signed catalog lands, so the pool never flashes a
  // placeholder "0 MIRRORS" before the node payload arrives.
  const mirrorCountSprite = new THREE.Sprite(
    new THREE.SpriteMaterial({
      transparent: true,
      depthTest: false,
      depthWrite: false,
      toneMapped: false,
    }),
  );
  mirrorCountSprite.name = "reward-pool-mirror-count";
  mirrorCountSprite.position.y = 6.7;
  mirrorCountSprite.scale.set(3.6, 1.8, 1);
  mirrorCountSprite.renderOrder = 12;
  mirrorCountSprite.visible = false;
  group.add(mirrorCountSprite);
  group.userData.mirrorCountSprite = mirrorCountSprite;

  for (const radius of [1.75, 2.65, 3.55]) {
    const ring = new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", radius, 0.04, 8, 48),
      makeSharedMaterial(THREE, "#a7ffe1", {
        emissive: "#6dffd0",
        emissiveIntensity: 0.9,
      }),
    );
    ring.rotation.x = Math.PI / 2;
    ring.position.y = 0.68;
    group.add(ring);
  }

  const particleCount = 230;
  const particlePositions = new Float32Array(particleCount * 3);
  const particleSeeds = [];
  for (let index = 0; index < particleCount; index += 1) {
    const angle = Math.random() * Math.PI * 2;
    const radius = 0.24 + Math.random() * 1.95;
    const height = 4.8 + Math.random() * 3.6;
    particlePositions[index * 3] = Math.cos(angle) * radius;
    particlePositions[index * 3 + 1] = height;
    particlePositions[index * 3 + 2] = Math.sin(angle) * radius;
    particleSeeds.push({ angle, radius, speed: 0.25 + Math.random() * 0.6, height });
  }
  const particleGeometry = new THREE.BufferGeometry();
  particleGeometry.setAttribute(
    "position",
    new THREE.BufferAttribute(particlePositions, 3).setUsage(THREE.DynamicDrawUsage),
  );
  const particles = new THREE.Points(
    particleGeometry,
    new THREE.PointsMaterial({
      color: "#ffe39a",
      size: 0.12,
      transparent: true,
      opacity: 0.86,
      sizeAttenuation: true,
    }),
  );
  group.add(particles);

  const light = new THREE.PointLight("#ffd66e", 5.5, 24, 1.7);
  light.position.y = 6.8;
  group.add(light);

  const treasurySign = createRewardTreasurySign(THREE);
  // The SOL board now rises from the exact centre of the global reward-pool
  // pedestal. Its raised base clears the pedestal rim while leaving a visible
  // gap beneath the fireball and its live online/total readout.
  treasurySign.position.set(0, 1, 0);
  treasurySign.rotation.y = 0;
  group.add(treasurySign);
  group.userData.treasurySign = treasurySign;

  group.position.set(...position);
  group.userData.landmark = "fountain";
  group.traverse((child) => {
    if (child.isMesh) {
      child.userData.landmark = "fountain";
      interactive.push(child);
    }
  });
  setShadows(group);
  animated.push((time) => {
    if (!group.parent?.visible) return;
    sun.rotation.y = time * 0.00045;
    sun.rotation.x = Math.sin(time * 0.0004) * 0.12;
    sun.position.y = sun.userData.baseY + Math.sin(time * 0.0012) * 0.16;
    fireShell.position.copy(sun.position);
    fireShell.rotation.y = -time * 0.00075;
    fireShell.rotation.z = time * 0.00052;
    const coronaPulse = 0.94 + Math.sin(time * 0.006) * 0.12;
    fireShell.scale.setScalar(coronaPulse);
    particleTime = time;
  });
  let particleTime = 0;
  let particleTimeWritten = -1;
  particles.onBeforeRender = () => {
    if (particleTime === particleTimeWritten) return;
    particleTimeWritten = particleTime;
    const time = particleTime;
    const positions = particles.geometry.attributes.position.array;
    for (let index = 0; index < particleCount; index += 1) {
      const seed = particleSeeds[index];
      const phase = time * 0.00035 * seed.speed;
      const pulse = 0.78 + Math.sin(phase * 2.3 + index) * 0.18;
      positions[index * 3] = Math.cos(seed.angle + phase) * seed.radius * pulse;
      positions[index * 3 + 1] =
        4.75 + ((seed.height + time * 0.00082 * seed.speed) % 3.8);
      positions[index * 3 + 2] = Math.sin(seed.angle + phase) * seed.radius * pulse;
    }
    particles.geometry.attributes.position.needsUpdate = true;
  };
  return group;
}
