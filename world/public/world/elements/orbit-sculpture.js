// Orbit sculpture — a core with rings turning around it at their own rates.

export const manifest = {
  id: "orbit-sculpture",
  label: "Orbit sculpture",
  category: "Store",
  version: 1,
  summary: "A slow kinetic sculpture of nested rings around a lit core.",
  params: [
    {
      key: "rings",
      type: "number",
      label: "Rings",
      default: 3,
      min: 1,
      max: 5,
      step: 1,
    },
    {
      key: "radius",
      type: "number",
      label: "Radius",
      default: 2.2,
      min: 1,
      max: 5,
      step: 0.1,
    },
    {
      key: "tint",
      type: "select",
      label: "Tint",
      default: "brass",
      options: ["brass", "slate", "violet"],
    },
    {
      key: "speed",
      type: "number",
      label: "Turn speed",
      default: 1,
      min: 0,
      max: 3,
      step: 0.1,
    },
  ],
  network: [],
};

const TINTS = {
  brass: { ring: 0xd9a441, core: 0xffe6ad },
  slate: { ring: 0x8fa3b8, core: 0xdfe9f5 },
  violet: { ring: 0xa07cd4, core: 0xe6d8ff },
};

export function build({ THREE, params }) {
  const tint = TINTS[params.tint] || TINTS.brass;
  const group = new THREE.Group();

  const plinth = new THREE.Mesh(
    new THREE.CylinderGeometry(0.7, 0.85, 0.6, 16),
    new THREE.MeshStandardMaterial({ color: 0x3b4048, roughness: 0.8 }),
  );
  plinth.position.y = 0.3;
  plinth.castShadow = true;
  plinth.receiveShadow = true;
  group.add(plinth);

  const centreY = 0.6 + params.radius;
  const core = new THREE.Mesh(
    new THREE.IcosahedronGeometry(0.42, 1),
    new THREE.MeshStandardMaterial({
      color: tint.core,
      emissive: tint.core,
      emissiveIntensity: 0.45,
      roughness: 0.3,
    }),
  );
  core.position.y = centreY;
  core.castShadow = true;
  group.add(core);

  const rings = [];
  for (let index = 0; index < params.rings; index += 1) {
    const ring = new THREE.Mesh(
      new THREE.TorusGeometry(
        params.radius * (0.45 + index * 0.22),
        0.06,
        8,
        48,
      ),
      new THREE.MeshStandardMaterial({
        color: tint.ring,
        roughness: 0.4,
        metalness: 0.55,
      }),
    );
    ring.position.y = centreY;
    ring.rotation.x = (Math.PI / 3) * index;
    ring.rotation.y = (Math.PI / 5) * index;
    ring.castShadow = true;
    group.add(ring);
    rings.push({
      mesh: ring,
      rate: (0.12 + index * 0.07) * (index % 2 === 0 ? 1 : -1),
      tilt: (Math.PI / 3) * index,
    });
  }

  return {
    group,
    tick(nowMs) {
      if (params.speed <= 0) return;
      const seconds = (nowMs / 1000) * params.speed;
      rings.forEach((entry) => {
        entry.mesh.rotation.z = seconds * entry.rate;
        entry.mesh.rotation.x = entry.tilt + Math.sin(seconds * 0.2) * 0.2;
      });
      core.rotation.y = seconds * 0.3;
    },
    dispose() {
      group.traverse((child) => {
        child.geometry?.dispose?.();
        child.material?.dispose?.();
      });
    },
  };
}
