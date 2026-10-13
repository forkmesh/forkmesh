// Aurora beacon — a lit column with slow ribbons of light above it.
// Self-contained: geometry, materials and animation all live here.

export const manifest = {
  id: "aurora-beacon",
  label: "Aurora beacon",
  category: "Store",
  version: 1,
  summary:
    "A quiet column of light with drifting aurora ribbons above the plaza.",
  params: [
    {
      key: "height",
      type: "number",
      label: "Height",
      default: 8,
      min: 4,
      max: 14,
      step: 0.5,
    },
    {
      key: "palette",
      type: "select",
      label: "Palette",
      default: "aurora",
      options: ["aurora", "ember", "ice"],
    },
    {
      key: "ribbons",
      type: "number",
      label: "Ribbons",
      default: 3,
      min: 1,
      max: 6,
      step: 1,
    },
    { key: "drift", type: "toggle", label: "Drifting motion", default: true },
  ],
  network: [],
};

const PALETTES = {
  aurora: { core: 0x7ce7c8, ribbon: 0x53c8ff, base: 0x1d3b4d },
  ember: { core: 0xffb066, ribbon: 0xff6b4a, base: 0x40241c },
  ice: { core: 0xdff3ff, ribbon: 0x9bd0ff, base: 0x2b3a4a },
};

export function build({ THREE, params }) {
  const palette = PALETTES[params.palette] || PALETTES.aurora;
  const height = params.height;
  const group = new THREE.Group();

  const base = new THREE.Mesh(
    new THREE.CylinderGeometry(0.9, 1.15, 0.5, 18),
    new THREE.MeshStandardMaterial({
      color: palette.base,
      roughness: 0.75,
      metalness: 0.1,
    }),
  );
  base.position.y = 0.25;
  base.castShadow = true;
  base.receiveShadow = true;
  group.add(base);

  const column = new THREE.Mesh(
    new THREE.CylinderGeometry(0.22, 0.32, height, 14),
    new THREE.MeshStandardMaterial({
      color: palette.core,
      emissive: palette.core,
      emissiveIntensity: 0.55,
      roughness: 0.35,
      // Single-pass: the world pays twice for transparent double-sided
      // materials, so the column stays opaque and only ribbons blend.
      metalness: 0.05,
    }),
  );
  column.position.y = 0.5 + height / 2;
  column.castShadow = true;
  group.add(column);

  const light = new THREE.PointLight(palette.core, 1.1, 14, 2);
  light.position.y = 0.5 + height;
  group.add(light);

  const ribbons = [];
  for (let index = 0; index < params.ribbons; index += 1) {
    const ribbon = new THREE.Mesh(
      new THREE.TorusGeometry(1.4 + index * 0.45, 0.05, 6, 40, Math.PI * 1.2),
      new THREE.MeshBasicMaterial({
        color: palette.ribbon,
        transparent: true,
        opacity: 0.5,
        forceSinglePass: true,
      }),
    );
    ribbon.rotation.x = Math.PI / 2 + index * 0.12;
    ribbon.position.y = 0.5 + height - 0.6 - index * 0.55;
    group.add(ribbon);
    ribbons.push({ mesh: ribbon, speed: 0.14 + index * 0.05 });
  }

  return {
    group,
    tick(nowMs) {
      if (!params.drift) return;
      const seconds = nowMs / 1000;
      ribbons.forEach((entry, index) => {
        entry.mesh.rotation.z = seconds * entry.speed + index;
      });
      light.intensity = 0.9 + Math.sin(seconds * 0.9) * 0.25;
    },
    dispose() {
      group.traverse((child) => {
        child.geometry?.dispose?.();
        child.material?.dispose?.();
      });
    },
  };
}
