import { centroid, clampValue, faceFrame, OneEuroFilter } from "./chat-call-face-geometry.js";

const PROBE_SPAN = 120;
const EYE_HALF = [0.62, 0.3];
const EYE_GRID = [18, 10];
const DARK_RATIO = 0.62;
const OPEN_MIN = 0.05;
const CLOSE_START = 0.52;
const CLOSE_FULL = 0.28;
const BASE_RISE_S = 0.8;
const BASE_FALL_S = 6;
const LID_OPEN = 0.9;
const LID_SHUT = 0.6;
const LID_BASE_RISE_S = 1.5;
const LID_BASE_FALL_S = 8;
const LID_MIN = 0.12;
const ENTER = 0.4;
const LEAVE = 0.18;
const PAIRED = 0.75;
const STILL_SPANS_S = 2.2;
const LOOK_LIMIT = 0.32;
const REST_S = 3;
const TONGUE_GRID = [14, 12];
const TONGUE_GATE = [0.1, 0.24];
const TONGUE_BASE_RISE_S = 10;
const UPPER_LIDS = [[37, 41], [38, 40], [43, 47], [44, 46]];
const LOWER_LIFT = 0.15;

export const EYE_CORNERS = Object.freeze([[36, 39], [42, 45]]);

export function lumaOf(r, g, b) {
  return 0.299 * r + 0.587 * g + 0.114 * b;
}

export function rednessOf(r, g, b) {
  return (r - g) / (r + g + b + 12);
}

export function eyeBox(points, side) {
  const [a, b] = EYE_CORNERS[side];
  const p = points[a];
  const q = points[b];
  const dx = q[0] - p[0];
  const dy = q[1] - p[1];
  const width = Math.hypot(dx, dy) || 1;
  const ux = [dx / width, dy / width];
  const uy = [-ux[1], ux[0]];
  const lids = centroid(points, side ? [43, 44, 46, 47] : [37, 38, 40, 41]);
  const mid = [(p[0] + q[0]) / 2, (p[1] + q[1]) / 2];
  const lift = (lids[0] - mid[0]) * uy[0] + (lids[1] - mid[1]) * uy[1];
  return {
    center: [mid[0] + uy[0] * lift * 0.5, mid[1] + uy[1] * lift * 0.5],
    ux,
    uy,
    half: [EYE_HALF[0] * width, EYE_HALF[1] * width],
  };
}

export function tongueWindow(points) {
  const frame = faceFrame(points);
  const lip = [(points[66][0] + points[57][0]) / 2, (points[66][1] + points[57][1]) / 2];
  const chin = points[8];
  const down = [chin[0] - lip[0], chin[1] - lip[1]];
  const reach = Math.hypot(down[0], down[1]) || frame.span * 0.6;
  const mouth = Math.hypot(points[54][0] - points[48][0], points[54][1] - points[48][1]) || frame.span * 0.8;
  const uy = [down[0] / reach, down[1] / reach];
  return {
    center: [lip[0] + down[0] * 0.38, lip[1] + down[1] * 0.38],
    ux: [-uy[1], uy[0]],
    uy,
    half: [mouth * 0.42, reach * 0.55],
  };
}

export function sampleBox(image, box, grid, visit) {
  const { data, width, height } = image;
  for (let j = 0; j < grid[1]; j += 1) {
    const v = ((j + 0.5) / grid[1]) * 2 - 1;
    for (let i = 0; i < grid[0]; i += 1) {
      const u = ((i + 0.5) / grid[0]) * 2 - 1;
      const x = Math.round(box.center[0] + box.ux[0] * u * box.half[0] + box.uy[0] * v * box.half[1]);
      const y = Math.round(box.center[1] + box.ux[1] * u * box.half[0] + box.uy[1] * v * box.half[1]);
      if (x < 0 || y < 0 || x >= width || y >= height) continue;
      const at = (y * width + x) * 4;
      visit(data[at], data[at + 1], data[at + 2], u, v);
    }
  }
}

export function skinReference(image, points) {
  const frame = faceFrame(points);
  let luma = 0;
  let red = 0;
  let count = 0;
  for (const [a, b] of [[2, 31], [14, 35], [3, 48], [13, 54]]) {
    const center = [(points[a][0] + points[b][0]) / 2, (points[a][1] + points[b][1]) / 2];
    const box = { center, ux: frame.ux, uy: frame.uy, half: [frame.span * 0.1, frame.span * 0.1] };
    sampleBox(image, box, [5, 5], (r, g, bl) => {
      luma += lumaOf(r, g, bl);
      red += rednessOf(r, g, bl);
      count += 1;
    });
  }
  return count ? { luma: luma / count, red: red / count } : null;
}

export function measureEye(image, box, skin) {
  const threshold = skin.luma * DARK_RATIO;
  const grid = new Float32Array(EYE_GRID[0] * EYE_GRID[1]).fill(-1);
  let total = 0;
  let dark = 0;
  let wx = 0;
  let wy = 0;
  let weight = 0;
  sampleBox(image, box, EYE_GRID, (r, g, b, u, v) => {
    total += 1;
    const luma = lumaOf(r, g, b);
    const i = Math.round(((v + 1) / 2) * EYE_GRID[1] - 0.5) * EYE_GRID[0] + Math.round(((u + 1) / 2) * EYE_GRID[0] - 0.5);
    grid[i] = luma;
    if (luma >= threshold) return;
    dark += 1;
    const w = (threshold - luma) ** 2;
    wx += w * u;
    wy += w * v;
    weight += w;
  });
  if (!total) return null;
  let edge = 0;
  let edges = 0;
  for (let j = 1; j < EYE_GRID[1] - 1; j += 1) {
    for (let i = 1; i < EYE_GRID[0] - 1; i += 1) {
      const left = grid[j * EYE_GRID[0] + i - 1];
      const right = grid[j * EYE_GRID[0] + i + 1];
      if (left < 0 || right < 0) continue;
      edge += Math.abs(right - left);
      edges += 1;
    }
  }
  return {
    dark: dark / total,
    edge: edges ? edge / edges / Math.max(skin.luma, 1) : 0,
    look: weight > 0 ? [wx / weight, wy / weight] : [0, 0],
  };
}

export function measureTongue(image, box, skin) {
  let total = 0;
  let red = 0;
  sampleBox(image, box, TONGUE_GRID, (r, g, b) => {
    total += 1;
    const luma = lumaOf(r, g, b);
    if (luma < skin.luma * 0.22 || luma > Math.max(skin.luma * 2.4, 60)) return;
    red += clampValue((rednessOf(r, g, b) - skin.red - 0.05) / 0.08, 0, 1);
  });
  return total ? red / total : 0;
}

export function eyeOpenness(points, side) {
  const [a, b] = EYE_CORNERS[side];
  const width = Math.hypot(points[b][0] - points[a][0], points[b][1] - points[a][1]) || 1;
  const pairs = side ? [[43, 47], [44, 46]] : [[37, 41], [38, 40]];
  const gap = pairs.reduce((sum, [u, l]) => sum + Math.hypot(points[u][0] - points[l][0], points[u][1] - points[l][1]), 0);
  return gap / pairs.length / width;
}

export function pairClosure(close) {
  const low = Math.min(close[0], close[1]);
  return close.map((value) => low + (value - low) * (1 - PAIRED));
}

export function closeLids(points, close) {
  if (!close || !(close[0] > 0.01 || close[1] > 0.01)) return points;
  const out = points.map((point) => point.slice());
  UPPER_LIDS.forEach(([upper, lower], k) => {
    const amount = clampValue(close[k < 2 ? 0 : 1], 0, 1);
    if (amount <= 0.01) return;
    const meet = [
      out[lower][0] + (out[upper][0] - out[lower][0]) * LOWER_LIFT,
      out[lower][1] + (out[upper][1] - out[lower][1]) * LOWER_LIFT,
    ];
    out[upper] = [out[upper][0] + (meet[0] - out[upper][0]) * amount, out[upper][1] + (meet[1] - out[upper][1]) * amount, ...out[upper].slice(2)];
    out[lower] = [out[lower][0] + (meet[0] - out[lower][0]) * amount, out[lower][1] + (meet[1] - out[lower][1]) * amount, ...out[lower].slice(2)];
  });
  return out;
}

export function createBlinkTracker() {
  const eyes = [0, 1].map(() => ({ base: 0, lidBase: 0, closing: false, close: new OneEuroFilter(4, 2) }));

  function reset() {
    for (const eye of eyes) {
      eye.base = 0;
      eye.lidBase = 0;
      eye.closing = false;
      eye.close.reset();
    }
  }

  function update({ edges, openness = null, elapsed, time, moving = false }) {
    const raw = [0, 0];
    for (const side of [0, 1]) {
      const eye = eyes[side];
      const edge = edges[side];
      if (!Number.isFinite(edge)) continue;
      let gate = 1;
      const open = openness?.[side];
      if (Number.isFinite(open)) {
        if (!eye.lidBase) eye.lidBase = open;
        else if (open >= eye.lidBase) eye.lidBase += (open - eye.lidBase) * clampValue(elapsed / LID_BASE_RISE_S, 0, 1);
        else if (!eye.closing) eye.lidBase -= (eye.lidBase - open) * clampValue(elapsed / LID_BASE_FALL_S, 0, 1);
        if (eye.lidBase >= LID_MIN) gate = clampValue((LID_OPEN - open / eye.lidBase) / (LID_OPEN - LID_SHUT), 0, 1);
      }
      if (!eye.base) eye.base = edge;
      else if (edge >= eye.base) eye.base += (edge - eye.base) * clampValue(elapsed / BASE_RISE_S, 0, 1);
      else if (!eye.closing) eye.base -= (eye.base - edge) * clampValue(elapsed / BASE_FALL_S, 0, 1);
      let amount = 0;
      if (eye.base >= OPEN_MIN) amount = clampValue((CLOSE_START - edge / eye.base) / (CLOSE_START - CLOSE_FULL), 0, 1);
      raw[side] = moving ? 0 : amount * gate;
    }
    const paired = pairClosure(raw);
    return paired.map((value, side) => {
      const eye = eyes[side];
      if (!Number.isFinite(edges[side])) return 0;
      let amount = value;
      if (eye.closing ? amount < LEAVE : amount < ENTER) {
        eye.closing = false;
        amount = 0;
      } else {
        eye.closing = true;
      }
      return clampValue(eye.close.filter(amount, time), 0, 1);
    });
  }

  return { reset, update };
}

export function createExpressionProbe({ documentRef = globalThis.document } = {}) {
  const canvas = documentRef?.createElement?.("canvas") || null;
  const ctx = canvas?.getContext?.("2d", { willReadFrequently: true }) || null;
  const blinks = createBlinkTracker();
  const eyes = [0, 1].map(() => ({
    lookX: new OneEuroFilter(1.2, 0.8),
    lookY: new OneEuroFilter(1.2, 0.8),
    look: [0, 0],
    rest: null,
  }));
  const tongue = { base: -1, gate: new OneEuroFilter(3, 1) };
  let last = 0;
  let lastMid = null;

  function reset() {
    lastMid = null;
    blinks.reset();
    for (const eye of eyes) {
      eye.lookX.reset();
      eye.lookY.reset();
      eye.look = [0, 0];
      eye.rest = null;
    }
    tongue.base = -1;
    tongue.gate.reset();
    last = 0;
  }

  function grab(source, points) {
    if (!ctx) return null;
    const frame = faceFrame(points);
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
    const pad = frame.span * 0.3;
    minX = Math.floor(minX - pad);
    minY = Math.floor(minY - pad);
    maxX = Math.ceil(maxX + pad);
    maxY = Math.ceil(maxY + pad);
    const scale = Math.min(1, PROBE_SPAN / frame.span);
    const w = Math.max(4, Math.round((maxX - minX) * scale));
    const h = Math.max(4, Math.round((maxY - minY) * scale));
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, w, h);
    ctx.setTransform(scale, 0, 0, scale, -minX * scale, -minY * scale);
    ctx.drawImage(source, 0, 0);
    const image = ctx.getImageData(0, 0, w, h);
    const local = points.map(([x, y]) => [(x - minX) * scale, (y - minY) * scale]);
    return { image, local, scale, origin: [minX, minY] };
  }

  function measure(source, points, time, { pupils = null, lids = false } = {}) {
    if (!points) return null;
    let grabbed = null;
    try {
      grabbed = grab(source, points);
    } catch (_) {
      grabbed = null;
    }
    if (!grabbed) return null;
    const { image, local, scale, origin } = grabbed;
    const skin = skinReference(image, local);
    if (!skin || skin.luma < 18) return null;
    const elapsed = last ? clampValue((time - last) / 1000, 0, 1) : 0;
    last = time;
    const frame = faceFrame(points);
    const speed = lastMid && elapsed > 0 ? Math.hypot(frame.mid[0] - lastMid[0], frame.mid[1] - lastMid[1]) / frame.span / elapsed : 0;
    lastMid = frame.mid;
    const boxes = [eyeBox(local, 0), eyeBox(local, 1)];
    const found = boxes.map((box) => measureEye(image, box, skin));
    const close = blinks.update({
      edges: found.map((seen) => (seen ? seen.edge : NaN)),
      openness: lids ? [eyeOpenness(points, 0), eyeOpenness(points, 1)] : null,
      elapsed,
      time,
      moving: speed > STILL_SPANS_S,
    });
    const look = [[0, 0], [0, 0]];
    for (const side of [0, 1]) {
      const eye = eyes[side];
      const seen = found[side];
      if (!seen) continue;
      const box = boxes[side];
      let gazeAt = seen.look;
      const pupil = pupils?.[side];
      if (pupil) {
        const px = (pupil[0] - origin[0]) * scale - box.center[0];
        const py = (pupil[1] - origin[1]) * scale - box.center[1];
        gazeAt = [(px * box.ux[0] + py * box.ux[1]) / box.half[0], (px * box.uy[0] + py * box.uy[1]) / box.half[1]];
      }
      if (close[side] < 0.4) {
        if (!eye.rest) eye.rest = [gazeAt[0], gazeAt[1]];
        const pull = clampValue(elapsed / REST_S, 0, 1);
        eye.rest = [eye.rest[0] + (gazeAt[0] - eye.rest[0]) * pull, eye.rest[1] + (gazeAt[1] - eye.rest[1]) * pull];
        eye.look = [
          clampValue(eye.lookX.filter(gazeAt[0] - eye.rest[0], time), -LOOK_LIMIT, LOOK_LIMIT),
          clampValue(eye.lookY.filter(gazeAt[1] - eye.rest[1], time), -LOOK_LIMIT, LOOK_LIMIT),
        ];
      }
      look[side] = eye.look;
    }
    const mouthBox = tongueWindow(local);
    const red = measureTongue(image, mouthBox, skin);
    if (tongue.base < 0) tongue.base = red;
    else if (red < tongue.base) tongue.base += (red - tongue.base) * 0.5;
    else tongue.base += (red - tongue.base) * (elapsed / TONGUE_BASE_RISE_S);
    const raised = red - tongue.base;
    const gate = clampValue((raised - TONGUE_GATE[0]) / (TONGUE_GATE[1] - TONGUE_GATE[0]), 0, 1);
    return {
      close,
      look,
      tongue: clampValue(tongue.gate.filter(gate, time), 0, 1),
      skinRed: skin.red,
      skinLuma: skin.luma / 255,
    };
  }

  return { measure, reset };
}
