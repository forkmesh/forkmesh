export const manifest = {
  id: "status-board",
  label: "System status board",
  category: "Store",
  version: 1,
  summary:
    "A standing board that mirrors forkmesh.com/status: every monitored system with its last hour, day and month of checks.",
  params: [
    {
      key: "caption",
      type: "text",
      label: "Caption",
      default: "System status",
      maxLength: 40,
    },
    {
      key: "refreshSeconds",
      type: "number",
      label: "Refresh (seconds)",
      default: 180,
      min: 60,
      max: 900,
      step: 30,
    },
    {
      key: "frame",
      type: "select",
      label: "Frame",
      default: "green",
      options: ["green", "blue", "slate"],
    },
  ],
  network: ["/api/status/world"],
};

const FRAMES = {
  green: "#49d98a",
  blue: "#60a5fa",
  slate: "#94a3b8",
};

const STATE_COLORS = {
  operational: "#198a43",
  degraded: "#a66f00",
  down: "#d92d3a",
  unknown: "#8c959f",
};

const LEGEND = [
  ["operational", "OPERATIONAL"],
  ["degraded", "DEGRADED"],
  ["down", "DOWN"],
  ["unknown", "NO DATA"],
];

const WIDTH = 1024;
const HEIGHT = 1280;
const FONT = "system-ui, sans-serif";
const MONO = "ui-monospace, monospace";

function stateColor(status) {
  return STATE_COLORS[String(status || "unknown").toLowerCase()] ||
    STATE_COLORS.unknown;
}

function dayState(day) {
  const checks = Math.max(0, Number(day?.checks) || 0);
  const failures = Math.max(0, Number(day?.failures) || 0);
  if (checks <= 0) return day?.status || "unknown";
  if (failures >= checks) return "down";
  if (failures > 0) return "degraded";
  return "operational";
}

function percent(value) {
  const number = Number(value);
  return Number.isFinite(number) ? `${number.toFixed(1)}%` : "—";
}

function list(value) {
  return Array.isArray(value) ? value : [];
}

function clip(context, text, width) {
  let value = String(text || "");
  if (context.measureText(value).width <= width) return value;
  while (value && context.measureText(`${value}…`).width > width) {
    value = value.slice(0, -1);
  }
  return `${value}…`;
}

function strip(context, cells, slots, left, top, width, height) {
  const slot = width / slots;
  const cell = Math.max(2, slot - Math.min(4, slot * 0.2));
  cells.slice(-slots).forEach((status, index) => {
    context.fillStyle = stateColor(status);
    context.fillRect(left + index * slot, top, cell, height);
  });
}

export function build({ THREE, params, net }) {
  const accent = FRAMES[params.frame] || FRAMES.green;
  const group = new THREE.Group();

  const canvas = document.createElement("canvas");
  canvas.width = WIDTH;
  canvas.height = HEIGHT;
  const context = canvas.getContext("2d");
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;

  let payload = null;
  let failed = false;

  function paint() {
    context.textAlign = "left";
    context.fillStyle = "#f7f7f8";
    context.fillRect(0, 0, WIDTH, HEIGHT);
    context.fillStyle = accent;
    context.fillRect(0, 0, WIDTH, 132);
    context.fillStyle = "#0f172a";
    context.font = `800 64px ${FONT}`;
    context.fillText(
      clip(context, (params.caption || "System status").toUpperCase(), 940),
      42,
      92,
    );
    context.fillStyle = "#686b74";
    context.font = `500 22px ${FONT}`;
    context.fillText(
      "last 60 minutes  ·  last 24 hours  ·  last 30 days",
      42,
      172,
    );

    const systems = list(payload?.systems);
    if (!systems.length) {
      context.fillStyle = "#25262b";
      context.font = `700 40px ${MONO}`;
      context.fillText(
        failed ? "STATUS UNAVAILABLE" : "LOADING LIVE CHECKS…",
        42,
        320,
      );
    }

    const rowTop = 200;
    const rowHeight = systems.length
      ? Math.min(200, 980 / systems.length)
      : 200;
    const scale = Math.min(1, rowHeight / 180);
    const cellHeight = Math.max(4, Math.min(16, rowHeight * 0.08));
    const graphLeft = 56;
    const graphWidth = WIDTH - graphLeft * 2;

    systems.forEach((system, index) => {
      const top = rowTop + index * rowHeight;
      context.fillStyle = "#ffffff";
      context.fillRect(36, top, WIDTH - 72, Math.max(20, rowHeight - 10));
      context.strokeStyle = "#dcdee4";
      context.lineWidth = 2;
      context.strokeRect(36, top, WIDTH - 72, Math.max(20, rowHeight - 10));

      const titleY = top + Math.max(18, 40 * scale);
      context.beginPath();
      context.fillStyle = stateColor(system?.status);
      context.arc(62, titleY - 9 * scale, Math.max(4, 9 * scale), 0, Math.PI * 2);
      context.fill();
      context.fillStyle = "#202126";
      context.font = `700 ${Math.max(12, 28 * scale)}px ${FONT}`;
      context.fillText(
        clip(context, system?.label || system?.id || "", 520),
        82,
        titleY,
      );

      const node = /^(core|runner):/.test(String(system?.id || ""));
      context.textAlign = "right";
      context.font = `700 ${Math.max(11, 24 * scale)}px ${MONO}`;
      context.fillText(percent(system?.uptime24hPct), node ? 968 : 800, titleY);
      if (!node) context.fillText(percent(system?.uptimePct), 968, titleY);
      context.fillStyle = "#686b74";
      context.font = `500 ${Math.max(9, 15 * scale)}px ${FONT}`;
      context.fillText("24h", node ? 968 : 800, titleY + Math.max(11, 20 * scale));
      if (!node) context.fillText("30d", 968, titleY + Math.max(11, 20 * scale));
      context.textAlign = "left";

      const days = list(system?.days).slice(-30);
      const hours = list(system?.hours).length
        ? list(system.hours)
        : days.flatMap((day) => list(day?.hours));
      strip(
        context,
        list(system?.minutes).map((minute) => minute?.status),
        60,
        graphLeft,
        top + rowHeight * 0.42,
        graphWidth,
        cellHeight,
      );
      strip(
        context,
        hours.map((hour) => hour?.status),
        24,
        graphLeft,
        top + rowHeight * 0.6,
        graphWidth,
        cellHeight,
      );
      if (!node) strip(
        context,
        days.map(dayState),
        30,
        graphLeft,
        top + rowHeight * 0.78,
        graphWidth,
        cellHeight,
      );
    });

    context.font = `600 22px ${MONO}`;
    let legendX = 42;
    LEGEND.forEach(([state, label]) => {
      context.fillStyle = stateColor(state);
      context.fillText("●", legendX, HEIGHT - 40);
      legendX += context.measureText("● ").width;
      context.fillStyle = "#686b74";
      context.fillText(label, legendX, HEIGHT - 40);
      legendX += context.measureText(`${label}   `).width;
    });
    context.strokeStyle = accent;
    context.lineWidth = 12;
    context.strokeRect(6, 6, WIDTH - 12, HEIGHT - 12);
    texture.needsUpdate = true;
  }

  const frame = new THREE.Mesh(
    new THREE.BoxGeometry(4.2, 5.2, 0.18),
    new THREE.MeshStandardMaterial({ color: 0x1e293b, roughness: 0.6 }),
  );
  frame.position.y = 4.1;
  frame.castShadow = true;
  frame.receiveShadow = true;
  group.add(frame);

  const postGeometry = new THREE.CylinderGeometry(0.09, 0.11, 1.6, 10);
  const postMaterial = new THREE.MeshStandardMaterial({
    color: accent,
    roughness: 0.5,
  });
  [-1.5, 1.5].forEach((x) => {
    const post = new THREE.Mesh(postGeometry, postMaterial);
    post.position.set(x, 0.8, 0);
    post.castShadow = true;
    group.add(post);
  });

  const face = new THREE.Mesh(
    new THREE.PlaneGeometry(4, 5),
    new THREE.MeshBasicMaterial({ map: texture }),
  );
  face.position.set(0, 4.1, 0.1);
  group.add(face);

  paint();

  let nextReadAt = 0;
  let reading = false;

  async function refresh() {
    if (reading) return;
    reading = true;
    try {
      payload = await net("/api/status/world");
      failed = false;
    } catch {
      failed = !payload;
    } finally {
      reading = false;
      paint();
    }
  }

  return {
    group,
    tick(nowMs) {
      if (nowMs < nextReadAt) return;
      if (typeof document !== "undefined" && document.hidden) return;
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
