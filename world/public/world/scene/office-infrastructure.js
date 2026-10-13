import { addSectionPlaque } from "./arrival.js";
import { canvasTexture } from "./canvas.js";
import { makeSharedMaterial } from "./materials.js";
import { setShadows } from "./object-utils.js";
import * as resources from "./resources.js";

export function createSystemCapacityPlatform(THREE) {
  const district = new THREE.Group();
  district.name = "system-capacity-infrastructure";
  district.userData.infrastructureKind = "system-capacity";
  district.userData.metricsAvailable = false;
  const base = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 13.5, 0.36, 9.5),
    makeSharedMaterial(THREE, "#102d32", {
      emissive: "#164a55",
      emissiveIntensity: 0.25,
      roughness: 0.62,
    }),
  );
  base.position.y = 0.18;
  district.add(base);
  addSectionPlaque(
    THREE,
    district,
    [0, 0, 0],
    "SYSTEM CAPACITY",
    "durable objects + database rows",
    "#80e8ff",
    6,
  );
  const emptyMarker = new THREE.Mesh(
    resources.primitive(THREE, "TorusGeometry", 1.2, 0.08, 8, 36),
    makeSharedMaterial(THREE, "#718087", {
      emissive: "#2d3f44",
      emissiveIntensity: 0.2,
      transparent: true,
      opacity: 0.75,
    }),
  );
  emptyMarker.name = "system-capacity-metrics-unavailable";
  emptyMarker.position.y = 1.4;
  emptyMarker.rotation.x = Math.PI / 2;
  district.add(emptyMarker);
  setShadows(district);
  return district;
}

export function drawEngineeringDebug(context, snapshot = {}) {
  const metrics = Array.isArray(snapshot.metrics) ? snapshot.metrics : [];
  const colorFor = (status) =>
    status === "critical"
      ? "#ff7189"
      : status === "warning"
        ? "#f7c96b"
        : "#4bbf73";
  {
    context.fillStyle = "#06110e";
    context.fillRect(0, 0, 1600, 1000);
    context.strokeStyle = colorFor(snapshot.overall);
    context.lineWidth = 16;
    context.strokeRect(10, 10, 1580, 980);
    context.fillStyle = "#effff6";
    context.font = '800 60px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("ENGINEERING · LIVE DEBUG CONTROL", 52, 78);
    context.fillStyle = "#80bda0";
    context.font = '650 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      "LOCAL RENDERER + LOOP TELEMETRY · GREEN HEALTHY · ORANGE OPTIMIZE · RED ACT",
      54,
      126,
    );
    context.strokeStyle = "#21443a";
    context.lineWidth = 3;
    context.beginPath();
    context.moveTo(54, 154);
    context.lineTo(1546, 154);
    context.stroke();
    metrics.slice(0, 12).forEach((metric, index) => {
      const column = index % 2;
      const row = Math.floor(index / 2);
      const x = 54 + column * 758;
      const y = 198 + row * 120;
      const statusColor = colorFor(metric.status);
      context.fillStyle =
        index % 4 < 2
          ? "rgba(19,49,41,0.72)"
          : "rgba(10,31,26,0.72)";
      context.fillRect(x, y - 25, 720, 96);
      context.fillStyle = statusColor;
      context.beginPath();
      context.arc(x + 24, y + 20, 12, 0, Math.PI * 2);
      context.fill();
      context.fillStyle = "#a9c7bb";
      context.font = '700 23px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(String(metric.label || "METRIC"), x + 52, y);
      context.fillStyle = statusColor;
      context.font = '850 31px "ForkMesh Mono", ui-monospace, monospace';
      context.textAlign = "right";
      context.fillText(String(metric.value ?? "—"), x + 690, y + 3, 240);
      context.textAlign = "left";
      context.fillStyle = "#6f9185";
      context.font = '550 18px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        String(metric.note || "bounded local sample").slice(0, 54),
        x + 52,
        y + 39,
        625,
      );
    });
    context.strokeStyle = "#21443a";
    context.beginPath();
    context.moveTo(54, 900);
    context.lineTo(1546, 900);
    context.stroke();
    context.fillStyle = "#80bda0";
    context.font = '650 21px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      `SAMPLED ${String(snapshot.sampledAt || "NOW")} · LOCAL ONLY · NO PROFILING DATA UPLOADED`,
      54,
      944,
    );
  }
}

function engineeringDebugTexture(THREE, snapshot = {}) {
  return canvasTexture(THREE, 1600, 1000, (context) =>
    drawEngineeringDebug(context, snapshot),
  );
}

export function createEngineeringDebugPanel(THREE) {
  const panel = new THREE.Group();
  panel.name = "forkmesh-engineering-live-debug-panel";
  panel.userData.officeFloorId = "engineering";
  const frame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 25.4, 12.8, 0.45),
    makeSharedMaterial(THREE, "#16372f", {
      emissive: "#174c3c",
      emissiveIntensity: 0.42,
      metalness: 0.58,
      roughness: 0.34,
    }),
  );
  frame.position.y = 7.25;
  panel.add(frame);
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 24.7, 12.1),
    resources.uniqueMaterial(THREE, {
      map: engineeringDebugTexture(THREE, {
        overall: "warning",
        metrics: [],
        sampledAt: "STARTING",
      }),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  face.name = "forkmesh-engineering-live-debug-face";
  face.position.set(0, 7.25, 0.24);
  panel.add(face);
  panel.userData.face = face;
  return panel;
}

export function formatCapacityBytes(bytes) {
  const value = Number(bytes);
  if (!Number.isFinite(value) || value <= 0) return "0 B";
  const units = ["B", "KB", "MB", "GB", "TB", "PB"];
  const step = Math.min(
    units.length - 1,
    Math.floor(Math.log(value) / Math.log(1024)),
  );
  const scaled = value / 1024 ** step;
  const shown =
    scaled >= 100 || step === 0 ? Math.round(scaled) : scaled.toFixed(1);
  return `${shown} ${units[step]}`;
}

export function formatPlatformLimitBytes(bytes) {
  const value = Math.max(0, Number(bytes) || 0);
  if (value < 1000) return `${Math.round(value)} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let scaled = value / 1000;
  let index = 0;
  while (scaled >= 1000 && index < units.length - 1) {
    scaled /= 1000;
    index += 1;
  }
  return `${scaled >= 10 ? scaled.toFixed(0) : scaled.toFixed(1)} ${units[index]}`;
}

export function systemCapacityMetricPairs(record) {
  const usage =
    record?.usage && typeof record.usage === "object" ? record.usage : null;
  const limits =
    record?.limits && typeof record.limits === "object" ? record.limits : null;
  if (!usage || !limits) return [];
  return Object.keys(usage)
    .filter(
      (key) =>
        /^[A-Za-z][A-Za-z0-9_-]{0,31}$/.test(key) &&
        Object.prototype.hasOwnProperty.call(limits, key),
    )
    .map((key) => ({
      key,
      usage: Number(usage[key]),
      limit: Number(limits[key]),
    }))
    .filter(
      (metric) =>
        Number.isFinite(metric.usage) &&
        metric.usage >= 0 &&
        Number.isFinite(metric.limit) &&
        metric.limit > 0,
    )
    .slice(0, 4);
}
