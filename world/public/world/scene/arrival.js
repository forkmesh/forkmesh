import { sharedGeometry } from "../world-resource-cache.js";
import {
  canvasTexture,
  roundedRect,
  wordTexture,
} from "./canvas.js";
import { activeLeaderboardTexture } from "./leaderboards.js";
import { makeSharedMaterial } from "./materials.js";
import * as resources from "./resources.js";

export const groundPlaqueParts = new Set();

export function makeGroundPlaque(THREE, title, subtitle, color) {
  const plaque = new THREE.Group();
  plaque.name = "forkmesh-section-plaque";
  const base = new THREE.Mesh(
    sharedGeometry(THREE, "BoxGeometry", 3.9, 0.22, 1.5),
    makeSharedMaterial(THREE, "#233b33", { roughness: 0.82 }),
  );
  base.position.y = 0.11;
  plaque.add(base);
  const slab = new THREE.Mesh(
    sharedGeometry(THREE, "BoxGeometry", 3.6, 1.3, 0.14),
    makeSharedMaterial(THREE, "#101d18", { roughness: 0.55, metalness: 0.12 }),
  );
  slab.position.set(0, 0.74, 0.12);
  // Lean the top back so the face reads from the raised world camera.
  slab.rotation.x = -0.42;
  plaque.add(slab);
  groundPlaqueParts.add(base);
  groundPlaqueParts.add(slab);
  const face = new THREE.Mesh(
    sharedGeometry(THREE, "PlaneGeometry", 3.44, 1.14),
    resources.uniqueMaterial(THREE, {
      map: wordTexture(THREE, title, subtitle, color),
      transparent: true,
    }, "MeshBasicMaterial"),
  );
  face.position.z = 0.08;
  slab.add(face);
  return plaque;
}

function formatArrivalCount(value) {
  return Math.max(0, Math.min(99999999, Math.round(Number(value) || 0)))
    .toLocaleString("en-US");
}

function arrivalTrendText(current, previous) {
  const now = Math.max(0, Math.round(Number(current) || 0));
  const then = Math.max(0, Math.round(Number(previous) || 0));
  const yesterday = `yesterday ${formatArrivalCount(then)}`.toUpperCase();
  if (then <= 0) {
    return now > 0 ? `▲ UP · ${yesterday}` : `= EVEN · ${yesterday}`;
  }
  const delta = Math.round(((now - then) / then) * 100);
  if (delta > 0) return `▲ +${delta}% · ${yesterday}`;
  if (delta < 0) return `▼ ${delta}% · ${yesterday}`;
  return `= EVEN · ${yesterday}`;
}

export function arrivalPlaqueTexture(THREE, stats) {
  const ready = Boolean(stats);
  const lines = [
    {
      label: "UNIQUE VISITORS",
      value: ready ? formatArrivalCount(stats.total) : "—",
      note: ready
        ? "ALL TIME · APPROXIMATE · NO RAW IP STORED"
        : "COUNTING…",
    },
    {
      label: "TODAY",
      value: ready ? formatArrivalCount(stats.today) : "—",
      note: ready
        ? arrivalTrendText(stats.today, stats.yesterdaySameTime)
        : "COUNTING…",
    },
    {
      label: "PAST HOUR",
      value: ready ? formatArrivalCount(stats.pastHour) : "—",
      note: ready
        ? arrivalTrendText(stats.pastHour, stats.pastHourYesterday)
        : "COUNTING…",
    },
  ];
  return canvasTexture(THREE, 768, 512, (context) => {
    context.clearRect(0, 0, 768, 512);
    roundedRect(context, 6, 6, 756, 500, 18);
    context.fillStyle = "rgba(6,17,14,0.94)";
    context.fill();
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 6;
    context.stroke();
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 54px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText("ARRIVAL GRID", 46, 74);
    context.fillStyle = "#9ef7c6";
    context.font = '400 23px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      "10 visitors per row · face the square".toUpperCase(),
      46,
      126,
    );
    context.fillStyle = "rgba(158,247,198,0.35)";
    context.fillRect(46, 158, 676, 3);
    lines.forEach((line, index) => {
      const top = 204 + index * 106;
      context.textAlign = "left";
      context.fillStyle = "#9ef7c6";
      context.font = '700 26px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(line.label, 46, top);
      context.fillStyle = "#77d9ff";
      context.font = '400 20px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(line.note, 46, top + 40);
      context.textAlign = "right";
      context.fillStyle = "#f1fff6";
      context.font = '700 46px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(line.value, 722, top + 14);
    });
  });
}

// The Arrival Grid's sign is a standing plaque at the grid's front edge:
// fresh arrivals spawn facing the square, so the visit counters are the
// first readable landmark, and the raised world camera sees the same face.
export function makeArrivalPlaque(THREE) {
  const plaque = new THREE.Group();
  plaque.name = "world-arrival-plaque";
  const base = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 4.1, 0.24, 1.6),
    makeSharedMaterial(THREE, "#233b33", { roughness: 0.82 }),
  );
  base.position.y = 0.12;
  plaque.add(base);
  const slab = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 3.7, 2.5, 0.16),
    makeSharedMaterial(THREE, "#101d18", { roughness: 0.55, metalness: 0.12 }),
  );
  slab.position.set(0, 1.5, 0.1);
  slab.rotation.x = -0.16;
  plaque.add(slab);
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 3.54, 2.36),
    resources.uniqueMaterial(THREE, {
      map: arrivalPlaqueTexture(THREE, null),
      transparent: true,
    }, "MeshBasicMaterial"),
  );
  face.position.z = 0.09;
  slab.add(face);
  plaque.userData.statsFace = face;
  return plaque;
}

export function makeActiveLeaderboardSign(THREE) {
  const sign = new THREE.Group();
  sign.name = "world-active-leaderboard";
  const base = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 4.8, 0.25, 1.35),
    makeSharedMaterial(THREE, "#1a3032", { roughness: 0.8 }),
  );
  base.position.y = 0.13;
  sign.add(base);
  for (const x of [-1.72, 1.72]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.16, 3.7, 0.16),
      makeSharedMaterial(THREE, "#234d4a", { metalness: 0.22, roughness: 0.52 }),
    );
    post.position.set(x, 1.85, 0);
    sign.add(post);
  }
  const board = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 3.72, 2.72, 0.12),
    makeSharedMaterial(THREE, "#071712", { roughness: 0.55 }),
  );
  board.position.set(0, 2.08, 0.08);
  sign.add(board);
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 3.52, 2.55),
    resources.uniqueMaterial(THREE, {
      map: activeLeaderboardTexture(THREE),
      transparent: true,
    }, "MeshBasicMaterial"),
  );
  face.position.set(0, 2.08, 0.151);
  sign.add(face);
  sign.userData.face = face;
  return sign;
}

// Places a plaque on the ground in front of the section — on the side facing
// the Town Square center, where visitors walk up. `position` is the section's
// world position; sections at the center face the arrival grid instead.
function placeSectionPlaque(group, plaque, position, distance) {
  const x = Array.isArray(position) ? position[0] : 0;
  const z = Array.isArray(position) ? position[2] : 0;
  const length = Math.hypot(x, z);
  const ux = length > 0.001 ? -x / length : 0;
  const uz = length > 0.001 ? -z / length : 1;
  plaque.position.set(ux * distance, 0, uz * distance);
  plaque.rotation.y = Math.atan2(ux, uz);
  group.add(plaque);
  return plaque;
}

export function addSectionPlaque(THREE, group, position, title, subtitle, color, distance) {
  return placeSectionPlaque(
    group,
    makeGroundPlaque(THREE, title, subtitle, color),
    position,
    distance,
  );
}
