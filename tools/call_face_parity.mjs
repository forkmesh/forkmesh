import { writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const load = (name) => import(pathToFileURL(resolve(root, "app/public", name)).href);
const G = await load("chat-call-face-geometry.js");
const X = await load("chat-call-face-expression.js");
const M = await load("chat-call-face-mesh.js");
const O = await load("chat-call-outfits.js");
const F = await load("chat-call-faces.js");
const C = await load("chat-call-face-custom.js");
const R = await load("chat-call-face-render.js");

const DIGITS = 1e8;
const WARP_STRIDE = 17;
const MATTE_STRIDE = 997;
const TILE_STRIDE = 257;
const SEQUENCE_LENGTH = 60;
const SEQUENCE_SCALE = 0.5;
const DROPOUT = 33;

const round = (v) => (Number.isFinite(v) ? Math.round(v * DIGITS) / DIGITS : v === Infinity ? 1e300 : v === -Infinity ? -1e300 : null);
const deep = (value) => {
  if (value === null || value === undefined) return null;
  if (typeof value === "number") return round(value);
  if (typeof value === "boolean" || typeof value === "string") return value;
  if (ArrayBuffer.isView(value)) return Array.from(value, round);
  if (Array.isArray(value)) return value.map(deep);
  const out = {};
  for (const [key, item] of Object.entries(value)) if (typeof item !== "function") out[key] = deep(item);
  return out;
};
const fine = (v) => Math.round(v * 1000) / 1000;

function fnv(view) {
  let hash = 0x811c9dc5;
  for (const word of new Uint32Array(view.buffer, view.byteOffset, view.byteLength >> 2)) hash = Math.imul(hash ^ word, 0x01000193) >>> 0;
  return hash;
}

function fnvBytes(bytes) {
  let hash = 0x811c9dc5;
  for (const byte of bytes) hash = Math.imul(hash ^ byte, 0x01000193) >>> 0;
  return hash;
}

function strided(values, stride) {
  const out = [];
  for (let i = 0; i < values.length; i += stride) out.push(values[i]);
  return out;
}

function float32Summary(values, stride) {
  let sum = 0;
  for (const v of values) sum += v;
  return { length: values.length, hash: fnv(values), sum: round(sum), samples: strided(values, stride).map(round) };
}

function xorshift(seed) {
  let s = seed >>> 0;
  return () => {
    s ^= s << 13;
    s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5;
    s >>>= 0;
    return s / 4294967296;
  };
}

function synthetic({ yaw = 0, pitch = 0, roll = 0, scale = 100, cx = 320, cy = 240, open = 0, close = [0, 0], smile = 0 } = {}) {
  const t = F.faceTemplate().map((p) => p.slice());
  for (const [i, w] of [[55, 0.45], [56, 0.8], [57, 1], [58, 0.8], [59, 0.45], [65, 0.8], [66, 1], [67, 0.8]]) t[i][1] += open * 0.35 * w;
  for (const [i, w] of [[5, 0.3], [6, 0.6], [7, 0.85], [8, 1], [9, 0.85], [10, 0.6], [11, 0.3]]) t[i][1] += open * 0.25 * w;
  t[48][0] -= smile * 0.08;
  t[48][1] -= smile * 0.06;
  t[54][0] += smile * 0.08;
  t[54][1] -= smile * 0.06;
  for (const [u, l, side] of [[37, 41, 0], [38, 40, 0], [43, 47, 1], [44, 46, 1]]) {
    const a = close[side] * 0.85;
    t[u][0] += (t[l][0] - t[u][0]) * a;
    t[u][1] += (t[l][1] - t[u][1]) * a;
  }
  const [cyw, syw, cp, sp, cr, sr] = [Math.cos(yaw), Math.sin(yaw), Math.cos(pitch), Math.sin(pitch), Math.cos(roll), Math.sin(roll)];
  return t.map(([x, y, z]) => {
    const x1 = x * cyw + z * syw;
    const z1 = -x * syw + z * cyw;
    const y2 = y * cp - z1 * sp;
    const z2 = y * sp + z1 * cp;
    return [cx + scale * (x1 * cr - y2 * sr), cy + scale * (x1 * sr + y2 * cr), scale * z2];
  });
}

function loop(points, count, rotate) {
  const out = G.resamplePolyline([...points, points[0]], count + 1).slice(0, count);
  return out.slice(rotate).concat(out.slice(0, rotate));
}

function visionRegions(p, { depth = false, scale = 1, medianLine = false, reverseJaw = false, pupils = null } = {}) {
  const dims = depth ? 3 : 2;
  const keep = (point) => point.slice(0, dims).map((v) => fine(v * scale));
  const pick = (idx) => idx.map((i) => p[i].slice(0, dims));
  const shift = (point, dy) => [point[0], point[1] + dy, ...point.slice(2)];
  const band = (idx) => loop([...pick(idx).map((q) => shift(q, -3)), ...pick(idx).map((q) => shift(q, 3)).reverse()], 6, 2);
  const jaw = G.resamplePolyline(pick([...Array(17).keys()]), 11);
  const regions = {
    faceContour: reverseJaw ? jaw : jaw.reverse(),
    leftEyebrow: band([22, 23, 24, 25, 26]),
    rightEyebrow: band([17, 18, 19, 20, 21]),
    leftEye: loop(pick([42, 43, 44, 45, 46, 47]), 8, 3),
    rightEye: loop(pick([36, 37, 38, 39, 40, 41]), 8, 5),
    nose: pick([31, 32, 33, 34, 35]),
    outerLips: loop(pick([48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59]), 14, 4),
    innerLips: loop(pick([60, 61, 62, 63, 64, 65, 66, 67]), 6, 1),
  };
  const crest = G.resamplePolyline(pick([27, 28, 29, 30]), 6);
  if (medianLine) regions.medianLine = crest;
  else regions.noseCrest = crest;
  if (pupils) {
    regions.leftPupil = [pupils[1]];
    regions.rightPupil = [pupils[0]];
  }
  const out = {};
  for (const [name, points] of Object.entries(regions)) out[name] = points.map(keep);
  return out;
}

function eyeCenter(p, side) {
  const [a, b] = side ? [42, 45] : [36, 39];
  return [(p[a][0] + p[b][0]) / 2, (p[a][1] + p[b][1]) / 2];
}

function scene(p, { close = [0, 0], gaze = [[0, 0], [0, 0]], open = 0, tongue = 0 } = {}) {
  const ellipses = [];
  for (const side of [0, 1]) {
    const [a, b] = side ? [42, 45] : [36, 39];
    const width = Math.hypot(p[b][0] - p[a][0], p[b][1] - p[a][1]);
    const center = eyeCenter(p, side);
    const openness = 1 - close[side];
    if (openness > 0.2) {
      ellipses.push([center[0], center[1], 0.5 * width, 0.24 * width * openness, 236, 234, 228]);
      ellipses.push([center[0] + gaze[side][0] * 0.22 * width, center[1] + gaze[side][1] * 0.1 * width, 0.17 * width, Math.min(0.17, 0.2 * openness) * width, 48, 34, 30]);
    } else {
      ellipses.push([center[0], center[1], 0.5 * width, 0.05 * width, 150, 104, 88]);
    }
  }
  const mouthWidth = Math.hypot(p[54][0] - p[48][0], p[54][1] - p[48][1]);
  if (open > 0.15) {
    const mid = [(p[62][0] + p[66][0]) / 2, (p[62][1] + p[66][1]) / 2];
    const gap = Math.hypot(p[66][0] - p[62][0], p[66][1] - p[62][1]);
    ellipses.push([mid[0], mid[1], 0.36 * mouthWidth, Math.max(1, 0.5 * gap), 42, 16, 18]);
  }
  if (tongue > 0) {
    ellipses.push([p[57][0], p[57][1] + 0.12 * mouthWidth, 0.28 * mouthWidth, 0.3 * mouthWidth * tongue, 214, 64, 82]);
  }
  return ellipses.map((e) => e.map((v, i) => (i < 4 ? fine(v) : v)));
}

function paint(ellipses, width, height, scale, origin, salt) {
  const data = new Uint8ClampedArray(width * height * 4);
  for (let py = 0; py < height; py += 1) {
    const wy = (py + 0.5) / scale + origin[1];
    for (let px = 0; px < width; px += 1) {
      const wx = (px + 0.5) / scale + origin[0];
      const noise = (Math.imul(px, 73856093) ^ Math.imul(py, 19349663) ^ Math.imul(salt, 83492791)) & 7;
      let color = [196, 150, 126];
      for (const [cx, cy, rx, ry, r, g, b] of ellipses) {
        const dx = wx - cx;
        const dy = wy - cy;
        if ((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) <= 1) color = [r, g, b];
      }
      const at = (py * width + px) * 4;
      data[at] = color[0] + noise;
      data[at + 1] = color[1] + noise;
      data[at + 2] = color[2] + noise;
      data[at + 3] = 255;
    }
  }
  return data;
}

function probeDocument(crops) {
  return {
    createElement() {
      const canvas = { width: 0, height: 0 };
      let transform = [1, 0, 0, 1, 0, 0];
      let source = null;
      const ctx = {
        setTransform(...m) {
          transform = m;
        },
        fillRect() {},
        drawImage(image) {
          source = image;
        },
        getImageData(x, y, w, h) {
          const scale = transform[0];
          const origin = [-transform[4] / scale, -transform[5] / scale];
          crops.push({ width: w, height: h, scale, origin });
          return { width: w, height: h, data: paint(source.ellipses, w, h, scale, origin, source.salt) };
        },
      };
      canvas.getContext = () => ctx;
      return canvas;
    },
  };
}

function lastCrop(crops, points) {
  const frame = G.faceFrame(points);
  let minX = Infinity;
  let minY = Infinity;
  for (const [x, y] of points) {
    minX = Math.min(minX, x);
    minY = Math.min(minY, y);
  }
  const pad = frame.span * 0.3;
  const crop = crops[crops.length - 1];
  return { width: crop.width, height: crop.height, scale: crop.scale, origin: [Math.floor(minX - pad), Math.floor(minY - pad)], paintOrigin: crop.origin };
}

function anchorsOf(points, yaw) {
  return {
    eyeL: G.centroid(points, G.LEFT_EYE),
    eyeR: G.centroid(points, G.RIGHT_EYE),
    mouth: G.centroid(points, [48, 54, 51, 57]),
    mouthWidth: Math.hypot(points[54][0] - points[48][0], points[54][1] - points[48][1]),
    open: G.clampValue(G.mouthGap(points) * 3, 0, 1),
    yaw,
  };
}

const maya = F.callFaceById("maya");
const mayaHead = F.faceHead(maya);

function headOutputs(head, entry, points, stride = 1, width = 640, height = 480) {
  const depths = G.vertexDepths(entry.model, head.pose);
  const warp = R.warpVertices(entry.mesh, entry.layout, head.points, 512, depths);
  return {
    pose: head.pose,
    jawTrust: head.jawTrust,
    points: strided(head.points, stride),
    depths: strided(depths, stride),
    warp: float32Summary(warp, WARP_STRIDE),
    zone: R.zoneFrame(points, head.pose),
    hair: R.hairRegion(head.pose, width, height),
  };
}

const STATIC_CASES = [
  { name: "neutral", pose: {} },
  { name: "turn-left", pose: { yaw: -0.45 } },
  { name: "turn-right", pose: { yaw: 0.45 } },
  { name: "tilted", pose: { roll: 0.3 } },
  { name: "nodding", pose: { pitch: 0.25 } },
  { name: "mouth-open", pose: { open: 1 } },
  { name: "eyes-closed", pose: { close: [1, 1] } },
  { name: "wink-smile", pose: { close: [1, 0], smile: 1 } },
  { name: "depth-turned", pose: { yaw: 0.3, pitch: -0.1 }, depth: true },
  { name: "depth-open", pose: { open: 0.6, roll: -0.2 }, depth: true },
  { name: "median-line", pose: { yaw: 0.1 }, medianLine: true, reverseJaw: true },
  { name: "far-small", pose: { scale: 40, cx: 100, cy: 90, yaw: 0.2 } },
];

function staticCase(spec) {
  const p = synthetic(spec.pose);
  const regions = visionRegions(p, spec);
  const points = G.landmarksFromRegions(regions);
  const head = G.liveHead(mayaHead.model, points);
  const outfit = O.outfitFrame(points);
  return {
    name: spec.name,
    regions,
    points,
    frame: G.faceFrame(points),
    normalized: G.normalizeShape(points),
    canonical: G.canonicalShape(points),
    mouthGap: G.mouthGap(points),
    noseTurn: G.noseTurn(points),
    hasDepth: G.hasDepth(points),
    pose: G.solvePose(mayaHead.model, points),
    head: headOutputs(head, mayaHead, points, 2),
    eyeBoxes: [X.eyeBox(points, 0), X.eyeBox(points, 1)],
    eyeOpenness: [X.eyeOpenness(points, 0), X.eyeOpenness(points, 1)],
    tongueWindow: X.tongueWindow(points),
    closed: X.closeLids(points, [0.8, 0.3]).slice(36, 48),
    outfit,
    outfitUv: O.outfitUv(outfit, [320, 400]),
    fit: G.fitTemplate(F.faceTemplate(), anchorsOf(points, head.pose.yaw)),
    fitPlain: G.fitTemplate(F.faceTemplate(), { eyeL: G.centroid(points, G.LEFT_EYE), eyeR: G.centroid(points, G.RIGHT_EYE) }),
  };
}

function everyFace() {
  const neutral = G.landmarksFromRegions(visionRegions(synthetic({ yaw: 0.2 })));
  const depth = G.landmarksFromRegions(visionRegions(synthetic({ yaw: -0.25, pitch: 0.1 }), { depth: true }));
  return F.CALL_FACES.map((face) => {
    const entry = F.faceHead(face);
    return {
      id: face.id,
      flat: headOutputs(G.liveHead(entry.model, neutral), entry, neutral, 4),
      depth: headOutputs(G.liveHead(entry.model, depth), entry, depth, 4),
    };
  });
}

function meshSummary(mesh) {
  return {
    count: mesh.count,
    extraStart: mesh.extraStart,
    mouthStart: mesh.mouthStart,
    triangles: mesh.triangles.map((t) => t.v),
    holes: mesh.triangles.map((t) => t.hole),
    mouth: mesh.triangles.map((t) => t.mouth),
    cull: mesh.triangles.map((t) => Boolean(t.cull)),
  };
}

function faceModels() {
  return F.CALL_FACES.map((face) => {
    const entry = F.faceHead(face);
    return {
      id: face.id,
      canonical: F.facePoints(face),
      layout: entry.layout,
      model: entry.model,
      mesh: meshSummary(entry.mesh),
      eyes: R.photoEyes(entry.layout, 512),
    };
  });
}

function sequence() {
  const crops = [];
  const documentRef = probeDocument(crops);
  const probe = X.createExpressionProbe({ documentRef });
  const pointSmoother = G.createLandmarkSmoother();
  const anchorSmoother = F.createAnchorSmoother();
  const outfitSmoother = O.createOutfitSmoother();
  const frames = [];
  const outputs = [];
  let live = null;
  let expression = null;
  for (let k = 0; k < SEQUENCE_LENGTH; k += 1) {
    const phase = (2 * Math.PI * k) / SEQUENCE_LENGTH;
    const blink = k >= 18 && k <= 22 ? [1, 1] : k >= 46 && k <= 48 ? [1, 0] : [0, 0];
    const open = k >= 36 && k <= 50 ? 0.5 + 0.5 * Math.sin(((k - 36) * Math.PI) / 14) : 0;
    const tongue = k >= 40 && k <= 47 ? 1 : 0;
    const gaze = [[0.4 * Math.sin(phase * 2), 0.25 * Math.cos(phase)], [0.4 * Math.sin(phase * 2), 0.25 * Math.cos(phase)]];
    const pose = {
      yaw: 0.35 * Math.sin(phase),
      pitch: 0.12 * Math.sin(phase * 2),
      roll: 0.1 * Math.sin(phase + 1),
      scale: 104 + 8 * Math.sin(phase * 3),
      cx: 320 + 30 * Math.sin(phase),
      cy: 240 + 10 * Math.cos(phase),
      open,
      close: blink,
    };
    const p = synthetic(pose);
    const time = 1000 + k * 42 - (k === 12 ? 42 : 0);
    const pupils = [0, 1].map((side) => {
      const [a, b] = side ? [42, 45] : [36, 39];
      const width = Math.hypot(p[b][0] - p[a][0], p[b][1] - p[a][1]);
      const center = eyeCenter(p, side);
      return [center[0] + gaze[side][0] * 0.22 * width, center[1] + gaze[side][1] * 0.1 * width];
    });
    const regions = k === DROPOUT ? null : visionRegions(p, { scale: SEQUENCE_SCALE, pupils });
    const ellipses = scene(p, { close: blink, gaze, open, tongue });
    frames.push({ time, regions, ellipses });
    const found = F.hostResult({ regions }, SEQUENCE_SCALE, true);
    const points = found.points ? pointSmoother.smooth(found.points, time) : null;
    if (points) live = points;
    const out = { found: found.points ? found.points[30] : null, pupils: found.pupils, points };
    if (points) {
      const before = crops.length;
      expression = probe.measure({ ellipses, salt: k }, points, time, { pupils: found.pupils, lids: true }) || expression;
      out.crop = crops.length > before ? lastCrop(crops, points) : null;
    }
    out.expression = expression ? { close: expression.close, look: expression.look, tongue: expression.tongue, skinRed: expression.skinRed, skinLuma: expression.skinLuma } : null;
    const drawn = live && expression ? X.closeLids(live, expression.close) : live;
    out.drawn = drawn ? drawn.slice(36, 48) : null;
    if (drawn) {
      const head = G.liveHead(mayaHead.model, drawn);
      const depths = G.vertexDepths(mayaHead.model, head.pose);
      out.pose = head.pose;
      out.head = strided(head.points, 5);
      out.depths = strided(depths, 7);
      out.warp = float32Summary(R.warpVertices(mayaHead.mesh, mayaHead.layout, head.points, 512, depths), 211);
      out.tongueBox = X.tongueWindow(drawn);
    }
    out.outfit = outfitSmoother.smooth(live ? O.outfitFrame(live) : null);
    if (points) {
      const anchors = anchorsOf(points, k % 9 === 4 ? null : G.noseTurn(points));
      const smooth = anchorSmoother.smooth(anchors, time);
      out.anchors = smooth;
      out.fit = G.fitTemplate(F.faceTemplate(), smooth);
    }
    outputs.push(out);
  }
  return { scale: SEQUENCE_SCALE, frames, outputs };
}

function filters() {
  const inputs = [];
  const next = xorshift(0x51ed270b);
  let time = 500;
  for (let k = 0; k < 40; k += 1) {
    time += k === 10 || k === 11 ? 0 : 16 + Math.floor(next() * 30);
    inputs.push([fine(10 * Math.sin(k / 4) + next() * 0.8), time, k < 20 ? 1 : fine(20 + next() * 40)]);
  }
  const filter = new G.OneEuroFilter();
  const custom = new G.OneEuroFilter(0.6, 0.3, 2);
  const outputs = inputs.map(([value, t, scale], k) => {
    if (k === 25) {
      filter.reset();
      custom.reset();
    }
    return [filter.filter(value, t, scale), custom.filter(value, t, scale)];
  });
  const blink = X.createBlinkTracker();
  const blinkInputs = [];
  const nextBlink = xorshift(0x0badf00d);
  for (let k = 0; k < 40; k += 1) {
    const shut = k >= 12 && k <= 16 ? 0.15 : k >= 28 && k <= 30 ? 0.3 : 1;
    const edges = [fine(0.4 * shut + 0.02 * nextBlink()), k === 20 ? null : fine(0.38 * (k >= 28 && k <= 30 ? 1 : shut) + 0.02 * nextBlink())];
    const openness = k % 7 === 3 ? null : [fine(0.32 * shut + 0.01 * nextBlink()), fine(0.3 * shut + 0.01 * nextBlink())];
    blinkInputs.push({ edges, openness, elapsed: k === 0 ? 0 : 0.042, time: 1000 + 42 * k, moving: k === 34 });
  }
  const blinkOutputs = blinkInputs.map((input) => blink.update({
    edges: input.edges.map((v) => (v === null ? NaN : v)),
    openness: input.openness,
    elapsed: input.elapsed,
    time: input.time,
    moving: input.moving,
  }));
  return { oneEuro: { inputs, outputs, resetAt: 25 }, blink: { inputs: blinkInputs, outputs: blinkOutputs } };
}

function detection() {
  const next = xorshift(0x9e3779b9);
  const anchors = M.blazeAnchors();
  const scores = new Float32Array(anchors.length);
  const boxes = new Float32Array(anchors.length * 16);
  anchors.forEach(([ax, ay], i) => {
    const near = Math.abs(ax - 0.5) < 0.07 && Math.abs(ay - 0.47) < 0.07;
    scores[i] = near ? 1 + 3 * next() : -9 + 6 * next();
    const r = i * 16;
    boxes[r] = (0.5 - ax) * 128 + (next() - 0.5) * 3;
    boxes[r + 1] = (0.47 - ay) * 128 + (next() - 0.5) * 3;
    boxes[r + 2] = 56 + 8 * next();
    boxes[r + 3] = 60 + 8 * next();
    const keys = [[0.43, 0.4], [0.57, 0.4], [0.5, 0.5], [0.5, 0.58], [0.36, 0.45], [0.64, 0.45]];
    keys.forEach(([kx, ky], k) => {
      boxes[r + 4 + k * 2] = (kx - ax) * 128 + (next() - 0.5) * 2;
      boxes[r + 5 + k * 2] = (ky - ay) * 128 + (next() - 0.5) * 2;
    });
  });
  const decoded = M.decodeDetections(scores, boxes, anchors);
  const none = M.decodeDetections(new Float32Array(anchors.length).fill(-5), boxes, anchors);
  const letterbox = { scale: 640, x: 0, y: (640 - 480) / 2 / 640 };
  return {
    seed: 0x9e3779b9,
    anchorCount: anchors.length,
    anchors: strided(anchors, 37),
    scores: strided(scores, 41),
    boxes: strided(boxes, 113),
    decoded,
    none,
    roi: M.roiFromDetection(decoded, letterbox),
  };
}

function faceMeshCase() {
  const p = synthetic({ yaw: 0.2, roll: 0.1, scale: 110 });
  const next = xorshift(0x2545f491);
  const mesh = Array.from({ length: 468 }, () => [fine(250 + 140 * next()), fine(160 + 170 * next()), 0]);
  const counts = Object.fromEntries(Object.entries(M.MESH_REGIONS).map(([name, idx]) => [name, idx.length]));
  const source = {
    faceContour: G.resamplePolyline(p.slice(0, 17), counts.faceContour),
    rightEyebrow: G.resamplePolyline(p.slice(17, 22), counts.rightEyebrow),
    leftEyebrow: G.resamplePolyline(p.slice(22, 27), counts.leftEyebrow),
    rightEye: loop(p.slice(36, 42), counts.rightEye, 0),
    leftEye: loop(p.slice(42, 48), counts.leftEye, 0),
    noseCrest: G.resamplePolyline(p.slice(27, 31), counts.noseCrest),
    nose: G.resamplePolyline(p.slice(31, 36), counts.nose),
    outerLips: loop(p.slice(48, 60), counts.outerLips, 0),
    innerLips: loop(p.slice(60, 68), counts.innerLips, 0),
  };
  for (const [name, idx] of Object.entries(M.MESH_REGIONS)) {
    idx.forEach((index, j) => {
      const q = source[name][j];
      mesh[index] = [fine(q[0]), fine(q[1]), fine(-q[2])];
    });
  }
  mesh[1] = [fine(p[30][0]), fine(p[30][1]), fine(-p[30][2])];
  return {
    mesh,
    regions: M.meshRegions(mesh),
    landmarks: M.meshToLandmarks(mesh),
    roi: M.roiFromMesh(mesh),
    flat: M.meshToLandmarks(mesh.map(([x, y]) => [x, y])),
  };
}

function customCase() {
  const size = 512;
  const next = xorshift(0x1234abcd);
  const raw = G.pointsFromFlat(F.callFaceById("ingrid").points).map(([x, y]) => [fine(x * 1.7 + 40 + next()), fine(y * 1.6 + 90 + next())]);
  const rotated = raw.map(([x, y]) => [fine(600 + (x - 600) * Math.cos(0.12) - (y - 600) * Math.sin(0.12)), fine(600 + (x - 600) * Math.sin(0.12) + (y - 600) * Math.cos(0.12))]);
  const transform = C.customFaceTransform(rotated);
  const placed = rotated.map((point) => C.applyTransform(transform.matrix, point));
  const pixels = new Uint8ClampedArray(size * size * 4);
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const at = (y * size + x) * 4;
      const dx = x - 256;
      const dy = y - 300;
      const skin = dx * dx / (150 * 150) + dy * dy / (190 * 190) <= 1;
      const hair = !skin && dx * dx / (190 * 190) + (dy + 60) * (dy + 60) / (220 * 220) <= 1;
      const noise = (Math.imul(x, 2654435761) ^ Math.imul(y, 40503)) >>> 27;
      const base = skin ? [204, 152, 128] : hair ? [52, 38, 30] : [30 + (x >> 3), 90, 140 - (y >> 3)];
      pixels[at] = base[0] + noise;
      pixels[at + 1] = base[1] + noise;
      pixels[at + 2] = base[2] + noise;
      pixels[at + 3] = 255;
    }
  }
  const person = new Float32Array(size * size);
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      const dx = x - 256;
      const dy = y - 280;
      const byte = dx * dx / (210 * 210) + dy * dy / (300 * 300) <= 1 ? 250 : ((x * 7 + y * 3) & 31);
      person[y * size + x] = C.smoothstep(0.2, 0.8, byte / 255);
    }
  }
  const polygons = [];
  const documentRef = {
    createElement() {
      let path = [];
      const ctx = {
        beginPath() {
          path = [];
        },
        moveTo(x, y) {
          path.push([x, y]);
        },
        lineTo(x, y) {
          path.push([x, y]);
        },
        closePath() {},
        fill() {},
        getImageData(x0, y0, w, h) {
          polygons.push(path.map((point) => point.slice()));
          const data = new Uint8ClampedArray(w * h * 4);
          for (let y = 0; y < h; y += 1) {
            for (let x = 0; x < w; x += 1) {
              if (G.insidePolygon(path, [x + 0.5, y + 0.5])) data[(y * w + x) * 4] = 255;
            }
          }
          return { data };
        },
      };
      return { getContext: () => ctx };
    },
  };
  const results = {};
  for (const [name, mask] of [["plain", null], ["person", person]]) {
    polygons.length = 0;
    const built = C.buildCustomMatte({ pixels, size, points: placed, person: mask, documentRef });
    results[name] = {
      polygons: polygons.map((polygon) => polygon.length),
      matte: float32Summary(built.matte, MATTE_STRIDE),
      skin: float32Summary(built.skin, MATTE_STRIDE),
      hair: float32Summary(built.hair, MATTE_STRIDE),
      head: built.head,
    };
  }
  const span = G.faceFrame(placed).span;
  const model = C.skinModel(pixels, size, placed, span);
  const likeness = C.skinLikeness(pixels, size, model);
  const blurInput = Float32Array.from({ length: 64 }, (_, i) => ((i * 37) % 11) / 10);
  const flat = placed.flatMap(([x, y]) => [Math.round(x * 10) / 10, Math.round(y * 10) / 10]);
  const face = { id: C.CUSTOM_FACE_ID, label: C.CUSTOM_FACE_LABEL, points: flat, head: results.plain.head };
  const entry = F.faceHead(face);
  return {
    size,
    raw: rotated,
    transform,
    placed,
    flat,
    smoothstep: [[0.2, 0.8, 0.1], [0.2, 0.8, 0.5], [0.03, 0.12, 0.2], [1, 2, 1.5]].map(([a, b, x]) => [a, b, x, C.smoothstep(a, b, x)]),
    blur: { input: Array.from(blurInput), output: Array.from(C.boxBlur(blurInput, 8, 2, 2)) },
    skinModel: model,
    likeness: float32Summary(likeness, MATTE_STRIDE),
    personSamples: float32Summary(person, MATTE_STRIDE),
    results,
    faceModel: { layout: entry.layout, model: entry.model, mesh: meshSummary(entry.mesh) },
  };
}

function recordingDocument(tiles) {
  return {
    createElement() {
      const canvas = { width: 0, height: 0, ops: [] };
      const ops = canvas.ops;
      const styleOf = (style) => {
        if (typeof style === "string") return style;
        if (style?.kind === "pattern") return { pattern: style.tile };
        if (style?.kind === "gradient") return { gradient: style.coords.slice(), stops: style.stops.map((stop) => stop.slice()) };
        return null;
      };
      const ctx = {
        save: () => ops.push(["save"]),
        restore: () => ops.push(["restore"]),
        beginPath: () => ops.push(["beginPath"]),
        closePath: () => ops.push(["closePath"]),
        fill: () => ops.push(["fill"]),
        stroke: () => ops.push(["stroke"]),
        clip: () => ops.push(["clip"]),
        moveTo: (x, y) => ops.push(["moveTo", x, y]),
        lineTo: (x, y) => ops.push(["lineTo", x, y]),
        quadraticCurveTo: (a, b, c, d) => ops.push(["quadTo", a, b, c, d]),
        arc: (x, y, r, s, e) => ops.push(["arc", x, y, r, s, e]),
        fillRect: (x, y, w, h) => ops.push(["fillRect", x, y, w, h]),
        translate: (x, y) => ops.push(["translate", x, y]),
        rotate: (a) => ops.push(["rotate", a]),
        setLineDash: (dash) => ops.push(["lineDash", dash.slice()]),
        createPattern: (source) => ({ kind: "pattern", tile: source.tile }),
        createLinearGradient: (x0, y0, x1, y1) => {
          const gradient = { kind: "gradient", coords: [x0, y0, x1, y1], stops: [] };
          gradient.addColorStop = (offset, color) => gradient.stops.push([offset, color]);
          return gradient;
        },
        createImageData: (w, h) => ({ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }),
        putImageData: (image) => {
          tiles.push(image.data);
          canvas.tile = tiles.length - 1;
        },
      };
      for (const [name, op] of [["fillStyle", "fillStyle"], ["strokeStyle", "strokeStyle"], ["lineWidth", "lineWidth"], ["lineCap", "lineCap"], ["lineJoin", "lineJoin"], ["globalCompositeOperation", "composite"]]) {
        Object.defineProperty(ctx, name, {
          set(value) {
            ops.push([op, name.endsWith("Style") ? styleOf(value) : value]);
          },
        });
      }
      canvas.getContext = () => ctx;
      return canvas;
    },
  };
}

function outfits() {
  const tiles = [];
  const documentRef = recordingDocument(tiles);
  const drawings = O.CALL_OUTFITS.filter((outfit) => outfit.id).map((outfit) => ({ id: outfit.id, ops: deep(O.drawCallOutfit(outfit.id, documentRef).ops) }));
  return {
    catalogue: O.CALL_OUTFITS.map((outfit) => ({ id: outfit.id, label: outfit.label })),
    constants: { size: O.OUTFIT_SIZE, unit: O.OUTFIT_UNIT, neck: O.OUTFIT_NECK, drop: O.OUTFIT_DROP, chinSpans: O.OUTFIT_CHIN_SPANS, roll: O.OUTFIT_ROLL, tile: O.OUTFIT_TILE },
    unknown: O.callOutfitById("nobody"),
    drawings,
    tiles: tiles.map((data) => ({ hash: fnvBytes(data), samples: strided(data, TILE_STRIDE) })),
  };
}

function delaunayCase() {
  const next = xorshift(0x7f4a7c15);
  const points = Array.from({ length: 40 }, () => [fine(next() * 100), fine(next() * 80)]);
  points.push([10, 10], [20, 10], [20, 20], [10, 20]);
  return { points, triangles: G.delaunay(points) };
}

function misc() {
  return {
    constants: {
      landmarkCount: G.LANDMARK_COUNT,
      extraCount: G.EXTRA_COUNT,
      cheekCount: G.CHEEK_COUNT,
      mouthExtraCount: G.MOUTH_EXTRA_COUNT,
      foreheadLift: G.FOREHEAD_LIFT,
      vertexFloats: R.VERTEX_FLOATS,
    },
    outputSize: [[640, 480], [1920, 1080], [0, 0], [333, 777], [1280, 720, 480]].map((args) => [args, F.outputSize(...args)]),
    choices: F.callFaceChoices(),
    custom: [F.isCustomFace("custom"), F.isCustomFace("maya")],
    unknownFace: F.callFaceById("nobody"),
    pupils: [
      F.pupilsFromRegions({ leftPupil: [[300, 200]], rightPupil: [[200, 201]] }, 2),
      F.pupilsFromRegions({ leftPupil: [[300, 200]] }, 1),
    ],
    hostResult: F.hostResult({ regions: { faceContour: [[0, 0]] } }, 1, true),
    vertexRegion: [0, 26, 27, 67, 68, 150, 200].map((v) => [v, R.vertexRegion(v), R.vertexRegion(v, 150)]),
    coverRect: [[1920, 1080, 640, 480], [480, 640, 640, 480], [0, 0, 100, 100]].map((args) => [args, R.coverRect(...args)]),
    lookUniform: [R.lookUniform(null), R.lookUniform([[0.1, -0.2], [0.3, 0.4]]), R.lookUniform([[0.1, NaN], null])],
    axisFrame: R.axisFrame({ center: [10, 20], ux: [0.6, 0.8], uy: [-0.8, 0.6], half: [4, 5] }),
    luma: X.lumaOf(200, 100, 50),
    redness: X.rednessOf(200, 100, 50),
    pairClosure: X.pairClosure([0.9, 0.2]),
    closeNone: X.closeLids([[1, 2]], [0, 0.005]),
    missing: G.landmarksFromRegions({ faceContour: [[0, 0]] }),
    shortCrest: G.landmarksFromRegions({ ...visionRegions(synthetic()), noseCrest: [[1, 2]] }),
    resample: [G.resamplePolyline([[1, 2, 3]], 3), G.resamplePolyline([[0, 0], [3, 4], [3, 4], [6, 8]], 5)],
    ringCount: G.ringCount(mayaHead.model.length),
  };
}

export function fixture() {
  return deep({
    digits: DIGITS,
    faces: F.CALL_FACES.map((face) => ({ id: face.id, label: face.label, points: face.points, head: face.head })),
    template: F.faceTemplate(),
    faceModels: faceModels(),
    misc: misc(),
    delaunay: delaunayCase(),
    statics: STATIC_CASES.map(staticCase),
    everyFace: everyFace(),
    sequence: sequence(),
    filters: filters(),
    detection: detection(),
    faceMesh: faceMeshCase(),
    custom: customCase(),
    outfits: outfits(),
  });
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const text = JSON.stringify(fixture()) + "\n";
  for (const path of process.argv.slice(2)) writeFileSync(path, text);
  if (process.argv.length < 3) process.stdout.write(text);
}
