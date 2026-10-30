import { faceFrame } from "./chat-call-face-geometry.js";

export const CALL_OUTFIT_STORAGE_KEY = "forkmesh.chat.callOutfit";
export const OUTFIT_SIZE = 1024;
export const OUTFIT_UNIT = 96;
export const OUTFIT_NECK = Object.freeze([512, 176]);
export const OUTFIT_DROP = 1.35;
export const OUTFIT_CHIN_SPANS = 1.9;
export const OUTFIT_ROLL = 0.3;
export const OUTFIT_TILE = 256;

export const CALL_OUTFITS = Object.freeze([
  Object.freeze({ id: "", label: "Your own clothes" }),
  Object.freeze({ id: "suit", label: "Suit and tie" }),
  Object.freeze({ id: "hoodie", label: "Hoodie" }),
  Object.freeze({ id: "sweater", label: "Cable-knit sweater" }),
  Object.freeze({ id: "denim", label: "Denim jacket" }),
  Object.freeze({ id: "flannel", label: "Flannel shirt" }),
  Object.freeze({ id: "turtleneck", label: "Turtleneck" }),
]);

export function callOutfitById(id) {
  return CALL_OUTFITS.find((outfit) => outfit.id === id) || null;
}

export function savedCallOutfit(storage = globalThis.localStorage) {
  try {
    const id = storage?.getItem(CALL_OUTFIT_STORAGE_KEY) || "";
    return callOutfitById(id) ? id : "";
  } catch (_) {
    return "";
  }
}

export function rememberCallOutfit(id, storage = globalThis.localStorage) {
  try {
    const outfit = callOutfitById(id);
    if (!outfit || !outfit.id) storage?.removeItem(CALL_OUTFIT_STORAGE_KEY);
    else storage?.setItem(CALL_OUTFIT_STORAGE_KEY, outfit.id);
    return true;
  } catch (_) {
    return false;
  }
}

export function outfitFrame(points) {
  if (!points || points.length < 68) return null;
  const frame = faceFrame(points);
  const chin = points[8];
  const unit = Math.max(frame.span, Math.hypot(chin[0] - frame.mid[0], chin[1] - frame.mid[1]) / OUTFIT_CHIN_SPANS);
  const roll = frame.roll * OUTFIT_ROLL;
  const ux = [Math.cos(roll), Math.sin(roll)];
  const uy = [-ux[1], ux[0]];
  const base = [(frame.mid[0] + chin[0]) / 2, chin[1]];
  return {
    center: [base[0] + uy[0] * OUTFIT_DROP * unit, base[1] + uy[1] * OUTFIT_DROP * unit],
    x: [ux[0] / unit, ux[1] / unit],
    y: [uy[0] / unit, uy[1] / unit],
    unit,
    roll,
  };
}

export function outfitUv(frame, pixel) {
  const d = [pixel[0] - frame.center[0], pixel[1] - frame.center[1]];
  const q = [d[0] * frame.x[0] + d[1] * frame.x[1], d[0] * frame.y[0] + d[1] * frame.y[1]];
  return [(OUTFIT_NECK[0] + q[0] * OUTFIT_UNIT) / OUTFIT_SIZE, (OUTFIT_NECK[1] + q[1] * OUTFIT_UNIT) / OUTFIT_SIZE];
}

export function createOutfitSmoother(rate = 0.35) {
  let last = null;
  return {
    smooth(frame) {
      if (!frame) return last;
      if (!last) {
        last = frame;
        return last;
      }
      const mix = (a, b) => [a[0] + (b[0] - a[0]) * rate, a[1] + (b[1] - a[1]) * rate];
      const unit = last.unit + (frame.unit - last.unit) * rate;
      const roll = last.roll + (frame.roll - last.roll) * rate;
      const ux = [Math.cos(roll), Math.sin(roll)];
      const uy = [-ux[1], ux[0]];
      last = { center: mix(last.center, frame.center), x: [ux[0] / unit, ux[1] / unit], y: [uy[0] / unit, uy[1] / unit], unit, roll };
      return last;
    },
    reset() {
      last = null;
    },
  };
}

function hash(x, y, seed) {
  let h = (x * 374761393 + y * 668265263 + seed * 2147483647) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967295;
}

function rgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

function tile(documentRef, shade) {
  const canvas = documentRef.createElement("canvas");
  canvas.width = OUTFIT_TILE;
  canvas.height = OUTFIT_TILE;
  const ctx = canvas.getContext("2d");
  const image = ctx.createImageData(OUTFIT_TILE, OUTFIT_TILE);
  for (let y = 0; y < OUTFIT_TILE; y += 1) {
    for (let x = 0; x < OUTFIT_TILE; x += 1) {
      const [r, g, b] = shade(x, y);
      const at = (y * OUTFIT_TILE + x) * 4;
      image.data[at] = r;
      image.data[at + 1] = g;
      image.data[at + 2] = b;
      image.data[at + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
  return canvas;
}

const wave = (value, period) => Math.sin((value * 2 * Math.PI) / period);
const scaled = (color, k) => color.map((c) => Math.max(0, Math.min(255, c * k)));

const FABRICS = {
  wool: (base) => (x, y) => scaled(base, 0.96 + 0.05 * hash(x, y, 1) + 0.025 * wave(x + y, 4)),
  jersey: (base) => (x, y) => scaled(base, 0.97 + 0.045 * hash(x, y, 2)),
  heather: (base) => (x, y) => {
    const fleck = hash(x >> 1, y, 3);
    return scaled(base, 0.9 + 0.12 * hash(x, y, 4) + (fleck > 0.93 ? 0.16 : fleck < 0.05 ? -0.12 : 0));
  },
  rib: (base) => (x, y) => scaled(base, 0.9 + 0.1 * Math.abs(wave(x, 8)) + 0.04 * hash(x, y, 5)),
  knit: (base) => (x, y) => {
    const cx = (x % 8) - 4;
    const cy = ((y + (Math.abs(cx) > 0 ? Math.abs(cx) : 0)) % 6) / 6;
    return scaled(base, 0.82 + 0.16 * Math.sin(cy * Math.PI) + 0.05 * hash(x, y, 6) - 0.06 * (Math.abs(cx) < 1 ? 1 : 0));
  },
  denim: (base) => (x, y) => {
    const twill = 0.5 + 0.5 * wave(x + y * 2, 6);
    const weft = hash(x, y, 7) > 0.82 ? 1.22 : 1;
    return scaled(base, (0.86 + 0.16 * twill + 0.06 * hash(x >> 2, y, 8)) * weft);
  },
  plaid: (colors) => (x, y) => {
    const band = (v) => {
      const m = v % 128;
      if (m < 36) return 2;
      if (m < 44 || (m >= 76 && m < 84)) return 1;
      if (m >= 100 && m < 104) return 3;
      return 0;
    };
    const a = band(x);
    const b = band(y);
    const pick = (y + x) % 4 < 2 ? a : b;
    const mixed = colors[pick].map((c, i) => (c + colors[a === b ? a : Math.max(a, b)][i]) / 2);
    return scaled(mixed, 0.94 + 0.08 * hash(x, y, 9) + 0.03 * wave(x - y, 4));
  },
};

const tiles = new WeakMap();

function fabric(documentRef, ctx, kind, color) {
  if (!tiles.has(documentRef)) tiles.set(documentRef, new Map());
  const cache = tiles.get(documentRef);
  const key = `${kind}:${color}`;
  if (!cache.has(key)) cache.set(key, tile(documentRef, FABRICS[kind](Array.isArray(color) ? color.map(rgb) : rgb(color))));
  return ctx.createPattern(cache.get(key), "repeat");
}

function at(x, y) {
  return [OUTFIT_NECK[0] + x * OUTFIT_UNIT, OUTFIT_NECK[1] + y * OUTFIT_UNIT];
}

function path(ctx, points, close = true) {
  ctx.beginPath();
  points.forEach(([x, y], i) => {
    const [px, py] = at(x, y);
    if (i === 0) ctx.moveTo(px, py);
    else ctx.lineTo(px, py);
  });
  if (close) ctx.closePath();
}

function curve(ctx, from, steps) {
  ctx.beginPath();
  ctx.moveTo(...at(...from));
  for (const step of steps) {
    if (step.length === 4) ctx.quadraticCurveTo(...at(step[0], step[1]), ...at(step[2], step[3]));
    else ctx.lineTo(...at(...step));
  }
}

function fillWith(ctx, style, draw) {
  ctx.save();
  ctx.fillStyle = style;
  draw();
  ctx.fill();
  ctx.restore();
}

function stroke(ctx, color, widthUnits, draw, dash = null) {
  ctx.save();
  ctx.strokeStyle = color;
  ctx.lineWidth = widthUnits * OUTFIT_UNIT;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  if (dash) ctx.setLineDash(dash.map((d) => d * OUTFIT_UNIT));
  draw();
  ctx.stroke();
  ctx.restore();
}

function shoulderLine(neck) {
  const line = [];
  for (let x = -5.5; x <= -neck; x += 0.25) line.push([x, -0.75 + 0.3 * (-x - 0.9)]);
  return line;
}

function cutTop(ctx, neckHalf, neckline) {
  ctx.save();
  ctx.globalCompositeOperation = "destination-out";
  const left = shoulderLine(neckHalf);
  const right = left.map(([x, y]) => [-x, y]).reverse();
  ctx.beginPath();
  ctx.moveTo(...at(-5.6, -2.2));
  for (const point of left) ctx.lineTo(...at(...point));
  neckline(ctx);
  for (const point of right) ctx.lineTo(...at(...point));
  ctx.lineTo(...at(5.6, -2.2));
  ctx.closePath();
  ctx.fill();
  ctx.restore();
}

function roundNeck(half, depth, top = -0.7) {
  return (ctx) => {
    ctx.lineTo(...at(-half, top));
    ctx.quadraticCurveTo(...at(-half * 0.92, depth), ...at(0, depth));
    ctx.quadraticCurveTo(...at(half * 0.92, depth), ...at(half, top));
  };
}

function soften(ctx) {
  ctx.save();
  ctx.globalCompositeOperation = "source-atop";
  const under = ctx.createLinearGradient(...at(0, -0.6), ...at(0, 1.6));
  under.addColorStop(0, "rgba(0,0,0,0.28)");
  under.addColorStop(1, "rgba(0,0,0,0)");
  ctx.fillStyle = under;
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  for (const side of [-1, 1]) {
    const edge = ctx.createLinearGradient(...at(side * 1.8, 0), ...at(side * 3.4, 0));
    edge.addColorStop(0, "rgba(0,0,0,0)");
    edge.addColorStop(1, "rgba(0,0,0,0.18)");
    ctx.fillStyle = edge;
    const [x0] = at(side * 1.8, 0);
    ctx.fillRect(side < 0 ? 0 : x0, 0, side < 0 ? x0 : OUTFIT_SIZE - x0, OUTFIT_SIZE);
  }
  ctx.restore();
}

function folds(ctx, lines, strength = 0.12) {
  for (const [x0, y0, x1, y1] of lines) {
    stroke(ctx, `rgba(0,0,0,${strength})`, 0.09, () => curve(ctx, [x0, y0], [[(x0 + x1) / 2 + 0.15, (y0 + y1) / 2, x1, y1]]));
    stroke(ctx, `rgba(255,255,255,${strength * 0.5})`, 0.05, () => curve(ctx, [x0 + 0.08, y0], [[(x0 + x1) / 2 + 0.23, (y0 + y1) / 2, x1 + 0.08, y1]]));
  }
}

function button(ctx, x, y, radius, color, rim = "rgba(0,0,0,0.45)") {
  ctx.save();
  ctx.fillStyle = color;
  ctx.strokeStyle = rim;
  ctx.lineWidth = 0.025 * OUTFIT_UNIT;
  ctx.beginPath();
  ctx.arc(...at(x, y), radius * OUTFIT_UNIT, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();
  ctx.restore();
}

function drawSuit(ctx, documentRef) {
  ctx.fillStyle = fabric(documentRef, ctx, "wool", "#33373e");
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  fillWith(ctx, fabric(documentRef, ctx, "jersey", "#eceff3"), () => path(ctx, [[-1.15, -0.8], [0, 3.4], [1.15, -0.8]]));
  fillWith(ctx, fabric(documentRef, ctx, "wool", "#6a2234"), () => path(ctx, [[-0.2, 0.05], [0.2, 0.05], [0.13, 0.42], [-0.13, 0.42]]));
  fillWith(ctx, fabric(documentRef, ctx, "wool", "#5e1d2e"), () => path(ctx, [[-0.13, 0.42], [0.13, 0.42], [0.3, 3.0], [0, 3.35], [-0.3, 3.0]]));
  ctx.save();
  ctx.beginPath();
  path(ctx, [[-0.13, 0.42], [0.13, 0.42], [0.3, 3.0], [0, 3.35], [-0.3, 3.0]]);
  ctx.clip();
  for (let y = 0.2; y < 3.6; y += 0.42) stroke(ctx, "rgba(214,178,120,0.55)", 0.05, () => path(ctx, [[-0.5, y], [0.5, y + 0.5]], false));
  ctx.restore();
  for (const side of [-1, 1]) {
    fillWith(ctx, "#f4f6f9", () => path(ctx, [[side * 0.82, -0.78], [side * 0.18, 0.08], [side * 0.36, 0.58], [side * 1.0, -0.28]]));
    stroke(ctx, "rgba(0,0,0,0.25)", 0.025, () => path(ctx, [[side * 0.18, 0.08], [side * 0.36, 0.58], [side * 1.0, -0.28]], false));
  }
  for (const side of [-1, 1]) {
    const lapel = [[side * 1.15, -0.8], [side * 1.62, 0.95], [side * 1.32, 1.12], [side * 1.5, 1.32], [side * 0.05, 3.42], [side * 0.02, 3.4]];
    fillWith(ctx, fabric(documentRef, ctx, "wool", "#3a3f47"), () => path(ctx, lapel));
    stroke(ctx, "rgba(0,0,0,0.35)", 0.04, () => path(ctx, lapel.slice(0, 5), false));
    stroke(ctx, "rgba(0,0,0,0.3)", 0.12, () => path(ctx, [[side * 1.17, -0.75], [side * 0.08, 3.35]], false));
  }
  stroke(ctx, "rgba(0,0,0,0.4)", 0.03, () => path(ctx, [[0.02, 3.4], [0.12, 9.5]], false));
  button(ctx, 0.1, 3.85, 0.12, "#1d1f23");
  button(ctx, 0.1, 5.05, 0.12, "#1d1f23");
  stroke(ctx, "rgba(0,0,0,0.4)", 0.05, () => path(ctx, [[1.35, 2.3], [2.35, 2.18]], false));
  fillWith(ctx, "#e9edf2", () => path(ctx, [[1.55, 2.29], [1.75, 2.0], [1.92, 2.18], [2.1, 1.98], [2.2, 2.2]]));
  folds(ctx, [[-2.3, 1.6, -2.1, 4.6], [2.2, 1.7, 2.0, 4.5], [-0.9, 3.9, -0.7, 6.0]], 0.1);
  cutTop(ctx, 0.82, roundNeck(0.74, -0.15, -0.82));
  soften(ctx);
}

function drawHoodie(ctx, documentRef) {
  ctx.fillStyle = fabric(documentRef, ctx, "heather", "#8e9299");
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  ctx.save();
  curve(ctx, [-1.85, -0.55], [[-1.5, 1.25, 0, 1.2], [1.5, 1.25, 1.85, -0.55]]);
  ctx.lineTo(...at(1.05, -0.6));
  ctx.quadraticCurveTo(...at(0.9, 0.45), ...at(0, 0.45));
  ctx.quadraticCurveTo(...at(-0.9, 0.45), ...at(-1.05, -0.6));
  ctx.closePath();
  ctx.fillStyle = fabric(documentRef, ctx, "heather", "#9a9ea5");
  ctx.fill();
  ctx.clip();
  const rim = ctx.createLinearGradient(...at(0, -0.4), ...at(0, 1.25));
  rim.addColorStop(0, "rgba(255,255,255,0.12)");
  rim.addColorStop(0.7, "rgba(0,0,0,0)");
  rim.addColorStop(1, "rgba(0,0,0,0.3)");
  ctx.fillStyle = rim;
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  ctx.restore();
  stroke(ctx, "rgba(0,0,0,0.3)", 0.07, () => curve(ctx, [-1.85, -0.55], [[-1.5, 1.25, 0, 1.2], [1.5, 1.25, 1.85, -0.55]]));
  stroke(ctx, "rgba(0,0,0,0.2)", 0.04, () => curve(ctx, [-1.05, -0.6], [[-0.9, 0.45, 0, 0.45], [0.9, 0.45, 1.05, -0.6]]));
  for (const [x0, x1, end] of [[-0.36, -0.44, 3.3], [0.36, 0.3, 3.55]]) {
    button(ctx, x0, 1.0, 0.07, "#6f737a", "rgba(0,0,0,0.5)");
    stroke(ctx, "#e6e2d8", 0.085, () => curve(ctx, [x0, 1.0], [[x0 - 0.05, (1 + end) / 2, x1, end]]));
    stroke(ctx, "#2b2d31", 0.11, () => path(ctx, [[x1, end], [x1, end + 0.3]], false));
  }
  stroke(ctx, "rgba(0,0,0,0.22)", 0.05, () => curve(ctx, [-1.9, 6.4], [[0, 5.8, 1.9, 6.4]]));
  folds(ctx, [[-2.2, 1.4, -1.9, 4.8], [2.1, 1.5, 1.9, 4.6], [-0.9, 2.2, -1.1, 5.2], [1.0, 2.4, 0.8, 5.4]], 0.13);
  cutTop(ctx, 1.05, roundNeck(0.92, 0.05, -0.6));
  soften(ctx);
}

function drawSweater(ctx, documentRef) {
  ctx.fillStyle = fabric(documentRef, ctx, "knit", "#2d3d5e");
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  for (const cx of [-1.15, 0, 1.15]) {
    for (let y = 0.65; y < 10.5; y += 0.7) {
      for (const dir of [-1, 1]) {
        stroke(ctx, "rgba(0,0,0,0.32)", 0.13, () => curve(ctx, [cx - dir * 0.2, y], [[cx + dir * 0.3, y + 0.35, cx + dir * 0.2, y + 0.7]]));
        stroke(ctx, "rgba(150,170,210,0.22)", 0.06, () => curve(ctx, [cx - dir * 0.2, y - 0.03], [[cx + dir * 0.3, y + 0.32, cx + dir * 0.2, y + 0.67]]));
      }
    }
    for (const side of [-1, 1]) stroke(ctx, "rgba(0,0,0,0.28)", 0.05, () => path(ctx, [[cx + side * 0.36, 0.6], [cx + side * 0.36, 10.5]], false));
  }
  ctx.save();
  curve(ctx, [-1.25, -0.72], [[-1.15, 0.48, 0, 0.48], [1.15, 0.48, 1.25, -0.72]]);
  ctx.lineTo(...at(0.92, -0.72));
  ctx.quadraticCurveTo(...at(0.85, 0.1), ...at(0, 0.1));
  ctx.quadraticCurveTo(...at(-0.85, 0.1), ...at(-0.92, -0.72));
  ctx.closePath();
  ctx.fillStyle = fabric(documentRef, ctx, "rib", "#34466b");
  ctx.fill();
  ctx.clip();
  for (let k = -1; k <= 1.001; k += 0.06) {
    const angle = k * 1.25;
    const inner = [Math.sin(angle) * 0.92, 0.1 - (1 - Math.cos(angle)) * 0.75];
    const outer = [Math.sin(angle) * 1.25, 0.48 - (1 - Math.cos(angle)) * 1.0];
    stroke(ctx, "rgba(0,0,0,0.22)", 0.025, () => path(ctx, [inner, outer], false));
  }
  ctx.restore();
  stroke(ctx, "rgba(0,0,0,0.3)", 0.04, () => curve(ctx, [-1.25, -0.72], [[-1.15, 0.48, 0, 0.48], [1.15, 0.48, 1.25, -0.72]]));
  folds(ctx, [[-2.3, 1.4, -2.0, 4.6], [2.2, 1.5, 2.0, 4.6]], 0.1);
  cutTop(ctx, 0.92, roundNeck(0.86, 0.1, -0.72));
  soften(ctx);
}

function drawDenim(ctx, documentRef) {
  ctx.fillStyle = fabric(documentRef, ctx, "denim", "#4b6f9e");
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  const tee = [[-0.95, -0.9], [0.95, -0.9], [0.8, 10.5], [-0.8, 10.5]];
  fillWith(ctx, fabric(documentRef, ctx, "jersey", "#efece5"), () => path(ctx, tee));
  stroke(ctx, "rgba(0,0,0,0.18)", 0.06, () => curve(ctx, [-0.78, -0.75], [[-0.7, 0.3, 0, 0.3], [0.7, 0.3, 0.78, -0.75]]));
  for (const side of [-1, 1]) {
    const edge = [[side * 0.92, -0.2], [side * 0.8, 10.5]];
    stroke(ctx, "rgba(0,0,0,0.35)", 0.14, () => path(ctx, edge.map(([x, y]) => [x - side * 0.02, y]), false));
    stroke(ctx, "#c8873a", 0.025, () => path(ctx, edge.map(([x, y]) => [x + side * 0.12, y]), false), [0.09, 0.07]);
    stroke(ctx, "#c8873a", 0.025, () => path(ctx, [[side * 0.92, 1.45], [side * 3.2, 1.25]], false), [0.09, 0.07]);
    stroke(ctx, "#c8873a", 0.025, () => path(ctx, [[side * 0.92, 1.58], [side * 3.2, 1.38]], false), [0.09, 0.07]);
    const flap = [[side * 1.35, 1.85], [side * 2.45, 1.85], [side * 2.45, 2.25], [side * 1.9, 2.5], [side * 1.35, 2.25]];
    fillWith(ctx, fabric(documentRef, ctx, "denim", "#45679a"), () => path(ctx, flap));
    stroke(ctx, "rgba(0,0,0,0.35)", 0.035, () => path(ctx, flap));
    stroke(ctx, "#c8873a", 0.022, () => path(ctx, flap.map(([x, y]) => [x - side * 0.0, y + 0.07]), true), [0.08, 0.06]);
    stroke(ctx, "rgba(0,0,0,0.25)", 0.03, () => path(ctx, [[side * 1.4, 2.3], [side * 1.4, 3.6], [side * 2.4, 3.6], [side * 2.4, 2.3]], false));
    button(ctx, side * 1.9, 2.3, 0.08, "#b07a3c");
    const collar = [[side * 0.95, -0.95], [side * 1.85, -0.4], [side * 1.5, 1.25], [side * 0.92, 0.45]];
    fillWith(ctx, fabric(documentRef, ctx, "denim", "#557aab"), () => path(ctx, collar));
    stroke(ctx, "rgba(0,0,0,0.38)", 0.04, () => path(ctx, collar));
    stroke(ctx, "#c8873a", 0.022, () => path(ctx, [[side * 1.75, -0.35], [side * 1.43, 1.08], [side * 1.0, 0.45]], false), [0.08, 0.06]);
  }
  for (let y = 1.9; y < 10; y += 1.25) button(ctx, -1.05, y, 0.09, "#b07a3c");
  folds(ctx, [[-2.3, 2.2, -2.0, 5.0], [2.3, 2.2, 2.0, 5.0]], 0.12);
  cutTop(ctx, 0.95, roundNeck(0.72, 0.1, -0.9));
  soften(ctx);
}

function drawFlannel(ctx, documentRef) {
  ctx.fillStyle = fabric(documentRef, ctx, "plaid", ["#a8323a", "#2a2a2e", "#5a1c22", "#e6d9b8"]);
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  stroke(ctx, "rgba(0,0,0,0.3)", 0.03, () => path(ctx, [[-0.2, 0.4], [-0.2, 10.5]], false));
  stroke(ctx, "rgba(0,0,0,0.3)", 0.03, () => path(ctx, [[0.2, 0.4], [0.2, 10.5]], false));
  for (let y = 0.9; y < 10; y += 1.05) button(ctx, 0, y, 0.085, "#ece6d6");
  for (const side of [-1, 1]) {
    const pocket = [[side * 1.25, 1.75], [side * 2.3, 1.75], [side * 2.3, 3.15], [side * 1.78, 3.4], [side * 1.25, 3.15]];
    ctx.save();
    path(ctx, pocket);
    ctx.clip();
    ctx.translate(...at(side * 1.78, 2.5));
    ctx.rotate(Math.PI / 4);
    ctx.fillStyle = fabric(documentRef, ctx, "plaid", ["#a8323a", "#2a2a2e", "#5a1c22", "#e6d9b8"]);
    ctx.fillRect(-200, -200, 400, 400);
    ctx.restore();
    stroke(ctx, "rgba(0,0,0,0.38)", 0.035, () => path(ctx, pocket));
    stroke(ctx, "rgba(0,0,0,0.3)", 0.03, () => path(ctx, [[side * 1.25, 2.1], [side * 2.3, 2.1]], false));
    const collar = [[side * 0.86, -0.85], [side * 1.6, -0.35], [side * 1.0, 1.0], [side * 0.15, 0.45]];
    fillWith(ctx, fabric(documentRef, ctx, "plaid", ["#b23a42", "#303034", "#62202a", "#ecdfbf"]), () => path(ctx, collar));
    stroke(ctx, "rgba(0,0,0,0.4)", 0.04, () => path(ctx, collar));
  }
  folds(ctx, [[-2.3, 1.8, -2.0, 4.8], [2.2, 1.8, 2.0, 4.8]], 0.13);
  cutTop(ctx, 0.86, roundNeck(0.78, 0.25, -0.85));
  soften(ctx);
}

function drawTurtleneck(ctx, documentRef) {
  ctx.fillStyle = fabric(documentRef, ctx, "rib", "#1e1f23");
  ctx.fillRect(0, 0, OUTFIT_SIZE, OUTFIT_SIZE);
  ctx.save();
  curve(ctx, [-0.86, -1.18], [[0, -1.0, 0.86, -1.18], [0.92, -0.72], [0, -0.55, -0.92, -0.72]]);
  ctx.closePath();
  ctx.fillStyle = "rgba(255,255,255,0.07)";
  ctx.fill();
  ctx.restore();
  stroke(ctx, "rgba(0,0,0,0.5)", 0.06, () => curve(ctx, [-0.92, -0.72], [[0, -0.55, 0.92, -0.72]]));
  for (const side of [-1, 1]) stroke(ctx, "rgba(0,0,0,0.35)", 0.12, () => curve(ctx, [side * 0.9, -0.7], [[side * 1.05, -0.2, side * 1.6, -0.05]]));
  folds(ctx, [[-2.2, 1.5, -2.0, 4.8], [2.2, 1.5, 2.0, 4.8], [-0.6, -0.3, -0.5, 0.2], [0.55, -0.35, 0.5, 0.15]], 0.1);
  ctx.save();
  ctx.globalCompositeOperation = "destination-out";
  ctx.beginPath();
  ctx.moveTo(...at(-5.6, -2.2));
  for (const point of shoulderLine(0.86)) ctx.lineTo(...at(...point));
  ctx.lineTo(...at(-0.86, -1.18));
  ctx.quadraticCurveTo(...at(0, -1.0), ...at(0.86, -1.18));
  for (const point of shoulderLine(0.86).map(([x, y]) => [-x, y]).reverse()) ctx.lineTo(...at(...point));
  ctx.lineTo(...at(5.6, -2.2));
  ctx.closePath();
  ctx.fill();
  ctx.restore();
  soften(ctx);
}

const DRAW = {
  suit: drawSuit,
  hoodie: drawHoodie,
  sweater: drawSweater,
  denim: drawDenim,
  flannel: drawFlannel,
  turtleneck: drawTurtleneck,
};

export function drawCallOutfit(id, documentRef = globalThis.document) {
  const draw = DRAW[id];
  if (!draw || !documentRef) return null;
  const canvas = documentRef.createElement("canvas");
  canvas.width = OUTFIT_SIZE;
  canvas.height = OUTFIT_SIZE;
  const ctx = canvas.getContext("2d");
  if (!ctx) return null;
  draw(ctx, documentRef);
  return canvas;
}

export function callOutfitThumbnail(id, documentRef = globalThis.document, width = 160, height = 120) {
  const garment = drawCallOutfit(id, documentRef);
  if (!garment) return "";
  const canvas = documentRef.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d");
  const unit = height / 6.4;
  const neck = [width / 2, height * 0.45];
  const place = (x, y) => [neck[0] + x * unit, neck[1] + y * unit];
  ctx.fillStyle = "#dfe3ea";
  ctx.fillRect(0, 0, width, height);
  ctx.fillStyle = "#b5bcc8";
  ctx.beginPath();
  ctx.ellipse(...place(0, -2.5), 1.15 * unit, 1.45 * unit, 0, 0, Math.PI * 2);
  ctx.fill();
  ctx.fillRect(...place(-0.55, -1.6), 1.1 * unit, 1.9 * unit);
  ctx.save();
  ctx.beginPath();
  ctx.moveTo(...place(-0.9, -0.35));
  ctx.quadraticCurveTo(...place(-2.9, -0.1), ...place(-3.2, 1.2));
  ctx.lineTo(...place(-3.5, 4.5));
  ctx.lineTo(...place(3.5, 4.5));
  ctx.lineTo(...place(3.2, 1.2));
  ctx.quadraticCurveTo(...place(2.9, -0.1), ...place(0.9, -0.35));
  ctx.closePath();
  ctx.clip();
  const scale = unit / OUTFIT_UNIT;
  ctx.drawImage(garment, neck[0] - OUTFIT_NECK[0] * scale, neck[1] - OUTFIT_NECK[1] * scale, OUTFIT_SIZE * scale, OUTFIT_SIZE * scale);
  ctx.restore();
  return canvas.toDataURL("image/png");
}
