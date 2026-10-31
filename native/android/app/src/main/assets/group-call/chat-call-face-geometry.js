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
const RESIDUAL_SIGMA = 0.55;
const RESIDUAL_ANCHOR = 0.08;
const FOREHEAD_CURVE = 1.5;
const FOREHEAD_BACK = -0.12;
const RING_DEPTH = -1;
const HEAD_CENTER = [0, -0.2, -1.1];
const RING_START = 78;
const SIDE_REACH = 0.4;
const SIDE_BACK = 0.35;
const JAW_FOLLOW = 0.7;
const JAW_FACING = [0.08, 0.38];
const JAW_NORMAL_Z = 0.55;
const FIELD_SOURCES = [
  ...range(17, 36).map((i) => [i, 1]),
  [36, 1], [39, 1], [42, 1], [45, 1],
];
const CHEEK_SOURCES = [...FIELD_SOURCES, [48, 0.6], [54, 0.6], [57, 0.5]];
const CHEEK_SIDE = [
  [[[2, 0.5], [31, 0.5]], 0.2],
  [[[1, 0.5], [41, 0.5]], 0.16],
  [[[4, 0.5], [48, 0.5]], 0.18],
  [[[3, 0.4], [31, 0.3], [48, 0.3]], 0.2],
  [[[6, 0.5], [59, 0.5]], 0.1],
  [[[40, 0.5], [31, 0.5]], 0.08],
  [[[0, 0.55], [36, 0.45]], 0.1],
];
const CHEEK_MIDDLE = [
  [[[8, 0.5], [57, 0.5]], 0.08],
];
const CHIN_DROP = [[5, 0.3], [6, 0.6], [7, 0.85], [8, 1], [9, 0.85], [10, 0.6], [11, 0.3]];
export const FOREHEAD_LIFT = 0.5;
const MAX_OPEN = 0.42;
const NOSE_TRUST = 0.06;
const RING_GAP = 0.05;
const NOSE = range(27, 36);
const NOSE_RIGID = 0.85;
const DEPTH_SPREAD = 0.04;
const LIP_STEPS = 4;
const MOUTH_FEATHER = 0.14;
const MOUTH_DEEP = 0.4;
const UPPER_INNER = [60, 61, 62, 63, 64];
const LOWER_INNER = [60, 67, 66, 65, 64];
const UPPER_OUTER = [48, 49, 50, 51, 52, 53, 54];
const LOWER_OUTER = [48, 59, 58, 57, 56, 55, 54];
const LIP_KEEP = 0.65;
const CORNER_TAPER = 0.35;
const OPEN_PROFILE = { 55: 0.45, 56: 0.8, 57: 1, 58: 0.8, 59: 0.45, 65: 0.8, 66: 1, 67: 0.8 };
const LIFT_PROFILE = { 61: 0.7, 62: 1, 63: 0.7 };

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
    for (const [i, weight] of Object.entries(OPEN_PROFILE)) local[i][1] += drop * weight;
    for (const [i, weight] of Object.entries(LIFT_PROFILE)) local[i][1] -= drop * 0.08 * weight;
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

function catmullWeights(t) {
  const t2 = t * t;
  const t3 = t2 * t;
  return [0.5 * (-t + 2 * t2 - t3), 0.5 * (2 - 5 * t2 + 3 * t3), 0.5 * (t + 4 * t2 - 3 * t3), 0.5 * (t3 - t2)];
}

function lipSamples(control) {
  const last = control.length - 1;
  const at = (j) => {
    if (j < 0) return [[control[0], 2], [control[1], -1]];
    if (j > last) return [[control[last], 2], [control[last - 1], -1]];
    return [[control[j], 1]];
  };
  const samples = [];
  for (let j = 0; j < last; j += 1) {
    samples.push([[control[j], 1]]);
    for (let s = 1; s < LIP_STEPS; s += 1) {
      const w = catmullWeights(s / LIP_STEPS);
      const weights = new Map();
      [j - 1, j, j + 1, j + 2].forEach((k, n) => {
        for (const [index, scale] of at(k)) weights.set(index, (weights.get(index) || 0) + w[n] * scale);
      });
      samples.push([...weights]);
    }
  }
  samples.push([[control[last], 1]]);
  return samples;
}

function blendWeights(a, b, amount) {
  const weights = new Map();
  for (const [index, w] of a) weights.set(index, (weights.get(index) || 0) + w * (1 - amount));
  for (const [index, w] of b) weights.set(index, (weights.get(index) || 0) + w * amount);
  return [...weights];
}

function buildMouth() {
  const extras = [];
  const reference = (weights) => {
    if (weights.length === 1 && weights[0][1] === 1) return { landmark: weights[0][0], weights };
    extras.push(weights);
    return { extra: extras.length - 1, weights };
  };
  const upper = lipSamples(UPPER_INNER).map(reference);
  const lower = lipSamples(LOWER_INNER).map(reference);
  const contourCount = extras.length;
  const last = upper.length - 1;
  const inner = (from, to, amount) => from.map((ref, k) => (k === 0 || k === last ? ref : reference(blendWeights(ref.weights, to[k].weights, amount))));
  const innerUpper = inner(upper, lower, MOUTH_FEATHER);
  const innerLower = inner(lower, upper, MOUTH_FEATHER);
  const deepUpper = inner(upper, lower, MOUTH_DEEP);
  const deepLower = inner(lower, upper, MOUTH_DEEP);
  return { extras, upper, lower, innerUpper, innerLower, deepUpper, deepLower, contourCount };
}

const MOUTH = buildMouth();
export const MOUTH_EXTRA_COUNT = MOUTH.extras.length;
const CHEEKS = [
  ...CHEEK_SIDE,
  ...CHEEK_SIDE.map(([weights, lift]) => [weights.map(([i, w]) => [MIRROR[i], w]), lift]),
  ...CHEEK_MIDDLE,
];
export const CHEEK_COUNT = CHEEKS.length;
export const EXTRA_COUNT = CHEEK_COUNT + MOUTH_EXTRA_COUNT;

function combine(points, weights) {
  let dims = Infinity;
  for (const [i] of weights) dims = Math.min(dims, points[i].length);
  const out = new Array(dims).fill(0);
  for (const [i, w] of weights) {
    for (let k = 0; k < dims; k += 1) out[k] += w * points[i][k];
  }
  return out;
}

export function mouthExtras(points) {
  return MOUTH.extras.map((weights) => combine(points, weights));
}

export function cheekExtras(points, lift = false) {
  return CHEEKS.map(([weights, bulge]) => {
    const point = combine(points, weights);
    if (lift && point.length > 2) point[2] += bulge;
    return point;
  });
}

export function mouthContour(points) {
  const pick = (ref) => (ref.landmark !== undefined ? points[ref.landmark] : combine(points, ref.weights));
  return [...MOUTH.upper.map(pick), ...MOUTH.lower.slice(1, -1).reverse().map(pick)];
}

export function insidePolygon(polygon, [x, y]) {
  let inside = false;
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i, i += 1) {
    const [xi, yi] = polygon[i];
    const [xj, yj] = polygon[j];
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

export function headLayout(points, ring, forehead = FOREHEAD_LIFT) {
  const frame = faceFrame(points);
  const lift = forehead * frame.span;
  const out = points.map((point) => [point[0], point[1]]);
  for (let i = 17; i <= 26; i += 1) {
    out.push([points[i][0] - frame.uy[0] * lift, points[i][1] - frame.uy[1] * lift]);
  }
  for (const point of ring) out.push([point[0], point[1]]);
  for (const [anchor, point] of sideAnchors(out, ring.length)) {
    out.push([out[anchor][0] + (point[0] - out[anchor][0]) * SIDE_REACH, out[anchor][1] + (point[1] - out[anchor][1]) * SIDE_REACH]);
  }
  return out.concat(cheekExtras(out), mouthExtras(out));
}

function sideAnchors(layout, count) {
  const edge = [...JAW, ...range(LANDMARK_COUNT, LANDMARK_COUNT + 10)];
  return layout.slice(RING_START, RING_START + count).map((point) => {
    let best = edge[0];
    let near = Infinity;
    for (const i of edge) {
      const d = (layout[i][0] - point[0]) ** 2 + (layout[i][1] - point[1]) ** 2;
      if (d < near) {
        near = d;
        best = i;
      }
    }
    return [best, point];
  });
}

export function ringCount(length) {
  return (length - EXTRA_COUNT - RING_START) / 2;
}

function vertexDepth(i, count, anchors) {
  if (i < LANDMARK_COUNT) return DEPTH[i];
  if (i < RING_START) return DEPTH[i - LANDMARK_COUNT + 17] * FOREHEAD_CURVE + FOREHEAD_BACK;
  if (i < RING_START + count) return RING_DEPTH;
  return vertexDepth(anchors[i - RING_START - count][0], count, anchors) - SIDE_BACK;
}

export function headModel(points, ring) {
  const frame = faceFrame(points);
  const layout = headLayout(points, ring);
  const anchors = sideAnchors(layout, ring.length);
  const base = layout.slice(0, layout.length - EXTRA_COUNT).map((point, i) => {
    const [a, b] = toFrame(frame, point);
    return [a / frame.span, b / frame.span, vertexDepth(i, ring.length, anchors)];
  });
  return base.concat(cheekExtras(base, true), mouthExtras(base));
}

function triangleArea(points, [a, b, c]) {
  return (points[b][0] - points[a][0]) * (points[c][1] - points[a][1]) - (points[c][0] - points[a][0]) * (points[b][1] - points[a][1]);
}

export function buildHeadMesh(model, template) {
  const open = fitTemplate(template, { eyeL: [-0.5, 0], eyeR: [0.5, 0], open: 0.6 });
  const start = model.length - EXTRA_COUNT;
  const mouthStart = start + CHEEK_COUNT;
  const base = model.slice(0, start).map((point, i) => (i < LANDMARK_COUNT ? open[i] : [point[0], point[1]]));
  const layout = base.concat(cheekExtras(base), mouthExtras(open));
  const vertex = (ref) => (ref.landmark !== undefined ? ref.landmark : mouthStart + ref.extra);
  const contour = [...MOUTH.upper.map(vertex), ...MOUTH.lower.slice(1, -1).reverse().map(vertex)];
  const onContour = new Set(contour);
  const polygon = contour.map((v) => layout[v]);
  const triangles = delaunay(layout.slice(0, mouthStart + MOUTH.contourCount))
    .filter((v) => !(v.every((i) => onContour.has(i)) && insidePolygon(polygon, centroid(layout, v))))
    .filter((v) => Math.abs(triangleArea(model, v)) > 1e-9)
    .map((v) => ({ v, hole: [0, 0, 0], mouth: false, cull: !v.some((i) => (i >= 48 && i < LANDMARK_COUNT) || i >= mouthStart) }));
  const corners = new Set([UPPER_INNER[0], UPPER_INNER[UPPER_INNER.length - 1]]);
  const add = (v, hole) => {
    if (new Set(v).size < 3) return;
    triangles.push({ v, hole: v.map((index, k) => (corners.has(index) ? Math.min(hole[k], 0.5) : hole[k])), mouth: true });
  };
  const strip = (a, b, ha, hb) => {
    for (let k = 0; k + 1 < a.length; k += 1) {
      const p = vertex(a[k]);
      const q = vertex(a[k + 1]);
      const r = vertex(b[k + 1]);
      const s = vertex(b[k]);
      add([p, q, r], [ha, ha, hb]);
      add([p, r, s], [ha, hb, hb]);
    }
  };
  strip(MOUTH.upper, MOUTH.innerUpper, 0, 1);
  strip(MOUTH.lower, MOUTH.innerLower, 0, 1);
  strip(MOUTH.innerUpper, MOUTH.deepUpper, 1, 2);
  strip(MOUTH.innerLower, MOUTH.deepLower, 1, 2);
  strip(MOUTH.deepUpper, MOUTH.deepLower, 2, 2);
  return { triangles, count: layout.length, extraStart: start, mouthStart };
}

export function hasDepth(live) {
  let low = Infinity;
  let high = -Infinity;
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    if (!POSE_WEIGHTS[i]) continue;
    const z = live[i]?.[2];
    if (!Number.isFinite(z)) return false;
    low = Math.min(low, z);
    high = Math.max(high, z);
  }
  const span = Math.hypot(live[45][0] - live[36][0], live[45][1] - live[36][1]) || 1;
  return high - low > DEPTH_SPREAD * span;
}

function dominantEigenvector(matrix) {
  const a = matrix.map((row) => row.slice());
  const v = a.map((_, i) => a.map((__, j) => (i === j ? 1 : 0)));
  for (let sweep = 0; sweep < 24; sweep += 1) {
    let off = 0;
    for (let p = 0; p < 4; p += 1) for (let q = p + 1; q < 4; q += 1) off += a[p][q] * a[p][q];
    if (off < 1e-18) break;
    for (let p = 0; p < 4; p += 1) {
      for (let q = p + 1; q < 4; q += 1) {
        if (Math.abs(a[p][q]) < 1e-20) continue;
        const theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
        const t = Math.sign(theta || 1) / (Math.abs(theta) + Math.sqrt(theta * theta + 1));
        const c = 1 / Math.sqrt(t * t + 1);
        const s = t * c;
        for (let k = 0; k < 4; k += 1) {
          const akp = a[k][p];
          const akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (let k = 0; k < 4; k += 1) {
          const apk = a[p][k];
          const aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (let k = 0; k < 4; k += 1) {
          const vkp = v[k][p];
          const vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }
  let best = 0;
  for (let i = 1; i < 4; i += 1) if (a[i][i] > a[best][best]) best = i;
  return [v[0][best], v[1][best], v[2][best], v[3][best]];
}

export function solvePoseWithDepth(model, live, weights = POSE_WEIGHTS) {
  let total = 0;
  const mm = [0, 0, 0];
  const lm = [0, 0, 0];
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    const w = weights[i];
    if (!w) continue;
    total += w;
    for (let k = 0; k < 3; k += 1) {
      mm[k] += w * model[i][k];
      lm[k] += w * live[i][k];
    }
  }
  for (let k = 0; k < 3; k += 1) {
    mm[k] /= total;
    lm[k] /= total;
  }
  const S = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    const w = weights[i];
    if (!w) continue;
    for (let a = 0; a < 3; a += 1) {
      for (let b = 0; b < 3; b += 1) S[a][b] += w * (model[i][a] - mm[a]) * (live[i][b] - lm[b]);
    }
  }
  const [[sxx, sxy, sxz], [syx, syy, syz], [szx, szy, szz]] = S;
  const [qw, qx, qy, qz] = dominantEigenvector([
    [sxx + syy + szz, syz - szy, szx - sxz, sxy - syx],
    [syz - szy, sxx - syy - szz, sxy + syx, szx + sxz],
    [szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy],
    [sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz],
  ]);
  const r1 = [qw * qw + qx * qx - qy * qy - qz * qz, 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)];
  const r2 = [2 * (qx * qy + qw * qz), qw * qw - qx * qx + qy * qy - qz * qz, 2 * (qy * qz - qw * qx)];
  const r3 = [2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), qw * qw - qx * qx - qy * qy + qz * qz];
  let num = 0;
  let den = 0;
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    const w = weights[i];
    if (!w) continue;
    const m = [model[i][0] - mm[0], model[i][1] - mm[1], model[i][2] - mm[2]];
    const px = dot3(r1, m);
    const py = dot3(r2, m);
    num += w * ((live[i][0] - lm[0]) * px + (live[i][1] - lm[1]) * py);
    den += w * (px * px + py * py);
  }
  if (!(den > 0) || !(num > 0)) return null;
  const scale = num / den;
  return {
    scale,
    rows: [r1, r2, r3],
    t: [lm[0] - scale * dot3(r1, mm), lm[1] - scale * dot3(r2, mm)],
    roll: Math.atan2(r2[0], r1[0]),
    yaw: Math.asin(clampValue(-r3[0], -1, 1)),
    pitch: Math.asin(clampValue(r3[1], -1, 1)),
    depth: true,
  };
}

export function solvePose(model, live, weights = POSE_WEIGHTS) {
  if (hasDepth(live)) {
    const pose = solvePoseWithDepth(model, live, weights);
    if (pose) return pose;
  }
  let total = 0;
  const mm = [0, 0, 0];
  const lm = [0, 0];
  for (let i = 0; i < LANDMARK_COUNT; i += 1) {
    const w = weights[i];
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
    const w = weights[i];
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
  if (noseDisagrees(model, live, r1, r2, scale)) {
    r1[2] = -r1[2];
    r2[2] = -r2[2];
  }
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

export function noseTurn(points) {
  const frame = faceFrame(points);
  const tip = toFrame(frame, points[30]);
  const bridge = toFrame(frame, points[27]);
  return (tip[0] - bridge[0]) / frame.span;
}

function noseDisagrees(model, live, r1, r2, scale) {
  const seen = noseTurn(live) - (model[30][0] - model[27][0]);
  if (Math.abs(seen) < NOSE_TRUST) return false;
  const turn = (r1[2] * (model[30][2] - model[27][2]));
  return Math.sign(turn) !== Math.sign(seen) && Math.abs(turn) > NOSE_TRUST * 0.5;
}

export function projectPose(pose, point) {
  const [r1, r2] = pose.rows;
  return [pose.t[0] + pose.scale * dot3(r1, point), pose.t[1] + pose.scale * dot3(r2, point)];
}

export function projectOutline(pose, point) {
  const center = projectPose(pose, HEAD_CENTER);
  const c = Math.cos(pose.roll);
  const s = Math.sin(pose.roll);
  const dx = (point[0] - HEAD_CENTER[0]) * pose.scale;
  const dy = (point[1] - HEAD_CENTER[1]) * pose.scale;
  return [center[0] + dx * c - dy * s, center[1] + dx * s + dy * c];
}

export function rigidNose(model, live, pose, out) {
  const rigid = NOSE.map((i) => projectPose(pose, model[i]));
  let sx = 0;
  let sy = 0;
  NOSE.forEach((i, k) => {
    sx += live[i][0] - rigid[k][0];
    sy += live[i][1] - rigid[k][1];
  });
  sx /= NOSE.length;
  sy /= NOSE.length;
  NOSE.forEach((i, k) => {
    const x = rigid[k][0] + sx;
    const y = rigid[k][1] + sy;
    out[i] = [live[i][0] + (x - live[i][0]) * NOSE_RIGID, live[i][1] + (y - live[i][1]) * NOSE_RIGID];
  });
  return out;
}

export function jawTrust(model, pose) {
  const r3 = pose.rows[2];
  return JAW.map((i) => {
    const [x, y] = model[i];
    const facing = dot3(r3, normalize3([x, Math.max(0, y - 1) * 0.8, JAW_NORMAL_Z]));
    return smoothstepValue(JAW_FACING[0], JAW_FACING[1], facing);
  });
}

function residualField(model, sources, at) {
  let wx = 0;
  let wy = 0;
  let weight = RESIDUAL_ANCHOR;
  for (const [i, dx, dy, trust] of sources) {
    if (!trust) continue;
    const d2 = (at[0] - model[i][0]) ** 2 + (at[1] - model[i][1]) ** 2;
    const w = trust * Math.exp(-d2 / (2 * RESIDUAL_SIGMA * RESIDUAL_SIGMA));
    wx += w * dx;
    wy += w * dy;
    weight += w;
  }
  return [wx / weight, wy / weight];
}

function mouthAxes(points) {
  const a = points[48];
  const b = points[54];
  const half = Math.hypot(b[0] - a[0], b[1] - a[1]) / 2;
  if (!(half > 1e-6)) return null;
  const ux = [(b[0] - a[0]) / (2 * half), (b[1] - a[1]) / (2 * half)];
  const mid = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];
  return {
    local: (p) => [((p[0] - mid[0]) * ux[0] + (p[1] - mid[1]) * ux[1]) / half, ((p[0] - mid[0]) * -ux[1] + (p[1] - mid[1]) * ux[0]) / half],
    place: ([s, t]) => [mid[0] + (s * ux[0] - t * ux[1]) * half, mid[1] + (s * ux[1] + t * ux[0]) * half],
  };
}

function lipCurve(local, indices) {
  return indices.map((i) => local[i]).sort((p, q) => p[0] - q[0]);
}

function curveAt(curve, s) {
  if (s <= curve[0][0]) return curve[0][1];
  for (let k = 1; k < curve.length; k += 1) {
    const [s1, t1] = curve[k];
    if (s <= s1) {
      const [s0, t0] = curve[k - 1];
      return s1 - s0 > 1e-9 ? t0 + ((t1 - t0) * (s - s0)) / (s1 - s0) : t1;
    }
  }
  return curve[curve.length - 1][1];
}

export function regularMouth(reference, points) {
  const modelAxes = mouthAxes(reference);
  const liveAxes = mouthAxes(points);
  if (!modelAxes || !liveAxes) return points;
  const ref = [];
  const seen = [];
  for (let i = 48; i < 68; i += 1) {
    ref[i] = modelAxes.local(reference[i]);
    seen[i] = liveAxes.local(points[i]);
  }
  const refUpper = lipCurve(ref, UPPER_OUTER);
  const refLower = lipCurve(ref, LOWER_OUTER);
  const refInnerUpper = lipCurve(ref, UPPER_INNER);
  const refInnerLower = lipCurve(ref, LOWER_INNER);
  const upper = lipCurve(seen, UPPER_OUTER);
  const lower = lipCurve(seen, LOWER_OUTER);
  const innerUpper = lipCurve(seen, UPPER_INNER);
  const innerLower = lipCurve(seen, LOWER_INNER);
  const placed = [];
  for (const i of [49, 50, 51, 52, 53]) placed[i] = [ref[i][0], curveAt(upper, ref[i][0])];
  for (const i of [55, 56, 57, 58, 59]) placed[i] = [ref[i][0], curveAt(lower, ref[i][0])];
  placed[48] = seen[48];
  placed[54] = seen[54];
  const top = lipCurve(placed, UPPER_OUTER);
  const bottom = lipCurve(placed, LOWER_OUTER);
  const reach = Math.max(Math.abs(ref[60][0]), Math.abs(ref[64][0]), 1e-3);
  const centerGap = Math.max(0, curveAt(innerLower, 0) - curveAt(innerUpper, 0));
  const inner = (s) => {
    const roof = curveAt(top, s) + LIP_KEEP * Math.max(0, curveAt(refInnerUpper, s) - curveAt(refUpper, s));
    const floor = curveAt(bottom, s) - LIP_KEEP * Math.max(0, curveAt(refLower, s) - curveAt(refInnerLower, s));
    const seenUpper = curveAt(innerUpper, s);
    const seenLower = curveAt(innerLower, s);
    const a = Math.max(seenUpper, roof);
    const b = Math.min(seenLower, floor);
    if (b <= a) {
      const seam = clampValue((seenUpper + seenLower) / 2, Math.min(roof, floor), Math.max(roof, floor));
      return [seam, seam];
    }
    const middle = (a + b) / 2;
    const room = Math.min(b - a, centerGap * Math.pow(Math.max(0, 1 - (s / reach) ** 2), CORNER_TAPER));
    return [middle - room / 2, middle + room / 2];
  };
  for (const [up, down] of [[61, 67], [62, 66], [63, 65]]) {
    const s = (ref[up][0] + ref[down][0]) / 2;
    const [a, b] = inner(s);
    placed[up] = [s, a];
    placed[down] = [s, b];
  }
  for (const i of [60, 64]) {
    const s = ref[i][0];
    const a = curveAt(top, s);
    const b = curveAt(bottom, s);
    placed[i] = [s, clampValue(seen[i][1], Math.min(a, b), Math.max(a, b))];
  }
  const out = points.slice();
  for (let i = 48; i < 68; i += 1) out[i] = liveAxes.place(placed[i]);
  return out;
}

export function liveHead(model, live) {
  const first = solvePose(model, live);
  const trust = jawTrust(model, first);
  const pose = solvePose(model, live, POSE_WEIGHTS.map((w, i) => (i < JAW.length ? w * trust[i] : w)));
  const settled = jawTrust(model, pose);
  const start = model.length - EXTRA_COUNT;
  const outlineEnd = RING_START + ringCount(model.length);
  const projected = model.map((point, v) => (v >= RING_START && v < outlineEnd ? projectOutline(pose, point) : projectPose(pose, point)));
  const source = ([i, trust]) => [i, live[i][0] - projected[i][0], live[i][1] - projected[i][1], trust];
  const jaw = JAW.map((i) => source([i, settled[i] * JAW_FOLLOW]));
  const field = FIELD_SOURCES.map(source).concat(jaw);
  const cheekField = CHEEK_SOURCES.map(source).concat(jaw);
  const out = live.slice(0, LANDMARK_COUNT).map((point) => [point[0], point[1]]);
  for (const i of JAW) {
    const follow = settled[i] * JAW_FOLLOW;
    const [fx, fy] = residualField(model, cheekField, model[i]);
    out[i] = [
      projected[i][0] + follow * jaw[i][1] + (1 - follow) * fx,
      projected[i][1] + follow * jaw[i][2] + (1 - follow) * fy,
    ];
  }
  rigidNose(model, live, pose, out);
  const mouth = regularMouth(projected, out);
  for (let i = 48; i < LANDMARK_COUNT; i += 1) out[i] = mouth[i];
  for (let v = LANDMARK_COUNT; v < start + CHEEK_COUNT; v += 1) {
    const [fx, fy] = residualField(model, v < start ? field : cheekField, model[v]);
    out.push([projected[v][0] + fx, projected[v][1] + fy]);
  }
  return { points: out.concat(mouthExtras(out)), pose, jawTrust: settled };
}

export function vertexDepths(model, pose) {
  const r3 = pose.rows[2];
  const depths = model.map((point) => dot3(r3, point));
  let jaw = Infinity;
  for (const i of JAW) jaw = Math.min(jaw, depths[i]);
  for (let v = RING_START; v < depths.length - EXTRA_COUNT; v += 1) depths[v] = Math.min(depths[v], jaw - RING_GAP);
  return depths;
}

function smoothstepValue(low, high, value) {
  const t = clampValue((value - low) / (high - low), 0, 1);
  return t * t * (3 - 2 * t);
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
  const dims = [...need.map((name) => regions[name]), crest, regions.nose]
    .every((region) => region.every((point) => Number.isFinite(point[2]))) ? 3 : 2;
  const copy = (point) => point.slice(0, dims);
  const out = new Array(LANDMARK_COUNT);
  let jaw = regions.faceContour.map(copy);
  if (jaw[0][0] > jaw[jaw.length - 1][0]) jaw.reverse();
  resamplePolyline(jaw, 17).forEach((point, i) => { out[i] = point; });
  const brows = [regions.leftEyebrow, regions.rightEyebrow]
    .sort((a, b) => meanX(a) - meanX(b));
  browLine(brows[0].map(copy)).forEach((point, i) => { out[17 + i] = point; });
  browLine(brows[1].map(copy)).forEach((point, i) => { out[22 + i] = point; });
  const ridge = crest.map(copy).sort((a, b) => a[1] - b[1]);
  resamplePolyline(ridge, 4).forEach((point, i) => { out[27 + i] = point; });
  const nose = regions.nose.map(copy);
  const middle = nose.reduce((sum, point) => sum + point[1], 0) / nose.length;
  const base = nose.filter((point) => point[1] >= middle).sort((a, b) => a[0] - b[0]);
  resamplePolyline(base.length >= 2 ? base : nose.sort((a, b) => a[0] - b[0]), 5)
    .forEach((point, i) => { out[31 + i] = point; });
  const eyes = [regions.leftEye.map(copy), regions.rightEye.map(copy)].sort((a, b) => meanX(a) - meanX(b));
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
  const [lipTop, lipBottom] = splitLoop(regions.outerLips.map(copy), 7, 7);
  for (let i = 0; i < 7; i += 1) out[48 + i] = lipTop[i];
  for (let i = 1; i < 6; i += 1) out[60 - i] = lipBottom[i];
  const [innerTop, innerBottom] = splitLoop(regions.innerLips.map(copy), 5, 5);
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
  const sorted = points.map((point) => point.slice()).sort((a, b) => a[0] - b[0]);
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
    const point = [x, near.reduce((sum, item) => sum + item[1], 0) / near.length];
    if (near.every((item) => Number.isFinite(item[2]))) point.push(near.reduce((sum, item) => sum + item[2], 0) / near.length);
    out.push(point);
  }
  return out;
}

function splitLoop(loop, upperCount, lowerCount) {
  const points = loop.map((point) => point.slice());
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
    out.push(a.map((value, k) => value + ((b[k] ?? value) - value) * t));
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
  const filters = Array.from({ length: count * 3 }, () => new OneEuroFilter(minCutoff, beta));
  return {
    reset() {
      for (const filter of filters) filter.reset();
    },
    smooth(points, time, scale) {
      return points.map((point, i) => point.slice(0, 3).map((value, k) => filters[i * 3 + k].filter(value, time, scale)));
    },
  };
}

export const SMOOTH_HEAD = Object.freeze([3, 5, 4]);
export const SMOOTH_GROUPS = Object.freeze([
  Object.freeze({ points: JAW, minCutoff: 1.2, beta: 2, derivativeCutoff: 2 }),
  Object.freeze({ points: range(17, 27), minCutoff: 2, beta: 3, derivativeCutoff: 3 }),
  Object.freeze({ points: NOSE, minCutoff: 1.5, beta: 2, derivativeCutoff: 2 }),
  Object.freeze({ points: range(36, 48), minCutoff: 2.5, beta: 4, derivativeCutoff: 3 }),
  Object.freeze({ points: range(48, 68), minCutoff: 2.5, beta: 6, derivativeCutoff: 4 }),
]);

export class GroupFilter {
  constructor(minCutoff = 1.2, beta = 0.6, derivativeCutoff = 1) {
    this.minCutoff = minCutoff;
    this.beta = beta;
    this.derivativeCutoff = derivativeCutoff;
    this.values = null;
    this.speed = 0;
    this.time = 0;
  }

  reset() {
    this.values = null;
    this.speed = 0;
  }

  filter(values, time, scale = 1) {
    if (this.values === null || this.values.length !== values.length || !(time > this.time)) {
      this.values = values.slice();
      this.time = time;
      return values.slice();
    }
    const elapsed = (time - this.time) / 1000;
    this.time = time;
    const smoothing = (cutoff) => {
      const tau = 1 / (2 * Math.PI * cutoff);
      return 1 / (1 + tau / elapsed);
    };
    let fastest = 0;
    for (let i = 0; i < values.length; i += 1) fastest = Math.max(fastest, Math.abs(values[i] - this.values[i]) / elapsed);
    this.speed += smoothing(this.derivativeCutoff) * (fastest / (scale || 1) - this.speed);
    const k = smoothing(this.minCutoff + this.beta * this.speed);
    for (let i = 0; i < values.length; i += 1) this.values[i] += k * (values[i] - this.values[i]);
    return this.values.slice();
  }
}

function frameFromEyes(left, right) {
  const dx = right[0] - left[0];
  const dy = right[1] - left[1];
  const span = Math.hypot(dx, dy) || 1;
  const ux = [dx / span, dy / span];
  return { mid: [(left[0] + right[0]) / 2, (left[1] + right[1]) / 2], span, ux, uy: [-ux[1], ux[0]] };
}

export function createLandmarkSmoother(head = SMOOTH_HEAD, groups = SMOOTH_GROUPS) {
  const eyes = new GroupFilter(...head);
  const filters = groups.map((group) => ({ points: group.points, filter: new GroupFilter(group.minCutoff, group.beta, group.derivativeCutoff) }));
  return {
    reset() {
      eyes.reset();
      for (const entry of filters) entry.filter.reset();
    },
    smooth(points, time) {
      if (!points || points.length < LANDMARK_COUNT) return points;
      const raw = faceFrame(points);
      const [lx, ly, rx, ry] = eyes.filter([raw.left[0], raw.left[1], raw.right[0], raw.right[1]], time, raw.span);
      const steady = frameFromEyes([lx, ly], [rx, ry]);
      const out = points.map((point) => point.slice(0, 3));
      for (const { points: indices, filter } of filters) {
        const flat = [];
        for (const i of indices) {
          const [a, b] = toFrame(raw, points[i]);
          flat.push(a / raw.span, b / raw.span);
          if (points[i].length > 2) flat.push(points[i][2] / raw.span);
        }
        const smooth = filter.filter(flat, time);
        let at = 0;
        for (const i of indices) {
          const point = fromFrame(steady, smooth[at] * steady.span, smooth[at + 1] * steady.span);
          at += 2;
          if (points[i].length > 2) {
            point.push(smooth[at] * steady.span);
            at += 1;
          }
          out[i] = point;
        }
      }
      return out;
    },
  };
}
