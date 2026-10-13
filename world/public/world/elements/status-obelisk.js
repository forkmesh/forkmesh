// Status obelisk — an obelisk whose face reads a live ForkMesh endpoint.
// This is the reference example of an element linked to a network request:
// every endpoint it may read is declared in `manifest.network`, and the store
// discloses that list before purchase.

export const manifest = {
  id: "status-obelisk",
  label: "Status obelisk",
  category: "Store",
  version: 1,
  summary:
    "A standing obelisk that shows the live deploy state of the instance.",
  params: [
    {
      key: "caption",
      type: "text",
      label: "Caption",
      default: "Deploy status",
      maxLength: 40,
    },
    {
      key: "refreshSeconds",
      type: "number",
      label: "Refresh (seconds)",
      default: 60,
      min: 15,
      max: 600,
      step: 5,
    },
    {
      key: "accent",
      type: "select",
      label: "Accent",
      default: "green",
      options: ["green", "amber", "blue"],
    },
  ],
  network: ["/api/world/deploy-status"],
};

const ACCENTS = {
  green: "#4ade80",
  amber: "#fbbf24",
  blue: "#60a5fa",
};

const STATE_LABELS = {
  idle: "Idle",
  deploying: "Deploying",
  ready: "Ready",
  failed: "Failed",
};

export function build({ THREE, params, net }) {
  const accent = ACCENTS[params.accent] || ACCENTS.green;
  const group = new THREE.Group();

  const canvas = document.createElement("canvas");
  canvas.width = 256;
  canvas.height = 256;
  const context = canvas.getContext("2d");
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;

  let state = "idle";
  let revision = "";

  function paint() {
    context.fillStyle = "#0f172a";
    context.fillRect(0, 0, 256, 256);
    context.fillStyle = accent;
    context.fillRect(0, 0, 256, 6);
    context.fillStyle = "#e2e8f0";
    context.font = "600 22px system-ui, sans-serif";
    context.textAlign = "center";
    context.fillText(params.caption || "Deploy status", 128, 60);
    context.fillStyle = accent;
    context.font = "700 34px system-ui, sans-serif";
    context.fillText(STATE_LABELS[state] || "Idle", 128, 130);
    if (revision) {
      context.fillStyle = "#94a3b8";
      context.font = "18px ui-monospace, monospace";
      context.fillText(revision.slice(0, 12), 128, 180);
    }
    texture.needsUpdate = true;
  }

  const shaft = new THREE.Mesh(
    new THREE.BoxGeometry(1.1, 4.4, 1.1),
    new THREE.MeshStandardMaterial({ color: 0x1e293b, roughness: 0.65 }),
  );
  shaft.position.y = 2.2;
  shaft.castShadow = true;
  shaft.receiveShadow = true;
  group.add(shaft);

  const cap = new THREE.Mesh(
    new THREE.ConeGeometry(0.8, 0.9, 4),
    new THREE.MeshStandardMaterial({
      color: accent,
      emissive: accent,
      emissiveIntensity: 0.35,
      roughness: 0.4,
    }),
  );
  cap.position.y = 4.85;
  cap.rotation.y = Math.PI / 4;
  cap.castShadow = true;
  group.add(cap);

  const face = new THREE.Mesh(
    new THREE.PlaneGeometry(0.95, 0.95),
    new THREE.MeshBasicMaterial({ map: texture }),
  );
  face.position.set(0, 2.9, 0.56);
  group.add(face);

  paint();

  let nextReadAt = 0;
  let reading = false;

  async function refresh() {
    if (reading) return;
    reading = true;
    try {
      const payload = await net("/api/world/deploy-status");
      state = String(payload?.state || "idle");
      revision = String(payload?.revision || "");
      paint();
    } catch {
      // A refused or failed read leaves the last painted state in place;
      // an element never breaks the frame loop.
    } finally {
      reading = false;
    }
  }

  return {
    group,
    tick(nowMs) {
      if (nowMs < nextReadAt) return;
      nextReadAt = nowMs + params.refreshSeconds * 1000;
      refresh();
    },
    dispose() {
      texture.dispose();
      group.traverse((child) => {
        child.geometry?.dispose?.();
        child.material?.dispose?.();
      });
    },
  };
}
