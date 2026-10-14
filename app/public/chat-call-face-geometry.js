export const LANDMARK_COUNT = 68;
export const JAW = range(0, 17);
export const LEFT_EYE = range(36, 42);
export const RIGHT_EYE = range(42, 48);
export const OUTER_LIPS = range(48, 60);
export const INNER_LIPS = range(60, 68);

const MIRROR = [
  16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
  26, 25, 24, 23, 22, 21, 20, 19, 18, 17,
  27, 28, 29, 30, 35, 34, 33, 32, 31,
  45, 44, 43, 42, 47, 46, 39, 38, 37, 36, 41, 40,
  54, 53, 52, 51, 50, 49, 48, 59, 58, 57, 56, 55,
  64, 63, 62, 61, 60, 67, 66, 65,
];

const DEPTH = [
  -0.95, -0.9, -0.8, -0.65, -0.45, -0.25, -0.08, 0.05, 0.1, 0.05, -0.08, -0.25, -0.45, -0.65, -0.8, -0.9, -0.95,
  -0.25, -0.08, 0.02, 0.05, 0.05, 0.05, 0.05, 0.02, -0.08, -0.25,
  0, 0.12, 0.25, 0.38, 0.12, 0.2, 0.25, 0.2, 0.12,
  -0.12, -0.04, -0.04, -0.03, -0.04, -0.04, -0.03, -0.04, -0.04, -0.12, -0.04, -0.04,
  0.02, 0.12, 0.17, 0.19, 0.17, 0.12, 0.02, 0.12, 0.15, 0.16, 0.15, 0.12,
  0.04, 0.13, 0.14, 0.13, 0.04, 0.13, 0.14, 0.13,
];

const POSE_WEIGHTS = DEPTH.map((_, i) => (i <= 16 ? 0.4 : i <= 26 ? 0.8 : i <= 35 ? 1.4 : i <= 47 ? 1.4 : 0));
const RESIDUAL_SOURCES = range(0, 36);
const RESIDUAL_SIGMA = 0.55;
const RESIDUAL_ANCHOR = 0.08;
const FOREHEAD_DEPTH = -0.05;
const RING_DEPTH = -0.75;
const CHIN_DROP = [[5, 0.3], [6, 0.6], [7, 0.85], [8, 1], [9, 0.85], [10, 0.6], [11, 0.3]];
export const FOREHEAD_LIFT = 0.5;
const MAX_OPEN = 0.42;

function range(start, end) {
  return Array.from({ length: end - start }, (_, i) => start + i);
}

export function pointsFromFlat(flat) {
  const points = [];
  for (let i = 0; i + 1 < flat.length; i += 2) points.push([flat[i], flat[i + 1]]);
  return points;
}

export function centroid(points, indices) {
  let x = 0;
  let y = 0;
  for (const i of indices) {
    x += points[i][0];
    y += points[i][1];
  }
  return [x / indices.length, y / indices.length];
}

export function faceFrame(points) {
  const left = centroid(points, LEFT_EYE);
  const right = centroid(points, RIGHT_EYE);
  const dx = right[0] - left[0];
  const dy = right[1] - left[1];
  const span = Math.hypot(dx, dy) || 1;
  const ux = [dx / span, dy / span];
  return {
    left,
    right,
    mid: [(left[0] + right[0]) / 2, (left[1] + right[1]) / 2],
    span,
    ux,
    uy: [-ux[1], ux[0]],
    roll: Math.atan2(dy, dx),
  };
}

function toFrame(frame, point) {
  const x = point[0] - frame.mid[0];
  const y = point[1] - frame.mid[1];
  return [x * frame.ux[0] + y * frame.ux[1], x * frame.uy[0] + y * frame.uy[1]];
}

function fromFrame(frame, a, b) {
  return [
    frame.mid[0] + a * frame.ux[0] + b * frame.uy[0],
    frame.mid[1] + a * frame.ux[1] + b * frame.uy[1],
  ];
}

export function normalizeShape(points) {
  const frame = faceFrame(points);
  return points.map((point) => {
    const [a, b] = toFrame(frame, point);
    return [a / frame.span, b / frame.span];
  });
}

export function mouthGap(points) {
  const frame = faceFrame(points);
  const top = toFrame(frame, points[62]);
  const bottom = toFrame(frame, points[66]);
  return (bottom[1] - top[1]) / frame.span;
}

export function buildTemplate(shapes) {
  const normalized = shapes.map(normalizeShape);
  const closed = normalized.filter((shape) => shape[66][1] - shape[62][1] < 0.05);
  const mouthSource = closed.length ? closed : normalized;
  const mean = [];
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    const pool = i >= 48 ? mouthSource : normalized;
    let x = 0;
    let y = 0;
    for (const shape of pool) {
      x += shape[i][0];
      y += shape[i][1];
    }
    mean.push([x / pool.length, y / pool.length]);
  }
  const symmetric = mean.map((point, i) => {
    const twin = mean[MIRROR[i]];
    return [(point[0] - twin[0]) / 2, (point[1] + twin[1]) / 2];
  });
  for (const [upper, lower] of [[61, 67], [62, 66], [63, 65]]) {
    const y = (symmetric[upper][1] + symmetric[lower][1]) / 2;
    symmetric[upper][1] = y;
    symmetric[lower][1] = y;
  }
  return symmetric.map(([x, y], i) => [x, y, DEPTH[i]]);
}

export function fitTemplate(template, anchors) {
  const yaw = clampValue(anchors.yaw || 0, -0.7, 0.7);
  const cos = Math.cos(yaw);
  const sin = Math.sin(yaw);
  const turned = template.map(([x, y, z]) => [x * cos + z * sin, y]);
  const frame = faceFrame(turned);
  const eyeL = anchors.eyeL;
  const eyeR = anchors.eyeR;
  const dx = eyeR[0] - eyeL[0];
  const dy = eyeR[1] - eyeL[1];
  const span = Math.hypot(dx, dy) || 1;
  const scale = span / frame.span;
  const angle = Math.atan2(dy, dx) - frame.roll;
  const c = Math.cos(angle) * scale;
  const s = Math.sin(angle) * scale;
  const mid = [(eyeL[0] + eyeR[0]) / 2, (eyeL[1] + eyeR[1]) / 2];
  const placed = turned.map(([x, y]) => {
    const px = x - frame.mid[0];
    const py = y - frame.mid[1];
    return [mid[0] + px * c - py * s, mid[1] + px * s + py * c];
  });
  const live = faceFrame(placed);
  const local = placed.map((point) => toFrame(live, point));
  const mouthAt = [(local[48][0] + local[54][0]) / 2, (local[48][1] + local[54][1]) / 2];
  if (anchors.mouth) {
    const target = toFrame(live, anchors.mouth);
    const stretch = clampValue(target[1] / (mouthAt[1] || 1), 0.7, 1.35);
    const shift = clampValue(target[0] - mouthAt[0], -0.35 * live.span, 0.35 * live.span);
    for (const point of local) {
      if (point[1] > 0) {
        point[0] += shift * clampValue(point[1] / mouthAt[1], 0, 1.4);
        point[1] *= stretch;
      } else {
        point[1] *= 1 + (stretch - 1) * 0.5;
      }
    }
  }
  const center = [(local[48][0] + local[54][0]) / 2, (local[48][1] + local[54][1]) / 2];
  if (anchors.mouthWidth) {
    const width = Math.hypot(local[54][0] - local[48][0], local[54][1] - local[48][1]) || 1;
    const ratio = Math.pow(clampValue(anchors.mouthWidth / width, 0.75, 1.4), 0.8);
    for (let i = 48; i < 68; i += 1) {
      local[i][0] = center[0] + (local[i][0] - center[0]) * ratio;
    }
  }
  const drop = clampValue(anchors.open || 0, 0, 1) * MAX_OPEN * live.span;
  if (drop > 0) {
    for (const i of [55, 56, 57, 58, 59, 65, 66, 67]) local[i][1] += drop;
    for (const i of [61, 62, 63]) local[i][1] -= drop * 0.08;
    for (const [i, weight] of CHIN_DROP) local[i][1] += drop * 0.85 * weight;
  }
  return local.map(([a, b]) => fromFrame(live, a, b));
}

export function delaunay(points) {
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const [x, y] of points) {
    minX = Math.min(minX, x);
    minY = Math.min(minY, y);
    maxX = Math.max(maxX, x);
    maxY = Math.max(maxY, y);
  }
  const size = Math.max(maxX - minX, maxY - minY) || 1;
  const midX = (minX + maxX) / 2;
  const midY = (minY + maxY) / 2;
  const all = points.concat([
    [midX - 20 * size, midY - size],
    [midX, midY + 20 * size],
    [midX + 20 * size, midY - size],
  ]);
  const n = points.length;
  const circle = (a, b, c) => {
    const [ax, ay] = all[a];
    const [bx, by] = all[b];
    const [cx, cy] = all[c];
    const d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (Math.abs(d) < 1e-12) return { x: 0, y: 0, r: Infinity };
    const a2 = ax * ax + ay * ay;
    const b2 = bx * bx + by * by;
    const c2 = cx * cx + cy * cy;
    const x = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d;
    const y = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;
    return { x, y, r: (x - ax) ** 2 + (y - ay) ** 2 };
  };
  let triangles = [{ v: [n, n + 1, n + 2], c: circle(n, n + 1, n + 2) }];
  for (let i = 0; i < n; i += 1) {
    const [px, py] = all[i];
    const bad = [];
    const keep = [];
    for (const t of triangles) {
      if ((px - t.c.x) ** 2 + (py - t.c.y) ** 2 < t.c.r) bad.push(t);
      else keep.push(t);
    }
    const edges = new Map();
    for (const t of bad) {
      for (const [a, b] of [[t.v[0], t.v[1]], [t.v[1], t.v[2]], [t.v[2], t.v[0]]]) {
        const key = a < b ? `${a},${b}` : `${b},${a}`;
        edges.set(key, edges.has(key) ? null : [a, b]);
      }
    }
    for (const edge of edges.values()) {
      if (edge) keep.push({ v: [edge[0], edge[1], i], c: circle(edge[0], edge[1], i) });
    }
    triangles = keep;
  }
  return triangles.filter((t) => t.v.every((v) => v < n)).map((t) => t.v);
}

export function headLayout(points, ring, forehead = FOREHEAD_LIFT) {
  const frame = faceFrame(points);
  const lift = forehead * frame.span;
  const out = points.map((point) => [point[0], point[1]]);
  for (let i = 17; i <= 26; i += 1) {
    out.push([points[i][0] - frame.uy[0] * lift, points[i][1] - frame.uy[1] * lift]);
  }
  for (const point of ring) out.push([point[0], point[1]]);
  return out;
}

export function headModel(points, ring) {
  const frame = faceFrame(points);
  const layout = headLayout(points, ring);
  return layout.map((point, i) => {
    const [a, b] = toFrame(frame, point);
    const z = i < LANDMARK_COUNT ? DEPTH[i] : i < LANDMARK_COUNT + 10 ? FOREHEAD_DEPTH : RING_DEPTH;
    return [a / frame.span, b / frame.span, z];
  });
}

export function buildHeadMesh(model, template) {
  const open = fitTemplate(template, { eyeL: [-0.5, 0], eyeR: [0.5, 0], open: 0.6 });
  const layout = model.map((point, i) => (i < LANDMARK_COUNT ? open[i] : [point[0], point[1]]));
  const triangles = delaunay(layout).map((triangle) => ({
    v: triangle,
    mouth: triangle.every((v) => v >= 60 && v < 68),
  }));
  return { triangles, count: layout.length };
}

export function solvePose(model, live) {
  let total = 0;
  const mm = [0, 0, 0];
  const lm = [0, 0];
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    const w = POSE_WEIGHTS[i];
    if (!w) continue;
    total += w;
    for (let k = 0; k < 3; k += 1) mm[k] += w * model[i][k];
    lm[0] += w * live[i][0];
    lm[1] += w * live[i][1];
  }
  for (let k = 0; k < 3; k += 1) mm[k] /= total;
  lm[0] /= total;
  lm[1] /= total;
  const mtm = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  const ltm = [[0, 0, 0], [0, 0, 0]];
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    const w = POSE_WEIGHTS[i];
    if (!w) continue;
    const m = [model[i][0] - mm[0], model[i][1] - mm[1], model[i][2] - mm[2]];
    const l = [live[i][0] - lm[0], live[i][1] - lm[1]];
    for (let a = 0; a < 3; a += 1) {
      for (let b = 0; b < 3; b += 1) mtm[a][b] += w * m[a] * m[b];
      ltm[0][a] += w * l[0] * m[a];
      ltm[1][a] += w * l[1] * m[a];
    }
  }
  for (let a = 0; a < 3; a += 1) mtm[a][a] += 1e-4;
  const inverse = invert3(mtm);
  const rows = ltm.map((row) => [0, 1, 2].map((b) => row[0] * inverse[0][b] + row[1] * inverse[1][b] + row[2] * inverse[2][b]));
  const s1 = Math.hypot(...rows[0]) || 1;
  const s2 = Math.hypot(...rows[1]) || 1;
  const a = rows[0].map((v) => v / s1);
  const b = rows[1].map((v) => v / s2);
  const c = normalize3(a.map((v, i) => v + b[i]));
  const d = normalize3(a.map((v, i) => v - b[i]));
  const r1 = c.map((v, i) => (v + d[i]) / Math.SQRT2);
  const r2 = c.map((v, i) => (v - d[i]) / Math.SQRT2);
  const scale = (s1 + s2) / 2;
  const r3 = [r1[1] * r2[2] - r1[2] * r2[1], r1[2] * r2[0] - r1[0] * r2[2], r1[0] * r2[1] - r1[1] * r2[0]];
  const t = [lm[0] - scale * dot3(r1, mm), lm[1] - scale * dot3(r2, mm)];
  return {
    scale,
    rows: [r1, r2, r3],
    t,
    roll: Math.atan2(r2[0], r1[0]),
    yaw: Math.asin(clampValue(-r3[0], -1, 1)),
    pitch: Math.asin(clampValue(r3[1], -1, 1)),
  };
}

export function projectPose(pose, point) {
  const [r1, r2] = pose.rows;
  return [pose.t[0] + pose.scale * dot3(r1, point), pose.t[1] + pose.scale * dot3(r2, point)];
}

export function liveHead(model, live) {
  const pose = solvePose(model, live);
  const out = live.map((point) => [point[0], point[1]]);
  const residuals = [];
  for (const i of RESIDUAL_SOURCES) {
    const projected = projectPose(pose, model[i]);
    residuals.push([i, live[i][0] - projected[0], live[i][1] - projected[1]]);
  }
  for (let v = LANDMARK_COUNT; v < model.length; v += 1) {
    const projected = projectPose(pose, model[v]);
    let wx = 0;
    let wy = 0;
    let weight = RESIDUAL_ANCHOR;
    for (const [i, dx, dy] of residuals) {
      const d2 = (model[v][0] - model[i][0]) ** 2 + (model[v][1] - model[i][1]) ** 2;
      const w = Math.exp(-d2 / (2 * RESIDUAL_SIGMA * RESIDUAL_SIGMA));
      wx += w * dx;
      wy += w * dy;
      weight += w;
    }
    out.push([projected[0] + wx / weight, projected[1] + wy / weight]);
  }
  return { points: out, pose };
}

function dot3(a, b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

function normalize3(v) {
  const length = Math.hypot(v[0], v[1], v[2]) || 1;
  return [v[0] / length, v[1] / length, v[2] / length];
}

function invert3(m) {
  const [[a, b, c], [d, e, f], [g, h, i]] = m;
  const A = e * i - f * h;
  const B = -(d * i - f * g);
  const C = d * h - e * g;
  const det = a * A + b * B + c * C || 1e-9;
  return [
    [A / det, -(b * i - c * h) / det, (b * f - c * e) / det],
    [B / det, (a * i - c * g) / det, -(a * f - c * d) / det],
    [C / det, -(a * h - b * g) / det, (a * e - b * d) / det],
  ];
}

export function landmarksFromRegions(regions) {
  const need = ["faceContour", "leftEyebrow", "rightEyebrow", "leftEye", "rightEye", "outerLips", "innerLips"];
  if (!regions || need.some((name) => !(regions[name]?.length >= 3))) return null;
  const crest = regions.noseCrest?.length >= 2 ? regions.noseCrest : regions.medianLine;
  if (!(crest?.length >= 2) || !(regions.nose?.length >= 3)) return null;
  const out = new Array(LANDMARK_COUNT);
  let jaw = regions.faceContour.map(([x, y]) => [x, y]);
  if (jaw[0][0] > jaw[jaw.length - 1][0]) jaw.reverse();
  resamplePolyline(jaw, 17).forEach((point, i) => { out[i] = point; });
  const brows = [regions.leftEyebrow, regions.rightEyebrow]
    .sort((a, b) => meanX(a) - meanX(b));
  browLine(brows[0]).forEach((point, i) => { out[17 + i] = point; });
  browLine(brows[1]).forEach((point, i) => { out[22 + i] = point; });
  const ridge = crest.map(([x, y]) => [x, y]).sort((a, b) => a[1] - b[1]);
  resamplePolyline(ridge, 4).forEach((point, i) => { out[27 + i] = point; });
  const nose = regions.nose.map(([x, y]) => [x, y]);
  const middle = nose.reduce((sum, point) => sum + point[1], 0) / nose.length;
  const base = nose.filter((point) => point[1] >= middle).sort((a, b) => a[0] - b[0]);
  resamplePolyline(base.length >= 2 ? base : nose.sort((a, b) => a[0] - b[0]), 5)
    .forEach((point, i) => { out[31 + i] = point; });
  const eyes = [regions.leftEye, regions.rightEye].sort((a, b) => meanX(a) - meanX(b));
  eyes.forEach((loop, side) => {
    const [upper, lower] = splitLoop(loop, 4, 4);
    const at = side ? 42 : 36;
    out[at] = upper[0];
    out[at + 1] = upper[1];
    out[at + 2] = upper[2];
    out[at + 3] = upper[3];
    out[at + 4] = lower[2];
    out[at + 5] = lower[1];
  });
  const [lipTop, lipBottom] = splitLoop(regions.outerLips, 7, 7);
  for (let i = 0; i < 7; i += 1) out[48 + i] = lipTop[i];
  for (let i = 1; i < 6; i += 1) out[60 - i] = lipBottom[i];
  const [innerTop, innerBottom] = splitLoop(regions.innerLips, 5, 5);
  for (let i = 0; i < 5; i += 1) out[60 + i] = innerTop[i];
  for (let i = 1; i < 4; i += 1) out[68 - i] = innerBottom[i];
  return out;
}

export function shapeRegions(points) {
  const pick = (indices) => indices.map((i) => [points[i][0], points[i][1]]);
  return {
    faceContour: pick(JAW),
    leftEyebrow: pick([17, 18, 19, 20, 21]),
    rightEyebrow: pick([22, 23, 24, 25, 26]),
    noseCrest: pick([27, 28, 29, 30]),
    nose: pick([31, 32, 33, 34, 35]),
    leftEye: pick(LEFT_EYE),
    rightEye: pick(RIGHT_EYE),
    outerLips: pick(OUTER_LIPS),
    innerLips: pick(INNER_LIPS),
  };
}

export function canonicalShape(points) {
  const shaped = landmarksFromRegions(shapeRegions(points));
  if (!shaped) return points;
  for (let i = 27; i <= 35; i += 1) shaped[i] = [points[i][0], points[i][1]];
  return shaped;
}

function meanX(points) {
  return points.reduce((sum, point) => sum + point[0], 0) / points.length;
}

function browLine(points) {
  const sorted = points.map(([x, y]) => [x, y]).sort((a, b) => a[0] - b[0]);
  const low = sorted[0][0];
  const high = sorted[sorted.length - 1][0];
  const out = [];
  for (let i = 0; i < 5; i += 1) {
    const x = low + ((high - low) * i) / 4;
    const near = sorted
      .map((point) => [Math.abs(point[0] - x), point])
      .sort((a, b) => a[0] - b[0])
      .slice(0, Math.min(2, sorted.length))
      .map(([, point]) => point);
    out.push([x, near.reduce((sum, point) => sum + point[1], 0) / near.length]);
  }
  return out;
}

function splitLoop(loop, upperCount, lowerCount) {
  const points = loop.map(([x, y]) => [x, y]);
  let left = 0;
  let right = 0;
  points.forEach((point, i) => {
    if (point[0] < points[left][0]) left = i;
    if (point[0] > points[right][0]) right = i;
  });
  const walk = (from, to) => {
    const path = [points[from]];
    for (let i = from; i !== to;) {
      i = (i + 1) % points.length;
      path.push(points[i]);
    }
    return path;
  };
  const one = walk(left, right);
  const two = walk(right, left).reverse();
  const meanY = (path) => path.reduce((sum, point) => sum + point[1], 0) / path.length;
  const [upper, lower] = meanY(one) <= meanY(two) ? [one, two] : [two, one];
  return [resamplePolyline(upper, upperCount), resamplePolyline(lower, lowerCount)];
}

export function resamplePolyline(points, count) {
  if (points.length === 1) return Array.from({ length: count }, () => [...points[0]]);
  const lengths = [0];
  for (let i = 1; i < points.length; i += 1) {
    lengths.push(lengths[i - 1] + Math.hypot(points[i][0] - points[i - 1][0], points[i][1] - points[i - 1][1]));
  }
  const total = lengths[lengths.length - 1] || 1;
  const out = [];
  let segment = 1;
  for (let i = 0; i < count; i += 1) {
    const at = (total * i) / (count - 1);
    while (segment < points.length - 1 && lengths[segment] < at) segment += 1;
    const span = lengths[segment] - lengths[segment - 1] || 1;
    const t = clampValue((at - lengths[segment - 1]) / span, 0, 1);
    const a = points[segment - 1];
    const b = points[segment];
    out.push([a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t]);
  }
  return out;
}

export function clampValue(value, low, high) {
  return Math.min(high, Math.max(low, value));
}

export class OneEuroFilter {
  constructor(minCutoff = 1.2, beta = 0.02, derivativeCutoff = 1) {
    this.minCutoff = minCutoff;
    this.beta = beta;
    this.derivativeCutoff = derivativeCutoff;
    this.value = null;
    this.slope = 0;
    this.time = 0;
  }

  reset() {
    this.value = null;
    this.slope = 0;
  }

  filter(value, time, scale = 1) {
    if (this.value === null || !(time > this.time)) {
      this.value = value;
      this.time = time;
      return value;
    }
    const elapsed = (time - this.time) / 1000;
    this.time = time;
    const smoothing = (cutoff) => {
      const tau = 1 / (2 * Math.PI * cutoff);
      return 1 / (1 + tau / elapsed);
    };
    const slope = (value - this.value) / elapsed;
    this.slope += smoothing(this.derivativeCutoff) * (slope - this.slope);
    const cutoff = this.minCutoff + (this.beta * Math.abs(this.slope)) / scale;
    this.value += smoothing(cutoff) * (value - this.value);
    return this.value;
  }
}

export function createPointSmoother(count, minCutoff = 1.2, beta = 0.6) {
  const filters = Array.from({ length: count * 2 }, () => new OneEuroFilter(minCutoff, beta));
  return {
    reset() {
      for (const filter of filters) filter.reset();
    },
    smooth(points, time, scale) {
      return points.map(([x, y], i) => [
        filters[i * 2].filter(x, time, scale),
        filters[i * 2 + 1].filter(y, time, scale),
      ]);
    },
  };
}
