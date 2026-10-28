import { clampValue } from "./chat-call-face-geometry.js";

export const PROBE_SIZE = 160;
export const ROI_SIZE = 128;
const MIN_BLOB_SHARE = 0.012;
const LOCK_SPAN = 3;
const LOCK_EYE_V = 0.35;
const LOCK_MISSES = 6;
const SEARCH_RADIUS = 0.3;
const CANDIDATES = 10;
const TEMPLATE_WIDTH = 24;
const TEMPLATE_HEIGHT = 28;
const TEMPLATE_BOX = [-1, 1, -0.55, 1.7833];
const TEMPLATE_STRETCH = [0.88, 1, 1.14];
const MIN_LIKENESS = 0.2;
const ACQUIRE_LIKENESS = 0.25;
const KEEP_LIKENESS = 0.2;
const YAW_REACH = 1.15;
const PAIR_CHOICES = 4;
const MOUTH_WEIGHT = 1.2;
const FOREHEAD_WEIGHT = 0.8;
const VERIFY_SPAN = 4;
const VERIFY_DROP = 0.45;
const VERIFY_ROUNDS = 3;
const CONFIRM_SHIFT = 0.25;
const REVERIFY_EVERY = 16;
const REVERIFY_SHIFT = 0.4;
const FACE_TEMPLATE_TEXT = (
  "RFx9mKq6ytHV3NvV1dvc1dHKuqqYfVxERVyCobK/w8TN1tTR0dTWzcTDv7KhglxFSlyAg4B+e4Sft77Cwr63n4R7foCDgFxKUVdiY11KOTxdhaa2tqaFXTw5Sl1jYldRUktjcG5nV01WcZSvr5RxVk1XZ25wY0tSUktcRzw3LjFFYo25uY1iRTEuNzxHXEtSWVtOHSwAC0NAZJbW1pZkQEMLACwdTltZc4SAa3hfZX53lLvs7LuUd35lX3hrgIRzhLK+noR8dn6bttLs7NK2m352fISevrKEkMne0LKkp7zPzNrz89rMz7ynpLLQ3smQls7p8Ovk6Ove1d/5+d/V3uvo5Ovw6c6Wk8fl9/379+PJ1OX//+XUyeP3+/335ceTi8Hg8Pfy3L/I3+nz8+nfyL/c8vfw4MGLdrHN19jMsq2ip7G5ubGnoq2yzNjXzbF2ZZq2vbegnq+JXGBmZmBcia+eoLe9tpplXIGZn5OMoramfmVZWWV+praijJOfmYFcWXCJkImRpbS1oo9/f4+itbSlkYmQiXBZXmqDlpSEgpKhpqWmpqWmoZKChJSWg2peZGVzn6F6VFVocXl5eXlxaFVUeqGfc2VkXlVjk6aNfHCJmJ+YmJ+YiXB8jaaTY1VeTklXc52cnZWbqK++vq+om5WdnJ1zV0lOOURJWo6gqaKQkZCRkZCRkKKpoI5aSUQ5MDdFVnGXp6malpeXl5eWmqmnl3FWRTcwLTNGS12Boq6ooZ6fn56hqK6igV1LRjMtMjc/P01olam4vsDDw8C+uKmVaE0/PzcyNz5AOjhLep2wu8DAwMC7sJ16Szg6QD43OUBGOjExToSZo6Snp6SjmYROMTE6RkA5OkRKPjUxM1N1goaHh4aCdVMzMTU+SkQ6");

const DARK_TEMPLATE_TEXT = (
  "Vj4cGRcSChAMAQMLCwMBDBAKEhcZHD5WVzwZGBIGChkJABAdHRAACRkKBhIYGTxXVEgdIzAzRD4bCBUhIRUIGz5EMzAjHUhUU0YsRlN4l5ZiMxMFBRMzYpaXeFNGLEZTU0MtFw8VL0dEOiMMDCM6REcvFQ8XLUNTXEgqPElTWWdhWTAJCTBZYWdZU0k8KkhcXkBbt4z/3mKBbTMHBzNtgWLe/4y3W0BeUCAvOxMxKxArKRMGBhMpKxArMRM7LyBQUQ4EDio2PjgeDwgFBQgPHjg+NioOBA5RTwgBCSM3LRICEA0FBQ0QAhItNyMJAQhPSwUAAAEDAAAGEA4GBg4QBgAAAwEAAAVLTgsAAAAAAAYZCggDAwgKGQYAAAAAAAtOTgwAAAAACR4SAgICAgICEh4JAAAAAAxOZhABAAANJSc1MCAXFyAwNSclDQAAARBmcRgEAggyMhNToo14eI2iUxMyMggCBBhxaioSCiQ6GAUUTXmQkHlNFAUYOiQKEipqWzQZDRsXBgEDDxsoKBsPAwEGFxsNGTRbSjgWAwofLiMaGhcRERcaGiMuHwoDFjhKQTksAQIzgYFpXldZWVdeaYGBMwIBLDlBPkhGCgQwT1k7NTlERDk1O1lPMAQKRkg+SktQMAUWExwUCAcDAwcIFBwTFgUwUEtKUThSSw0IAQ0gISovLyohIA0BCA1LUjhRU0U7RDQMAQQiMz1ERD0zIgQBDDREO0VTUkEsPEokCAMSJzM3NzMnEgMIJEo8LEFSQTcqPEs3EwsFCg8REQ8KBQsTN0s8KjdBMBwdPV1UGA0ODAwNDQwMDg0YVF09HRwwOxsXMFt+VxkTFiUtLSUWExlXflswFxs7RSMTLT5lhGBBQUVKSkVBQWCEZT4tEyNF");
const TEMPLATE_WEIGHTS = templateWeights();
const FACE_TEMPLATES = [decodeTemplate(FACE_TEMPLATE_TEXT)];
const DARK_TEMPLATES = [decodeTemplate(DARK_TEMPLATE_TEXT)];

function templateWeights() {
  const [a0, a1, b0, b1] = TEMPLATE_BOX;
  const weights = new Float32Array(TEMPLATE_WIDTH * TEMPLATE_HEIGHT);
  for (let y = 0; y < TEMPLATE_HEIGHT; y += 1) {
    const b = b0 + ((y + 0.5) / TEMPLATE_HEIGHT) * (b1 - b0);
    for (let x = 0; x < TEMPLATE_WIDTH; x += 1) {
      const a = a0 + ((x + 0.5) / TEMPLATE_WIDTH) * (a1 - a0);
      weights[y * TEMPLATE_WIDTH + x] = Math.exp(-0.5 * ((a / 0.62) ** 2 + ((b - 0.55) / 0.85) ** 2));
    }
  }
  return weights;
}

export function flattenPatch(values, weights, width, height) {
  let sw = 0;
  let sx = 0;
  let sy = 0;
  let sxx = 0;
  let syy = 0;
  let sxy = 0;
  let sv = 0;
  let svx = 0;
  let svy = 0;
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const i = y * width + x;
      const w = weights[i];
      const v = values[i];
      sw += w;
      sx += w * x;
      sy += w * y;
      sxx += w * x * x;
      syy += w * y * y;
      sxy += w * x * y;
      sv += w * v;
      svx += w * v * x;
      svy += w * v * y;
    }
  }
  const mx = sx / sw;
  const my = sy / sw;
  const mv = sv / sw;
  const cxx = sxx / sw - mx * mx;
  const cyy = syy / sw - my * my;
  const cxy = sxy / sw - mx * my;
  const cvx = svx / sw - mv * mx;
  const cvy = svy / sw - mv * my;
  const det = cxx * cyy - cxy * cxy || 1;
  const gx = (cvx * cyy - cvy * cxy) / det;
  const gy = (cvy * cxx - cvx * cxy) / det;
  const out = new Float32Array(values.length);
  let norm = 0;
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const i = y * width + x;
      const v = (values[i] - mv - gx * (x - mx) - gy * (y - my)) * weights[i];
      out[i] = v;
      norm += v * v;
    }
  }
  norm = Math.sqrt(norm);
  if (norm < 1e-6) return null;
  for (let i = 0; i < out.length; i += 1) out[i] /= norm;
  return out;
}

function decodeTemplate(text) {
  const raw = globalThis.atob(text);
  const values = new Float32Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) values[i] = raw.charCodeAt(i);
  return flattenPatch(values, TEMPLATE_WEIGHTS, TEMPLATE_WIDTH, TEMPLATE_HEIGHT);
}

export function toYCbCr(data, count) {
  const y = new Float32Array(count);
  const cb = new Float32Array(count);
  const cr = new Float32Array(count);
  for (let i = 0; i < count; i += 1) {
    const r = data[i * 4];
    const g = data[i * 4 + 1];
    const b = data[i * 4 + 2];
    y[i] = 0.299 * r + 0.587 * g + 0.114 * b;
    cb[i] = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b;
    cr[i] = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b;
  }
  return { y, cb, cr };
}

export function createSkinModel() {
  return { ready: false, cb: 108, cr: 150, vcb: 60, vcr: 60, cov: 0 };
}

export function isSkin(model, y, cb, cr) {
  if (y < 30 || cr < 128 || cr > 185 || cb < 68 || cb > 135) return false;
  if (!model.ready) return cb >= 77 && cb <= 127 && cr >= 133 && cr <= 173;
  const a = cb - model.cb;
  const b = cr - model.cr;
  const det = model.vcb * model.vcr - model.cov * model.cov;
  if (det <= 1e-6) return false;
  const d2 = (a * a * model.vcr - 2 * a * b * model.cov + b * b * model.vcb) / det;
  return d2 < 7.5;
}

export function learnSkin(model, samples, rate = 0.15) {
  if (samples.length < 12) return model;
  let mcb = 0;
  let mcr = 0;
  for (const [cb, cr] of samples) {
    mcb += cb;
    mcr += cr;
  }
  mcb /= samples.length;
  mcr /= samples.length;
  let vcb = 0;
  let vcr = 0;
  let cov = 0;
  for (const [cb, cr] of samples) {
    vcb += (cb - mcb) ** 2;
    vcr += (cr - mcr) ** 2;
    cov += (cb - mcb) * (cr - mcr);
  }
  vcb = Math.max(9, vcb / samples.length) * 1.8;
  vcr = Math.max(9, vcr / samples.length) * 1.8;
  cov = (cov / samples.length) * 1.8;
  const mix = model.ready ? rate : 1;
  model.cb += (mcb - model.cb) * mix;
  model.cr += (mcr - model.cr) * mix;
  model.vcb += (vcb - model.vcb) * mix;
  model.vcr += (vcr - model.vcr) * mix;
  model.cov += (cov - model.cov) * mix;
  model.ready = true;
  return model;
}

export function skinMask(image, model) {
  const { width, height, data } = image;
  const count = width * height;
  const raw = new Uint8Array(count);
  for (let i = 0; i < count; i += 1) {
    const r = data[i * 4];
    const g = data[i * 4 + 1];
    const b = data[i * 4 + 2];
    const y = 0.299 * r + 0.587 * g + 0.114 * b;
    const cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b;
    const cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b;
    raw[i] = isSkin(model, y, cb, cr) ? 1 : 0;
  }
  const mask = new Uint8Array(count);
  for (let y = 1; y < height - 1; y += 1) {
    for (let x = 1; x < width - 1; x += 1) {
      let sum = 0;
      for (let dy = -1; dy <= 1; dy += 1) {
        const row = (y + dy) * width + x;
        sum += raw[row - 1] + raw[row] + raw[row + 1];
      }
      mask[y * width + x] = sum >= 5 ? 1 : 0;
    }
  }
  return mask;
}

export function faceBlob(mask, width, height, prefer = null) {
  const labels = new Int32Array(width * height);
  const stack = new Int32Array(width * height);
  let best = null;
  let label = 0;
  for (let start = 0; start < mask.length; start += 1) {
    if (!mask[start] || labels[start]) continue;
    label += 1;
    let top = 0;
    stack[top++] = start;
    labels[start] = label;
    let count = 0;
    let sx = 0;
    let sy = 0;
    let minY = height;
    let maxY = 0;
    while (top) {
      const at = stack[--top];
      const x = at % width;
      const y = (at - x) / width;
      count += 1;
      sx += x;
      sy += y;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
      const near = [at - 1, at + 1, at - width, at + width];
      const valid = [x > 0, x < width - 1, y > 0, y < height - 1];
      for (let k = 0; k < 4; k += 1) {
        const next = near[k];
        if (valid[k] && mask[next] && !labels[next]) {
          labels[next] = label;
          stack[top++] = next;
        }
      }
    }
    if (count < width * height * MIN_BLOB_SHARE) continue;
    const cx = sx / count / width;
    const cy = sy / count / height;
    let score = count * (1.2 - Math.abs(cx - 0.5)) * (1.2 - cy * 0.5);
    if (prefer) score *= 1 / (1 + 6 * Math.hypot(cx - prefer[0], cy - prefer[1]));
    if (!best || score > best.score) best = { label, score, count, minY, maxY };
  }
  if (!best) return null;
  const rows = [];
  const span = best.maxY - best.minY + 1;
  for (let y = best.minY; y <= best.maxY; y += 1) {
    let low = -1;
    let high = -1;
    for (let x = 0; x < width; x += 1) {
      if (labels[y * width + x] !== best.label) continue;
      if (low < 0) low = x;
      high = x;
    }
    rows.push(low < 0 ? null : [low, high]);
  }
  const upper = rows
    .slice(Math.floor(span * 0.12), Math.max(Math.floor(span * 0.12) + 1, Math.floor(span * 0.6)))
    .filter(Boolean);
  if (!upper.length) return null;
  const widths = upper.map(([low, high]) => high - low + 1).sort((a, b) => a - b);
  const faceWidth = widths[Math.floor(widths.length * 0.6)];
  const centerX = upper.reduce((sum, [low, high]) => sum + (low + high) / 2, 0) / upper.length;
  const faceTop = best.minY;
  return {
    labels,
    label: best.label,
    width: faceWidth,
    cx: centerX,
    top: faceTop,
    bottom: best.maxY,
  };
}

function boxBlur(src, width, height, radius) {
  const tmp = new Float32Array(src.length);
  const out = new Float32Array(src.length);
  for (let y = 0; y < height; y += 1) {
    let sum = 0;
    let n = 0;
    for (let x = -radius; x < width; x += 1) {
      const add = x + radius;
      if (add < width) { sum += src[y * width + add]; n += 1; }
      const drop = x - radius - 1;
      if (drop >= 0) { sum -= src[y * width + drop]; n -= 1; }
      if (x >= 0) tmp[y * width + x] = sum / n;
    }
  }
  for (let x = 0; x < width; x += 1) {
    let sum = 0;
    let n = 0;
    for (let y = -radius; y < height; y += 1) {
      const add = y + radius;
      if (add < height) { sum += tmp[add * width + x]; n += 1; }
      const drop = y - radius - 1;
      if (drop >= 0) { sum -= tmp[drop * width + x]; n -= 1; }
      if (y >= 0) out[y * width + x] = sum / n;
    }
  }
  return out;
}

function morph(src, width, height, radius, grow) {
  const tmp = new Float32Array(src.length);
  const out = new Float32Array(src.length);
  for (let y = 0; y < height; y += 1) {
    const row = y * width;
    for (let x = 0; x < width; x += 1) {
      let value = src[row + x];
      const end = Math.min(width - 1, x + radius);
      for (let k = Math.max(0, x - radius); k <= end; k += 1) {
        const next = src[row + k];
        if (grow ? next > value : next < value) value = next;
      }
      tmp[row + x] = value;
    }
  }
  for (let y = 0; y < height; y += 1) {
    const end = Math.min(height - 1, y + radius);
    for (let x = 0; x < width; x += 1) {
      let value = tmp[y * width + x];
      for (let k = Math.max(0, y - radius); k <= end; k += 1) {
        const next = tmp[k * width + x];
        if (grow ? next > value : next < value) value = next;
      }
      out[y * width + x] = value;
    }
  }
  return out;
}

function normalizeMap(map) {
  const values = new Float32Array(Math.ceil(map.length / 5));
  for (let i = 0, j = 0; i < map.length; i += 5, j += 1) values[j] = map[i];
  values.sort();
  const low = values[Math.floor(values.length * 0.05)];
  const high = values[Math.floor(values.length * 0.995)];
  const scale = 1 / (high - low || 1);
  const out = new Float32Array(map.length);
  for (let i = 0; i < map.length; i += 1) {
    const v = (map[i] - low) * scale;
    out[i] = v < 0 ? 0 : v > 1 ? 1 : v;
  }
  return out;
}

export function eyeMap(roi, model = createSkinModel()) {
  const { width, height } = roi;
  const count = width * height;
  const { y, cb, cr } = toYCbCr(roi.data, count);
  const skin = new Float32Array(count);
  for (let i = 0; i < count; i += 1) skin[i] = isSkin(model, y[i], cb[i], cr[i]) ? 1 : 0;
  const skinShare = boxBlur(skin, width, height, 2);
  const smooth = boxBlur(y, width, height, 1);
  const local = boxBlur(smooth, width, height, 7);
  const dark = new Float32Array(count);
  for (let i = 0; i < count; i += 1) dark[i] = Math.max(0, local[i] - smooth[i]) / (local[i] + 24);
  const grown = morph(smooth, width, height, 3, true);
  const shrunk = morph(smooth, width, height, 2, false);
  const contrast = new Float32Array(count);
  for (let i = 0; i < count; i += 1) contrast[i] = (grown[i] - shrunk[i]) / (grown[i] + 24);
  const chroma = new Float32Array(count);
  for (let i = 0; i < count; i += 1) {
    const b = cb[i] / 255;
    const r = cr[i] / 255;
    chroma[i] = (b * b + (1 - r) * (1 - r) + b / Math.max(0.05, r)) / 3;
  }
  const darkSoft = boxBlur(dark, width, height, 1);
  const d = normalizeMap(darkSoft);
  const c = normalizeMap(contrast);
  const h = normalizeMap(chroma);
  const score = new Float32Array(count);
  for (let i = 0; i < count; i += 1) score[i] = d[i] * 0.5 + c[i] * 0.3 + h[i] * 0.2;
  return {
    score: boxBlur(score, width, height, 1),
    y: smooth,
    soft: boxBlur(smooth, width, height, 1),
    dark: darkSoft,
    cb,
    cr,
    skin: skinShare,
  };
}

function localPeaks(score, width, height, box, limit) {
  const peaks = [];
  const [x0, y0, x1, y1] = box;
  for (let y = Math.max(2, Math.floor(y0)); y <= Math.min(height - 3, Math.ceil(y1)); y += 1) {
    for (let x = Math.max(2, Math.floor(x0)); x <= Math.min(width - 3, Math.ceil(x1)); x += 1) {
      const value = score[y * width + x];
      let peak = true;
      for (let dy = -2; dy <= 2 && peak; dy += 1) {
        for (let dx = -2; dx <= 2; dx += 1) {
          if ((dx || dy) && score[(y + dy) * width + x + dx] > value) {
            peak = false;
            break;
          }
        }
      }
      if (peak) peaks.push([x, y, value]);
    }
  }
  return peaks.sort((a, b) => b[2] - a[2]).slice(0, limit);
}

function templateMatch(values, width, height, left, right, templates) {
  const dx = right[0] - left[0];
  const dy = right[1] - left[1];
  const mid = [(left[0] + right[0]) / 2, (left[1] + right[1]) / 2];
  const [a0, a1, b0, b1] = TEMPLATE_BOX;
  const patch = new Float32Array(TEMPLATE_WIDTH * TEMPLATE_HEIGHT);
  let best = -1;
  for (const stretch of TEMPLATE_STRETCH) {
    for (let y = 0; y < TEMPLATE_HEIGHT; y += 1) {
      const b = (b0 + ((y + 0.5) / TEMPLATE_HEIGHT) * (b1 - b0)) * stretch;
      for (let x = 0; x < TEMPLATE_WIDTH; x += 1) {
        const a = a0 + ((x + 0.5) / TEMPLATE_WIDTH) * (a1 - a0);
        patch[y * TEMPLATE_WIDTH + x] = sampleAt(values, width, height, mid[0] + a * dx - b * dy, mid[1] + a * dy + b * dx);
      }
    }
    const flat = flattenPatch(patch, TEMPLATE_WEIGHTS, TEMPLATE_WIDTH, TEMPLATE_HEIGHT);
    if (!flat) continue;
    for (const template of templates) {
      let dot = 0;
      for (let i = 0; i < flat.length; i += 1) dot += flat[i] * template[i];
      if (dot > best) best = dot;
    }
  }
  return best;
}

export function faceLikenessParts(map, width, height, left, right) {
  return [
    templateMatch(map.soft, width, height, left, right, FACE_TEMPLATES),
    templateMatch(map.dark, width, height, left, right, DARK_TEMPLATES),
  ];
}

function mirrorMatch(y, width, height, left, right, radius) {
  let sa = 0;
  let sb = 0;
  let saa = 0;
  let sbb = 0;
  let sab = 0;
  let n = 0;
  for (let dy = -radius; dy <= radius; dy += 1) {
    for (let dx = -radius; dx <= radius; dx += 1) {
      const ax = Math.round(left[0] + dx);
      const ay = Math.round(left[1] + dy);
      const bx = Math.round(right[0] - dx);
      const by = Math.round(right[1] + dy);
      if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= width || bx >= width || ay >= height || by >= height) continue;
      const a = y[ay * width + ax];
      const b = y[by * width + bx];
      sa += a;
      sb += b;
      saa += a * a;
      sbb += b * b;
      sab += a * b;
      n += 1;
    }
  }
  if (n < 9) return 0;
  const cov = sab / n - (sa / n) * (sb / n);
  const va = saa / n - (sa / n) ** 2;
  const vb = sbb / n - (sb / n) ** 2;
  return va > 1 && vb > 1 ? cov / Math.sqrt(va * vb) : 0;
}

export function refineEyeCenter(y, width, height, start, radius) {
  const r = Math.max(3, Math.round(radius));
  const x0 = Math.max(1, Math.round(start[0]) - r);
  const x1 = Math.min(width - 2, Math.round(start[0]) + r);
  const y0 = Math.max(1, Math.round(start[1]) - r);
  const y1 = Math.min(height - 2, Math.round(start[1]) + r);
  const gxs = [];
  const gys = [];
  const pxs = [];
  const pys = [];
  let total = 0;
  let count = 0;
  const mags = [];
  for (let py = y0; py <= y1; py += 1) {
    for (let px = x0; px <= x1; px += 1) {
      const gx = y[py * width + px + 1] - y[py * width + px - 1];
      const gy = y[(py + 1) * width + px] - y[(py - 1) * width + px];
      const m = Math.sqrt(gx * gx + gy * gy);
      mags.push([px, py, gx, gy, m]);
      total += m;
      count += 1;
    }
  }
  if (!count || !total) return start;
  const floor = (total / count) * 0.6;
  for (const [px, py, gx, gy, m] of mags) {
    if (m <= floor) continue;
    pxs.push(px);
    pys.push(py);
    gxs.push(gx / m);
    gys.push(gy / m);
  }
  const n = pxs.length;
  if (!n) return start;
  let best = start;
  let bestScore = -Infinity;
  for (let cy = y0; cy <= y1; cy += 1) {
    for (let cx = x0; cx <= x1; cx += 1) {
      const ox = (cx - start[0]) / r;
      const oy = (cy - start[1]) / r;
      let sum = 0;
      for (let i = 0; i < n; i += 1) {
        const dx = pxs[i] - cx;
        const dy = pys[i] - cy;
        const len2 = dx * dx + dy * dy;
        if (!len2) continue;
        const dot = dx * gxs[i] + dy * gys[i];
        if (dot > 0) sum += (dot * dot) / len2;
      }
      const score = (sum / n) * (255 - y[cy * width + cx]) * (1 - 0.35 * (ox * ox + oy * oy));
      if (score > bestScore) {
        bestScore = score;
        best = [cx, cy];
      }
    }
  }
  return best;
}

export function findEyes(map, width, height, search, limit = PAIR_CHOICES) {
  const below = Math.max(3, Math.round(search.minSpan * 0.3));
  const peaks = localPeaks(map.score, width, height, search.box, CANDIDATES * 6)
    .filter(([x, y]) => sampleAt(map.skin, width, height, x, y + below) >= 0.3)
    .slice(0, CANDIDATES * 3);
  const ranked = [];
  for (let i = 0; i < peaks.length; i += 1) {
    for (let j = 0; j < peaks.length; j += 1) {
      const a = peaks[i];
      const b = peaks[j];
      if (b[0] <= a[0]) continue;
      const dx = b[0] - a[0];
      const dy = b[1] - a[1];
      if (dx < search.minSpan || dx > search.maxSpan || Math.abs(dy) > dx * (search.tilt ?? 0.3)) continue;
      const skinAt = (x, y) => sampleAt(map.skin, width, height, x, y);
      const cheeks = Math.min(skinAt(a[0] - dy * 0.35, a[1] + dx * 0.35), skinAt(b[0] - dy * 0.35, b[1] + dx * 0.35));
      const bridge = skinAt((a[0] + b[0]) / 2 - dy * 0.1, (a[1] + b[1]) / 2 + dx * 0.1);
      if (cheeks < 0.35 || bridge < 0.35) continue;
      const [shade, darkness] = faceLikenessParts(map, width, height, a, b);
      const likeness = Math.max(shade, darkness);
      if (likeness < MIN_LIKENESS) continue;
      const brow = Math.min(skinAt(a[0] + dy * 0.6, a[1] - dx * 0.6), skinAt(b[0] + dy * 0.6, b[1] - dx * 0.6));
      let score = shade + 2.2 * darkness + 0.4 * (a[2] + b[2]) - Math.abs(dy) / dx + 0.3 * (cheeks + bridge) +
        FOREHEAD_WEIGHT * brow;
      score += 0.3 * mirrorMatch(map.y, width, height, a, b, Math.max(3, Math.round(dx * 0.18)));
      if (search.expect) {
        const miss = Math.hypot(a[0] - search.expect[0][0], a[1] - search.expect[0][1]) +
          Math.hypot(b[0] - search.expect[1][0], b[1] - search.expect[1][1]);
        score -= (miss / (search.expectSpan || dx)) * 1.2;
      } else {
        score -= Math.abs((a[0] + b[0]) / 2 - search.axis) / dx * 0.8;
      }
      ranked.push({ score, likeness, left: a, right: b });
    }
  }
  return ranked
    .sort((x, y) => y.score - x.score)
    .slice(0, limit)
    .map((pair) => ({ left: pair.left, right: pair.right, score: pair.score, likeness: pair.likeness }));
}

function sampleAt(values, width, height, x, y) {
  const fx = clampValue(x, 0, width - 1.001);
  const fy = clampValue(y, 0, height - 1.001);
  const ix = Math.floor(fx);
  const iy = Math.floor(fy);
  const tx = fx - ix;
  const ty = fy - iy;
  const i = iy * width + ix;
  return values[i] * (1 - tx) * (1 - ty) + values[i + 1] * tx * (1 - ty) +
    values[i + width] * (1 - tx) * ty + values[i + width + 1] * tx * ty;
}

function patchMean(values, width, height, x, y, radius) {
  let sum = 0;
  let n = 0;
  for (let dy = -radius; dy <= radius; dy += 1) {
    for (let dx = -radius; dx <= radius; dx += 1) {
      sum += sampleAt(values, width, height, x + dx, y + dy);
      n += 1;
    }
  }
  return sum / n;
}

export function findMouth(map, width, height, left, right) {
  const dx = right[0] - left[0];
  const dy = right[1] - left[1];
  const span = Math.hypot(dx, dy) || 1;
  const ux = [dx / span, dy / span];
  const uy = [-ux[1], ux[0]];
  const mid = [(left[0] + right[0]) / 2, (left[1] + right[1]) / 2];
  const at = (a, b) => [mid[0] + a * ux[0] + b * uy[0], mid[1] + a * ux[1] + b * uy[1]];
  const cheeks = [at(-0.42 * span, 0.62 * span), at(0.42 * span, 0.62 * span)];
  const skin = { y: 0, cb: 0, cr: 0 };
  const radius = Math.max(1, Math.round(span * 0.07));
  const samples = [];
  for (const [x, y] of cheeks) {
    skin.y += patchMean(map.y, width, height, x, y, radius) / 2;
    skin.cb += patchMean(map.cb, width, height, x, y, radius) / 2;
    skin.cr += patchMean(map.cr, width, height, x, y, radius) / 2;
    for (let sy = -radius; sy <= radius; sy += 1) {
      for (let sx = -radius; sx <= radius; sx += 1) {
        samples.push([
          sampleAt(map.cb, width, height, x + sx, y + sy),
          sampleAt(map.cr, width, height, x + sx, y + sy),
        ]);
      }
    }
  }
  const half = 0.32 * span;
  const step = Math.max(0.5, span / 90);
  const rows = [];
  for (let t = 0.7 * span; t <= 1.5 * span; t += step) {
    let red = 0;
    let gap = 0;
    let n = 0;
    for (let a = -half; a <= half; a += step * 2) {
      const [x, y] = at(a, t);
      const luma = sampleAt(map.y, width, height, x, y);
      const cr = sampleAt(map.cr, width, height, x, y);
      const cb = sampleAt(map.cb, width, height, x, y);
      red += Math.max(0, (cr - skin.cr) - 0.5 * (cb - skin.cb));
      gap += Math.max(0, skin.y - luma) / Math.max(24, skin.y);
      n += 1;
    }
    rows.push({ t, red: red / n, gap: gap / n });
  }
  if (!rows.length) return null;
  const redPeak = Math.max(...rows.map((row) => row.red)) || 1;
  const gapPeak = Math.max(...rows.map((row) => row.gap)) || 1;
  let line = rows[0];
  let lineScore = -Infinity;
  rows.forEach((row) => {
    const prior = (row.t / span - 1.05) / 0.4;
    const score = row.gap / gapPeak + 0.6 * (row.red / redPeak) - 0.5 * prior * prior;
    if (score > lineScore) {
      lineScore = score;
      line = row;
    }
  });
  const lineIndex = rows.indexOf(line);
  let gapRows = 0;
  const threshold = Math.max(0.22, line.gap * 0.65);
  for (let i = lineIndex; i < rows.length && rows[i].gap >= threshold; i += 1) gapRows += 1;
  for (let i = lineIndex - 1; i >= 0 && rows[i].gap >= threshold; i -= 1) gapRows += 1;
  const opening = (gapRows * step) / span;
  const lineValue = (a) => {
    const [x, y] = at(a, line.t);
    const luma = sampleAt(map.y, width, height, x, y);
    const cr = sampleAt(map.cr, width, height, x, y);
    const cb = sampleAt(map.cb, width, height, x, y);
    return Math.max(0, skin.y - luma) / Math.max(24, skin.y) +
      Math.max(0, (cr - skin.cr) - 0.5 * (cb - skin.cb)) / Math.max(4, redPeak);
  };
  const center = lineValue(0);
  const edge = (sign) => {
    let reach = 0.15 * span;
    for (let a = 0.15 * span; a <= 0.6 * span; a += step) {
      if (lineValue(sign * a) < center * 0.35) break;
      reach = a;
    }
    return reach;
  };
  const leftReach = edge(-1);
  const rightReach = edge(1);
  const mouthCenter = at((rightReach - leftReach) / 2, line.t);
  return {
    center: mouthCenter,
    evidence: Math.min(1, line.red / 10) + 0.4 * Math.min(1, line.gap / 0.3),
    width: leftReach + rightReach,
    open: clampValue((opening - 0.07) / 0.3, 0, 1),
    strength: lineScore,
    skin,
    samples,
  };
}

export function estimateYaw(skin, width, height, left, right) {
  const dx = right[0] - left[0];
  const dy = right[1] - left[1];
  const span = Math.hypot(dx, dy) || 1;
  const ux = [dx / span, dy / span];
  const uy = [-ux[1], ux[0]];
  const mid = [(left[0] + right[0]) / 2, (left[1] + right[1]) / 2];
  const limit = YAW_REACH * span;
  const step = Math.max(0.5, span / 60);
  const sides = [0, 0];
  let rows = 0;
  for (let b = 0.3; b <= 0.9; b += 0.1) {
    const reach = [0, 0];
    let open = false;
    for (const [k, sign] of [[0, -1], [1, 1]]) {
      let gap = 0;
      let d = 0.25 * span;
      for (; d <= limit; d += step) {
        const x = mid[0] + sign * ux[0] * d + uy[0] * b * span;
        const y = mid[1] + sign * ux[1] * d + uy[1] * b * span;
        if (x < 0 || y < 0 || x > width - 1 || y > height - 1) break;
        if (sampleAt(skin, width, height, x, y) >= 0.5) {
          reach[k] = d;
          gap = 0;
        } else if ((gap += step) > 0.08 * span) {
          break;
        }
      }
      if (reach[k] >= limit - step * 2 || d > limit) open = true;
    }
    if (open || !reach[0] || !reach[1]) continue;
    sides[0] += reach[0];
    sides[1] += reach[1];
    rows += 1;
  }
  if (rows < 3) return null;
  const ratio = (sides[0] - sides[1]) / (sides[0] + sides[1]);
  return Math.asin(clampValue(ratio * 1.6, -0.8, 0.8));
}

export function acquisitionPlans(width, height, blob, scale) {
  const plans = [];
  const search = {
    box: [ROI_SIZE * 0.12, ROI_SIZE * 0.12, ROI_SIZE * 0.88, ROI_SIZE * 0.62],
    minSpan: ROI_SIZE * 0.17,
    maxSpan: ROI_SIZE * 0.45,
    axis: ROI_SIZE / 2,
    expect: null,
    tilt: 0.45,
  };
  if (blob) {
    plans.push({
      rect: { cx: blob.cx * scale, cy: (blob.top + blob.width * 0.62) * scale, side: blob.width * 1.55 * scale, angle: 0 },
      search,
    });
  }
  for (const [size, xs, ys] of [
    [0.85, [0.5, 0.36, 0.64], [0.5]],
    [0.55, [0.5, 0.28, 0.72], [0.4, 0.62]],
  ]) {
    for (const y of ys) {
      for (const x of xs) {
        plans.push({ rect: { cx: width * x, cy: height * y, side: height * size, angle: 0 }, search });
      }
    }
  }
  return plans;
}

export function createFaceTracker({ grab, budget = 2 }) {
  let skin = createSkinModel();
  let lock = null;
  let misses = 0;
  let cursor = 0;
  let verifications = 0;
  let frames = 0;
  let rival = null;

  function lockedPlan() {
    const span = lock.span;
    const side = span * LOCK_SPAN;
    const center = [
      lock.mid[0] + lock.uy[0] * span * (0.5 - LOCK_EYE_V) * LOCK_SPAN,
      lock.mid[1] + lock.uy[1] * span * (0.5 - LOCK_EYE_V) * LOCK_SPAN,
    ];
    const roiSpan = ROI_SIZE / LOCK_SPAN;
    const eyeV = ROI_SIZE * LOCK_EYE_V;
    const expect = [[ROI_SIZE / 2 - roiSpan / 2, eyeV], [ROI_SIZE / 2 + roiSpan / 2, eyeV]];
    const reach = roiSpan * SEARCH_RADIUS;
    return {
      rect: { cx: center[0], cy: center[1], side, angle: lock.roll },
      search: {
        box: [expect[0][0] - reach, eyeV - reach, expect[1][0] + reach, eyeV + reach],
        minSpan: roiSpan * 0.7,
        maxSpan: roiSpan * 1.3,
        axis: ROI_SIZE / 2,
        expect,
        expectSpan: roiSpan,
      },
    };
  }

  function verifyPlan() {
    const span = lock.span;
    const side = span * VERIFY_SPAN;
    const roiSpan = ROI_SIZE / VERIFY_SPAN;
    return {
      rect: {
        cx: lock.mid[0],
        cy: lock.mid[1] + span * VERIFY_DROP,
        side,
        angle: 0,
      },
      search: {
        box: [ROI_SIZE * 0.1, ROI_SIZE * 0.06, ROI_SIZE * 0.9, ROI_SIZE * 0.72],
        minSpan: roiSpan * 0.6,
        maxSpan: roiSpan * 1.35,
        axis: ROI_SIZE / 2,
        expect: null,
        tilt: 0.6,
      },
    };
  }

  function analyze(source, plan, needed) {
    const roi = grab(source, plan.rect, ROI_SIZE, ROI_SIZE);
    const map = eyeMap(roi, skin);
    let best = null;
    for (const eyes of findEyes(map, ROI_SIZE, ROI_SIZE, plan.search)) {
      if (eyes.likeness < needed) continue;
      const mouth = findMouth(map, ROI_SIZE, ROI_SIZE, eyes.left, eyes.right);
      if (!mouth) continue;
      const score = eyes.score + MOUTH_WEIGHT * mouth.evidence;
      if (!best || score > best.score) best = { plan, map, eyes, mouth, score };
    }
    if (!best) return null;
    const span = Math.hypot(best.eyes.right[0] - best.eyes.left[0], best.eyes.right[1] - best.eyes.left[1]);
    const left = refineEyeCenter(map.y, ROI_SIZE, ROI_SIZE, best.eyes.left, span * 0.14);
    const right = refineEyeCenter(map.y, ROI_SIZE, ROI_SIZE, best.eyes.right, span * 0.14);
    best.eyes = { ...best.eyes, left, right };
    best.mouth = findMouth(map, ROI_SIZE, ROI_SIZE, left, right) || best.mouth;
    return best;
  }

  function lockFrom(found) {
    const [eyeL, eyeR] = imagePoints(found);
    const dx = eyeR[0] - eyeL[0];
    const dy = eyeR[1] - eyeL[1];
    const span = Math.hypot(dx, dy) || 1;
    return {
      mid: [(eyeL[0] + eyeR[0]) / 2, (eyeL[1] + eyeR[1]) / 2],
      span,
      roll: Math.atan2(dy, dx),
      uy: [-dy / span, dx / span],
    };
  }

  function pairShift(found) {
    const next = lockFrom(found);
    return Math.hypot(next.mid[0] - lock.mid[0], next.mid[1] - lock.mid[1]) / lock.span;
  }

  function imagePoints(found) {
    const { plan, eyes } = found;
    const k = plan.rect.side / ROI_SIZE;
    const cos = Math.cos(plan.rect.angle);
    const sin = Math.sin(plan.rect.angle);
    const back = ([u, v]) => {
      const a = (u - ROI_SIZE / 2) * k;
      const b = (v - ROI_SIZE / 2) * k;
      return [plan.rect.cx + a * cos - b * sin, plan.rect.cy + a * sin + b * cos];
    };
    return [back(eyes.left), back(eyes.right), back(found.mouth.center)];
  }

  function miss() {
    if (lock && !lock.confirmed) {
      lock = null;
      verifications = 0;
      return null;
    }
    misses += 1;
    if (misses > LOCK_MISSES) {
      lock = null;
      skin = createSkinModel();
      misses = 0;
    }
    return null;
  }

  function track(source, width, height) {
    let found = null;
    if (lock && !lock.confirmed) {
      found = analyze(source, verifyPlan(), ACQUIRE_LIKENESS);
      if (!found) return miss();
    } else if (lock) {
      frames += 1;
      if (frames % REVERIFY_EVERY === 0) {
        const check = analyze(source, verifyPlan(), ACQUIRE_LIKENESS);
        if (check && pairShift(check) > REVERIFY_SHIFT) {
          const next = lockFrom(check);
          const agrees = rival && Math.hypot(next.mid[0] - rival.mid[0], next.mid[1] - rival.mid[1]) <
            CONFIRM_SHIFT * next.span;
          if (agrees) {
            rival = null;
            lock = { ...lock, ...next, confirmed: false };
            return null;
          }
          rival = next;
        } else {
          rival = null;
        }
      }
      found = analyze(source, lockedPlan(), KEEP_LIKENESS);
      if (!found) return miss();
    } else {
      const scale = width / PROBE_SIZE;
      const probeHeight = Math.max(1, Math.round(height / scale));
      const probe = grab(source, { cx: width / 2, cy: height / 2, side: width, angle: 0 },
        PROBE_SIZE, probeHeight);
      const blob = faceBlob(skinMask(probe, skin), PROBE_SIZE, probeHeight);
      const plans = acquisitionPlans(width, height, blob, scale);
      const scans = blob ? plans.slice(1) : plans;
      const tries = blob ? [plans[0]] : [];
      const start = cursor;
      for (let i = 0; tries.length < budget && i < scans.length; i += 1) {
        tries.push(scans[(start + i) % scans.length]);
        cursor = (start + i + 1) % scans.length;
      }
      for (const plan of tries) {
        const result = analyze(source, plan, ACQUIRE_LIKENESS);
        if (result && (!found || result.score > found.score)) found = result;
      }
      if (!found) return miss();
    }
    misses = 0;
    const { map, eyes, mouth } = found;
    const [eyeL, eyeR, mouthAt] = imagePoints(found);
    const next = lockFrom(found);
    const confirmed = Boolean(lock) && (lock.confirmed ||
      (pairShift(found) < CONFIRM_SHIFT && Math.abs(next.span / lock.span - 1) < CONFIRM_SHIFT));
    lock = { ...next, confirmed };
    if (!confirmed) {
      verifications += 1;
      if (verifications > VERIFY_ROUNDS) {
        lock = null;
        verifications = 0;
      }
      return null;
    }
    verifications = 0;
    learnSkin(skin, mouth.samples);
    return {
      eyeL,
      eyeR,
      mouth: mouthAt,
      open: mouth.open,
      yaw: estimateYaw(map.skin, ROI_SIZE, ROI_SIZE, eyes.left, eyes.right),
      confidence: clampValue(eyes.likeness, 0, 1),
    };
  }

  return {
    track,
    reset() {
      lock = null;
      misses = 0;
      cursor = 0;
      verifications = 0;
      rival = null;
      skin = createSkinModel();
    },
    get skin() {
      return skin;
    },
    get locked() {
      return Boolean(lock);
    },
  };
}
