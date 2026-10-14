import { centroid, clampValue, faceFrame, LANDMARK_COUNT } from "./chat-call-face-geometry.js";

export const CUSTOM_FACE_ID = "custom";
export const CUSTOM_FACE_LABEL = "Your photo";
export const CUSTOM_FACE_STORAGE_KEY = "forkmesh.chat.callFaceImage";
export const CUSTOM_FACE_SIDE = 1024;

const SIZE = 512;
const EYE_SPAN = 118;
const EYE_MID = [256, 285];
const RING = 28;

export function customFaceTransform(points) {
  const frame = faceFrame(points);
  const scale = EYE_SPAN / frame.span;
  const cos = Math.cos(-frame.roll) * scale;
  const sin = Math.sin(-frame.roll) * scale;
  return {
    scale,
    matrix: [cos, sin, -sin, cos, EYE_MID[0] - (cos * frame.mid[0] - sin * frame.mid[1]), EYE_MID[1] - (sin * frame.mid[0] + cos * frame.mid[1])],
  };
}

export function applyTransform(matrix, [x, y]) {
  const [a, b, c, d, e, f] = matrix;
  return [a * x + c * y + e, b * x + d * y + f];
}

export function smoothstep(edge0, edge1, x) {
  const t = clampValue((x - edge0) / (edge1 - edge0), 0, 1);
  return t * t * (3 - 2 * t);
}

export function boxBlur(values, size, radius, passes = 1) {
  let source = values;
  let out = new Float32Array(values.length);
  for (let pass = 0; pass < passes; pass += 1) {
    const temp = new Float32Array(values.length);
    for (let y = 0; y < size; y += 1) {
      let sum = 0;
      const row = y * size;
      for (let x = -radius; x <= radius; x += 1) sum += source[row + clampValue(x, 0, size - 1)];
      for (let x = 0; x < size; x += 1) {
        temp[row + x] = sum / (2 * radius + 1);
        sum += source[row + Math.min(size - 1, x + radius + 1)] - source[row + Math.max(0, x - radius)];
      }
    }
    for (let x = 0; x < size; x += 1) {
      let sum = 0;
      for (let y = -radius; y <= radius; y += 1) sum += temp[clampValue(y, 0, size - 1) * size + x];
      for (let y = 0; y < size; y += 1) {
        out[y * size + x] = sum / (2 * radius + 1);
        sum += temp[Math.min(size - 1, y + radius + 1) * size + x] - temp[Math.max(0, y - radius) * size + x];
      }
    }
    source = out;
    out = new Float32Array(values.length);
  }
  return source;
}

export function headRing(matte, size, mid, span) {
  const center = [mid[0], mid[1] - 0.35 * span];
  const reaches = [];
  const directions = [];
  for (let k = 0; k < RING; k += 1) {
    const angle = (2 * Math.PI * k) / RING - Math.PI / 2;
    const direction = [Math.cos(angle), Math.sin(angle)];
    let reach = 0;
    for (let r = 0; ; r += 1) {
      const x = center[0] + direction[0] * r;
      const y = center[1] + direction[1] * r;
      if (!(x >= 0 && x < size - 1 && y >= 0 && y < size - 1)) break;
      if (matte[Math.floor(y) * size + Math.floor(x)] > 0.03) reach = r;
    }
    reaches.push(reach);
    directions.push(direction);
  }
  const at = (i) => reaches[(i + RING) % RING];
  const widened = reaches.map((_, i) => Math.max(at(i - 1), at(i), at(i + 1)));
  const wide = (i) => widened[(i + RING) % RING];
  const out = [];
  reaches.forEach((reach, i) => {
    const smooth = (wide(i - 2) + wide(i - 1) + wide(i) + wide(i + 1) + wide(i + 2)) / 5;
    const radius = Math.max(smooth, reach, 0.9 * span) + 0.18 * span;
    const x = clampValue(center[0] + directions[i][0] * radius, 0, size);
    const y = clampValue(center[1] + directions[i][1] * radius, 0, size);
    out.push(Math.round(x * 10) / 10, Math.round(y * 10) / 10);
  });
  return out;
}

export function polygonMask(documentRef, polygon, size) {
  const canvas = documentRef.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.fillStyle = "#fff";
  ctx.beginPath();
  polygon.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
  ctx.closePath();
  ctx.fill();
  const data = ctx.getImageData(0, 0, size, size).data;
  const out = new Float32Array(size * size);
  for (let i = 0; i < out.length; i += 1) out[i] = data[i * 4] / 255;
  return out;
}

function chroma(r, g, b) {
  const total = r + g + b + 7.65;
  return [r / total, g / total];
}

export function skinModel(pixels, size, points, span) {
  const samples = [];
  const radius = Math.max(2, Math.round(span * 0.12));
  for (const [a, b] of [[2, 31], [14, 35], [3, 48], [13, 54]]) {
    const cx = Math.round((points[a][0] + points[b][0]) / 2);
    const cy = Math.round((points[a][1] + points[b][1]) / 2);
    for (let y = cy - radius; y < cy + radius; y += 1) {
      for (let x = cx - radius; x < cx + radius; x += 1) {
        if (x < 0 || y < 0 || x >= size || y >= size) continue;
        const at = (y * size + x) * 4;
        samples.push(chroma(pixels[at], pixels[at + 1], pixels[at + 2]));
      }
    }
  }
  if (samples.length < 8) return null;
  const mean = [0, 0];
  for (const [u, v] of samples) {
    mean[0] += u / samples.length;
    mean[1] += v / samples.length;
  }
  let a = 2e-5;
  let b = 0;
  let d = 2e-5;
  for (const [u, v] of samples) {
    a += ((u - mean[0]) ** 2) / samples.length;
    b += ((u - mean[0]) * (v - mean[1])) / samples.length;
    d += ((v - mean[1]) ** 2) / samples.length;
  }
  const det = a * d - b * b || 1e-12;
  return { mean, inverse: [d / det, -b / det, a / det] };
}

export function skinLikeness(pixels, size, model) {
  const out = new Float32Array(size * size);
  if (!model) return out;
  const [ia, ib, id] = model.inverse;
  for (let i = 0; i < out.length; i += 1) {
    const r = pixels[i * 4];
    const g = pixels[i * 4 + 1];
    const bl = pixels[i * 4 + 2];
    const [u, v] = chroma(r, g, bl);
    const du = u - model.mean[0];
    const dv = v - model.mean[1];
    const distance = du * du * ia + 2 * du * dv * ib + dv * dv * id;
    out[i] = Math.exp(-0.5 * distance / 9) * smoothstep(0.03, 0.12, (r + g + bl) / 765);
  }
  return out;
}

export function buildCustomMatte({ pixels, size = SIZE, points, person = null, documentRef }) {
  const p = points;
  const left = centroid(p, [36, 37, 38, 39, 40, 41]);
  const right = centroid(p, [42, 43, 44, 45, 46, 47]);
  const span = Math.hypot(right[0] - left[0], right[1] - left[1]) || EYE_SPAN;
  const center = [(left[0] + right[0]) / 2, (left[1] + right[1]) / 2];
  const forehead = [];
  for (let i = 26; i >= 17; i -= 1) forehead.push([p[i][0], p[i][1] - span * 1.2]);
  const facePoly = p.slice(0, 17).concat(forehead);
  const innerPoly = p.slice(0, 17).concat(p.slice(17, 27).reverse().map(([x, y]) => [x, y - span * 0.15]));
  const chin = p[8];
  const neckHalf = Math.hypot(p[4][0] - p[12][0], p[4][1] - p[12][1]) * 0.5;
  const neckPoly = [p[4], p[12], [p[12][0] + neckHalf * 0.08, chin[1] + span * 1.1], [p[4][0] - neckHalf * 0.08, chin[1] + span * 1.1]];
  const face = boxBlur(polygonMask(documentRef, facePoly, size), size, 3, 2);
  const inner = boxBlur(polygonMask(documentRef, innerPoly, size), size, 2, 2);
  const neckRaw = boxBlur(polygonMask(documentRef, neckPoly, size), size, 5, 2);
  const faceWide = boxBlur(face, size, 6, 2);
  const likeness = boxBlur(skinLikeness(pixels, size, skinModel(pixels, size, p, span)), size, 2, 1);
  const matte = new Float32Array(size * size);
  const neck = new Float32Array(size * size);
  for (let y = 0; y < size; y += 1) {
    const below = smoothstep(chin[1] - span * 0.1, chin[1] + span * 0.2, y);
    const fadeNeck = 1 - smoothstep(chin[1] + span * 0.1, chin[1] + span * 0.7, y);
    const above = 1 - smoothstep(p[4][1], p[5][1] + span * 0.25, y);
    const bottom = 1 - smoothstep(size - span * 0.55, size - span * 0.05, y);
    for (let x = 0; x < size; x += 1) {
      const i = y * size + x;
      neck[i] = neckRaw[i] * fadeNeck;
      const reach = Math.hypot((x - center[0]) / (span * 1.75), (y - center[1] + span * 0.4) / (span * 2.3));
      const near = 1 - smoothstep(0.85, 1.05, reach);
      const hairReach = Math.hypot((x - center[0]) / (span * 1.9), (y - center[1] - span * 0.2) / (span * 2.9));
      const hairZone = (1 - smoothstep(0.85, 1.05, hairReach)) * (1 - smoothstep(chin[1], chin[1] + span * 0.4, y));
      const oval = 1 - smoothstep(0.8, 1, Math.hypot((x - center[0]) / (span * 1.3), (y - center[1] + span * 0.45) / (span * 1.95)));
      const region = person ? Math.max(above * near, face[i], neck[i], hairZone) : Math.max(face[i], neck[i], oval);
      const who = person ? person[i] : 1;
      let value = clampValue(who * region, 0, 1);
      const keepNeck = Math.max(smoothstep(0.08, 0.35, likeness[i]), face[i]);
      value *= 1 - below + below * keepNeck;
      matte[i] = value * bottom;
    }
  }
  const soft = boxBlur(matte, size, 1, 1);
  const skin = new Float32Array(size * size);
  const hair = new Float32Array(size * size);
  for (let i = 0; i < soft.length; i += 1) {
    const m = Math.max(soft[i], matte[i] > 0.97 ? 1 : 0) * (matte[i] > 0.001 || soft[i] > 0.05 ? 1 : 0);
    soft[i] = m;
    const skinLike = smoothstep(0.2, 0.55, likeness[i]);
    skin[i] = clampValue(m * Math.max(skinLike * Math.max(faceWide[i], neck[i]), inner[i] * 0.85), 0, 1);
    hair[i] = clampValue(m * (1 - skinLike) * (1 - inner[i]), 0, 1);
  }
  return { matte: soft, skin: boxBlur(skin, size, 2, 1), hair: boxBlur(hair, size, 1, 1), head: headRing(soft, size, center, span) };
}

export function resizeMask(mask, documentRef) {
  if (!mask) return null;
  const canvas = documentRef.createElement("canvas");
  canvas.width = mask.width;
  canvas.height = mask.height;
  const ctx = canvas.getContext("2d");
  const image = ctx.createImageData(mask.width, mask.height);
  for (let i = 0; i < mask.bytes.length; i += 1) {
    image.data[i * 4] = mask.bytes[i];
    image.data[i * 4 + 1] = mask.bytes[i];
    image.data[i * 4 + 2] = mask.bytes[i];
    image.data[i * 4 + 3] = 255;
  }
  ctx.putImageData(image, 0, 0);
  return canvas;
}

export function buildCustomFace({ image, width, height, points, mask = null, documentRef = globalThis.document }) {
  if (!points || points.length < LANDMARK_COUNT) throw new Error("face_not_found");
  const frame = faceFrame(points);
  if (!(frame.span >= 12)) throw new Error("face_too_small");
  const { matrix } = customFaceTransform(points);
  const photo = documentRef.createElement("canvas");
  photo.width = SIZE;
  photo.height = SIZE;
  const ctx = photo.getContext("2d", { willReadFrequently: true });
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, SIZE, SIZE);
  ctx.setTransform(...matrix);
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(image, 0, 0, width, height);
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  const pixels = ctx.getImageData(0, 0, SIZE, SIZE).data;
  let person = null;
  const maskCanvas = resizeMask(mask, documentRef);
  if (maskCanvas) {
    const layer = documentRef.createElement("canvas");
    layer.width = SIZE;
    layer.height = SIZE;
    const lctx = layer.getContext("2d", { willReadFrequently: true });
    lctx.fillStyle = "#000";
    lctx.fillRect(0, 0, SIZE, SIZE);
    lctx.setTransform(...matrix);
    lctx.drawImage(maskCanvas, 0, 0, width, height);
    const data = lctx.getImageData(0, 0, SIZE, SIZE).data;
    person = new Float32Array(SIZE * SIZE);
    for (let i = 0; i < person.length; i += 1) person[i] = smoothstep(0.2, 0.8, data[i * 4] / 255);
  }
  const placed = points.map((point) => applyTransform(matrix, point));
  const built = buildCustomMatte({ pixels, size: SIZE, points: placed, person, documentRef });
  const matte = documentRef.createElement("canvas");
  matte.width = SIZE;
  matte.height = SIZE;
  const mctx = matte.getContext("2d");
  const out = mctx.createImageData(SIZE, SIZE);
  for (let i = 0; i < built.matte.length; i += 1) {
    out.data[i * 4] = Math.round(built.matte[i] * 255);
    out.data[i * 4 + 1] = Math.round(built.skin[i] * 255);
    out.data[i * 4 + 2] = Math.round(built.hair[i] * 255);
    out.data[i * 4 + 3] = 255;
  }
  mctx.putImageData(out, 0, 0);
  return {
    id: CUSTOM_FACE_ID,
    label: CUSTOM_FACE_LABEL,
    custom: true,
    points: placed.flatMap(([x, y]) => [Math.round(x * 10) / 10, Math.round(y * 10) / 10]),
    head: built.head,
    photo,
    matte,
  };
}

export function savedCustomFaceImage(storage = globalThis.localStorage) {
  try {
    const value = storage?.getItem(CUSTOM_FACE_STORAGE_KEY) || "";
    return value.startsWith("data:image/") ? value : "";
  } catch (_) {
    return "";
  }
}

export function rememberCustomFaceImage(image, storage = globalThis.localStorage) {
  try {
    if (image) storage?.setItem(CUSTOM_FACE_STORAGE_KEY, image);
    else storage?.removeItem(CUSTOM_FACE_STORAGE_KEY);
  } catch (_) {}
}

export async function customFaceFromFile(file, documentRef = globalThis.document, ImageImpl = globalThis.Image) {
  if (!file || !/^image\//.test(file.type || "")) throw new Error("not an image");
  const url = globalThis.URL.createObjectURL(file);
  try {
    const image = new ImageImpl();
    image.src = url;
    await image.decode();
    const w = image.naturalWidth || 1;
    const h = image.naturalHeight || 1;
    const scale = Math.min(1, CUSTOM_FACE_SIDE / Math.max(w, h));
    const canvas = documentRef.createElement("canvas");
    canvas.width = Math.max(1, Math.round(w * scale));
    canvas.height = Math.max(1, Math.round(h * scale));
    canvas.getContext("2d").drawImage(image, 0, 0, canvas.width, canvas.height);
    return canvas.toDataURL("image/jpeg", 0.9);
  } finally {
    globalThis.URL.revokeObjectURL(url);
  }
}
