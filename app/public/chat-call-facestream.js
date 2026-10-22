import { buildHeadMesh, clampValue, faceFrame, headLayout, headModel, liveHead, MOUTH_EXTRA_COUNT, mouthExtras, mouthGap, pointsFromFlat, solvePose, vertexDepths } from "./chat-call-face-geometry.js";
import { closeLids } from "./chat-call-face-expression.js";
import { applyTransform, buildCustomFace, buildCustomMatte, customFaceTransform, headRing } from "./chat-call-face-custom.js";
import { faceHead, faceTemplate } from "./chat-call-faces.js";
import { FACE_WARP, VERTEX_FLOATS, faceWebGL, lookUniform, photoEyes, warpVertices } from "./chat-call-face-render.js";

export const FACE_STREAM_VERSION = 1;
export const FACE_STREAM_KIND = Object.freeze({ delta: 0, key: 1, reference: 2, atlas: 3 });
export const FACE_STREAM_MOUTH = Object.freeze({ closed: 0, open: 1, wide: 2, round: 3 });
export const FACE_STREAM_FORMAT = Object.freeze({ jpeg: 0, heic: 1, webp: 2 });
export const FACE_STREAM_DEFAULTS = Object.freeze({ fps: 30, precision: 96, keyInterval: 30 });
export const FACE_STREAM_LIMITS = Object.freeze({
  maxFramePacket: 1024,
  maxReferencePacket: 256 * 1024,
  maxSide: 4096,
  maxReferenceSide: 1024,
  maxMaskSide: 128,
  maxAtlasPacket: 64 * 1024,
  maxAtlasTiles: 8,
  minAtlasSide: 8,
  maxAtlasSide: 128,
  minPrecision: 16,
  maxPrecision: 255,
});
export const FACE_STREAM_LABEL = "Face stream";
export const FACE_STREAM_DETAIL = "Animated from a recent photo while the connection is weak";
export const FACE_STREAM_STALE_DETAIL = "Face stream: catching up";
export const FACE_STREAM_REFERENCE = Object.freeze({
  width: 256,
  height: 320,
  quality: 0.72,
  minIntervalMs: 4000,
  refreshMs: 10000,
  better: 0.15,
  maxYaw: 0.26,
  maxPitch: 0.26,
  maxRoll: 0.35,
  maxLid: 0.2,
  maxMouth: 0.12,
  luma: Object.freeze([0.22, 0.85]),
  minContrast: 0.035,
  minSpan: 36,
  mask: Object.freeze([96, 120]),
  cropHeight: 4.6,
  cropDrop: 0.35,
  restRise: 0.006,
  eyeOpen: 0.25,
  eyeFall: 0.0005,
  budgetBurst: 10500,
  budgetRate: 1400,
  expectReference: 9000,
  expectAtlas: 2500,
});

export const FACE_STREAM_ATLAS = Object.freeze({
  tileW: 64,
  tileH: 48,
  quality: 0.82,
  reach: 1.6,
  minIntervalMs: 3000,
  refreshMs: 60000,
  better: 0.15,
  maxYaw: 0.35,
  maxPitch: 0.35,
  maxRoll: 0.4,
  minWidth: 20,
  closed: 0.05,
  open: 0.25,
  mid: 0.1,
  wide: 0.06,
  round: 0.06,
  restRate: 0.05,
});

export const FACE_STREAM_TEMPLATE = Object.freeze([
  [-1.0496, 0.0765, -0.95], [-1.0473, 0.3567, -0.9], [-1.032, 0.6366, -0.8], [-0.9939, 0.914, -0.65],
  [-0.9103, 1.1805, -0.45], [-0.7622, 1.4155, -0.25], [-0.5366, 1.5709, -0.08], [-0.2751, 1.6607, 0.05],
  [0, 1.7019, 0.1], [0.2751, 1.6607, 0.05], [0.5366, 1.5709, -0.08], [0.7622, 1.4155, -0.25],
  [0.9103, 1.1805, -0.45], [0.9939, 0.914, -0.65], [1.032, 0.6366, -0.8], [1.0473, 0.3567, -0.9],
  [1.0496, 0.0765, -0.95], [-0.8602, -0.2609, -0.25], [-0.7013, -0.3102, -0.08], [-0.5424, -0.3296, 0.02],
  [-0.3836, -0.3117, 0.05], [-0.2248, -0.3094, 0.05], [0.2248, -0.3094, 0.05], [0.3836, -0.3117, 0.05],
  [0.5424, -0.3296, 0.02], [0.7013, -0.3102, -0.08], [0.8602, -0.2609, -0.25], [0, -0.0138, 0],
  [0, 0.1764, 0.12], [0, 0.365, 0.25], [0, 0.554, 0.38], [-0.2372, 0.6484, 0.12],
  [-0.1297, 0.6943, 0.2], [0, 0.7127, 0.25], [0.1297, 0.6943, 0.2], [0.2372, 0.6484, 0.12],
  [-0.7439, -0.0089, -0.12], [-0.5813, -0.07, -0.04], [-0.4077, -0.0609, -0.04], [-0.2572, 0.0273, -0.03],
  [-0.4211, 0.0591, -0.04], [-0.5888, 0.0535, -0.04], [0.2572, 0.0273, -0.03], [0.4077, -0.0609, -0.04],
  [0.5813, -0.07, -0.04], [0.7439, -0.0089, -0.12], [0.5888, 0.0535, -0.04], [0.4211, 0.0591, -0.04],
  [-0.4809, 0.9814, 0.02], [-0.3228, 0.9409, 0.12], [-0.1623, 0.9114, 0.17], [0, 0.9221, 0.19],
  [0.1623, 0.9114, 0.17], [0.3228, 0.9409, 0.12], [0.4809, 0.9814, 0.02], [0.3384, 1.0858, 0.12],
  [0.1765, 1.1543, 0.15], [0, 1.1789, 0.16], [-0.1765, 1.1543, 0.15], [-0.3384, 1.0858, 0.12],
  [-0.4495, 0.9876, 0.04], [-0.2256, 1.021, 0.13], [0, 1.0353, 0.14], [0.2256, 1.021, 0.13],
  [0.4495, 0.9876, 0.04], [0.2261, 1.021, 0.13], [0, 1.0353, 0.14], [-0.2261, 1.021, 0.13],
].map((point) => Object.freeze(point)));

const COUNT = 68;
const PROB_ONE = 4096;
const PROB_HALF = 2048;
const TOP = 16777216;
const TWO32 = 4294967296;
const MAX_LEN = 16;
const MAX_INT = 65535;
const MAX_TRIM = 8;
const QUAT_STEPS = 512;
const TRANS_STEPS = 2;
const SCALE_STEPS = 8;
const MAX_TIME_JUMP = 4000;
const KEY_HEADER = 12;
const POINT_STEPS = 16;
const REFERENCE_FIXED = 1 + 4 + 12 + COUNT * 4 + 3 + 2;
const MOUTH_POINTS = 20;
const ATLAS_FIXED = 9;
const ATLAS_TILE = 3 + MOUTH_POINTS * 4;
const ATLAS_STEPS = 16;
const EXPRESSION = Object.freeze([
  ["lidLeft", 16, 0, 16, false, 3600],
  ["lidRight", 16, 0, 16, false, 3600],
  ["gazeX", 64, -32, 32, true, PROB_HALF],
  ["gazeY", 64, -32, 32, true, PROB_HALF],
  ["mouth", 16, 0, 16, false, PROB_HALF],
  ["smile", 16, 0, 16, false, 2800],
  ["tongue", 16, 0, 16, false, 3900],
]);

export class FaceStreamError extends Error {
  constructor(code) {
    super(`face stream: ${code}`);
    this.code = code;
  }
}

function fail(code) {
  throw new FaceStreamError(code);
}

class RangeEncoder {
  constructor(out) {
    this.low = 0;
    this.rng = 0xFFFFFFFF;
    this.cache = 0;
    this.cacheSize = 1;
    this.out = out;
    this.started = false;
  }

  emit(value) {
    if (!this.started) {
      this.started = true;
      return;
    }
    this.out.push(value & 255);
  }

  shiftLow() {
    const carry = Math.floor(this.low / TWO32);
    const low32 = this.low - carry * TWO32;
    if (low32 < 0xFF000000 || carry !== 0) {
      let temp = this.cache;
      for (;;) {
        this.emit(temp + carry);
        temp = 0xFF;
        this.cacheSize -= 1;
        if (this.cacheSize === 0) break;
      }
      this.cache = Math.floor(low32 / TOP);
    }
    this.cacheSize += 1;
    this.low = (low32 % TOP) * 256;
  }

  normalize() {
    while (this.rng < TOP) {
      this.rng *= 256;
      this.shiftLow();
    }
  }

  bit(probs, at, bit) {
    const p = probs[at];
    const bound = (this.rng >>> 12) * p;
    if (bit === 0) {
      this.rng = bound;
      probs[at] = p + ((PROB_ONE - p) >> 4);
    } else {
      this.low += bound;
      this.rng -= bound;
      probs[at] = p - (p >> 4);
    }
    this.normalize();
  }

  direct(value, count) {
    for (let i = count - 1; i >= 0; i -= 1) {
      this.rng = this.rng >>> 1;
      if (Math.floor(value / 2 ** i) % 2 === 1) this.low += this.rng;
      this.normalize();
    }
  }

  finish() {
    for (const k of [32, 24]) {
      const unit = 2 ** k;
      const value = Math.ceil(this.low / unit) * unit;
      if (value < this.low + this.rng) {
        this.low = value;
        break;
      }
    }
    for (let i = 0; i < 5; i += 1) this.shiftLow();
    const full = this.out.length;
    let end = full;
    while (end > 0 && this.out[end - 1] === 0 && full - end < MAX_TRIM) end -= 1;
    this.out.length = end;
    return this.out;
  }
}

class RangeDecoder {
  constructor(data) {
    this.code = 0;
    this.rng = 0xFFFFFFFF;
    this.data = data;
    this.pos = 0;
    for (let i = 0; i < 4; i += 1) this.code = this.code * 256 + this.next();
  }

  next() {
    const value = this.pos < this.data.length ? this.data[this.pos] : 0;
    this.pos += 1;
    return value;
  }

  normalize() {
    while (this.rng < TOP) {
      this.rng *= 256;
      this.code = (this.code % TOP) * 256 + this.next();
    }
  }

  bit(probs, at) {
    const p = probs[at];
    const bound = (this.rng >>> 12) * p;
    let bit = 0;
    if (this.code < bound) {
      this.rng = bound;
      probs[at] = p + ((PROB_ONE - p) >> 4);
    } else {
      this.code -= bound;
      this.rng -= bound;
      probs[at] = p - (p >> 4);
      bit = 1;
    }
    this.normalize();
    return bit;
  }

  direct(count) {
    let value = 0;
    for (let i = 0; i < count; i += 1) {
      this.rng = this.rng >>> 1;
      let bit = 0;
      if (this.code >= this.rng) {
        this.code -= this.rng;
        bit = 1;
      }
      value = value * 2 + bit;
      this.normalize();
    }
    return value;
  }

  overrun() {
    return this.pos > this.data.length + MAX_TRIM;
  }
}

function intModel(zeroPrior) {
  return {
    zero: [zeroPrior, zeroPrior],
    sign: [PROB_HALF],
    length: new Array(MAX_LEN).fill(PROB_HALF),
    mant: new Array(MAX_LEN).fill(PROB_HALF),
  };
}

function clampInt(value, low, high) {
  return value < low ? low : value > high ? high : value;
}

function bitLength(value) {
  let n = 0;
  let v = value;
  while (v > 0) {
    n += 1;
    v = Math.floor(v / 2);
  }
  return n;
}

function writeMagnitude(enc, model, value) {
  const n = bitLength(value);
  for (let i = 1; i < n; i += 1) enc.bit(model.length, i - 1, 1);
  if (n < MAX_LEN) enc.bit(model.length, n - 1, 0);
  if (n >= 2) {
    enc.bit(model.mant, n - 2, Math.floor(value / 2 ** (n - 2)) % 2);
    if (n >= 3) enc.direct(value % 2 ** (n - 2), n - 2);
  }
}

function writeUint(enc, model, ctx, value) {
  const v = clampInt(value, 0, MAX_INT);
  if (v === 0) {
    enc.bit(model.zero, ctx, 0);
    return;
  }
  enc.bit(model.zero, ctx, 1);
  writeMagnitude(enc, model, v);
}

function writeInt(enc, model, ctx, value) {
  let v = clampInt(value, -MAX_INT, MAX_INT);
  if (v === 0) {
    enc.bit(model.zero, ctx, 0);
    return;
  }
  enc.bit(model.zero, ctx, 1);
  if (v < 0) {
    enc.bit(model.sign, 0, 1);
    v = -v;
  } else {
    enc.bit(model.sign, 0, 0);
  }
  writeMagnitude(enc, model, v);
}

function readMagnitude(dec, model) {
  let n = 1;
  while (n < MAX_LEN && dec.bit(model.length, n - 1) === 1) n += 1;
  let value = 1;
  if (n >= 2) {
    value = value * 2 + dec.bit(model.mant, n - 2);
    if (n >= 3) value = value * 2 ** (n - 2) + dec.direct(n - 2);
  }
  return value;
}

function readUint(dec, model, ctx) {
  if (dec.bit(model.zero, ctx) === 0) return 0;
  return readMagnitude(dec, model);
}

function readInt(dec, model, ctx) {
  if (dec.bit(model.zero, ctx) === 0) return 0;
  const negative = dec.bit(model.sign, 0) === 1;
  const a = readMagnitude(dec, model);
  return negative ? -a : a;
}

function floorHalf(value) {
  const f = Math.floor(value + 0.5);
  if (f > 1e9) return 1e9;
  if (f < -1e9) return -1e9;
  return f;
}

function finite(value) {
  return typeof value === "number" && Number.isFinite(value);
}

function landmarkGroup(i) {
  if (i <= 16) return 0;
  if (i <= 26) return 1;
  if (i <= 35) return 2;
  if (i <= 47) return 3;
  if (i <= 59) return 4;
  return 5;
}

function freshModels(count, prior = PROB_HALF) {
  return Array.from({ length: count }, () => intModel(prior));
}

function quantizePose(pose) {
  const [qw, qx, qy, qz] = pose.rotation;
  const n = Math.sqrt(qw * qw + qx * qx + qy * qy + qz * qz);
  let x = 0;
  let y = 0;
  let z = 0;
  if (n > 0) {
    x = qx / n;
    y = qy / n;
    z = qz / n;
    if (qw < 0) {
      x = -x;
      y = -y;
      z = -z;
    }
  }
  return {
    qx: clampInt(floorHalf(x * QUAT_STEPS), -QUAT_STEPS, QUAT_STEPS),
    qy: clampInt(floorHalf(y * QUAT_STEPS), -QUAT_STEPS, QUAT_STEPS),
    qz: clampInt(floorHalf(z * QUAT_STEPS), -QUAT_STEPS, QUAT_STEPS),
    tx: clampInt(floorHalf(pose.x * TRANS_STEPS), -32768, 32767),
    ty: clampInt(floorHalf(pose.y * TRANS_STEPS), -32768, 32767),
    s: clampInt(floorHalf(pose.scale * SCALE_STEPS), 1, 32767),
  };
}

function poseOf(q) {
  const x = q.qx / QUAT_STEPS;
  const y = q.qy / QUAT_STEPS;
  const z = q.qz / QUAT_STEPS;
  const ss = x * x + y * y + z * z;
  const w = ss < 1 ? Math.sqrt(1 - ss) : 0;
  const n = Math.sqrt(w * w + x * x + y * y + z * z);
  return { rotation: [w / n, x / n, y / n, z / n], x: q.tx / TRANS_STEPS, y: q.ty / TRANS_STEPS, scale: q.s / SCALE_STEPS };
}

export function rotationRows([w, x, y, z]) {
  return [
    [w * w + x * x - y * y - z * z, 2 * (x * y - w * z), 2 * (x * z + w * y)],
    [2 * (x * y + w * z), w * w - x * x + y * y - z * z, 2 * (y * z - w * x)],
    [2 * (x * z - w * y), 2 * (y * z + w * x), w * w - x * x - y * y + z * z],
  ];
}

function dot3(r, v) {
  return r[0] * v[0] + r[1] * v[1] + r[2] * v[2];
}

export function projectFaceStreamPose(pose) {
  const r = rotationRows(pose.rotation);
  return FACE_STREAM_TEMPLATE.map((m) => [pose.x + pose.scale * dot3(r[0], m), pose.y + pose.scale * dot3(r[1], m)]);
}

function quantizeExpression(expression = {}) {
  return EXPRESSION.map(([name, steps, low, high]) => {
    const value = finite(expression[name]) ? expression[name] : 0;
    return clampInt(floorHalf(value * steps), low, high);
  });
}

function expressionOf(q) {
  const out = {};
  EXPRESSION.forEach(([name, steps], i) => {
    out[name] = q[i] / steps;
  });
  return out;
}

function nominalInterval(fps) {
  return Math.floor((1000 + Math.floor(fps / 2)) / fps);
}

function normalizedConfig(config = {}, strict = false) {
  const c = {
    width: config.width | 0,
    height: config.height | 0,
    fps: config.fps ?? (strict ? 0 : FACE_STREAM_DEFAULTS.fps),
    precision: config.precision ?? (strict ? 0 : FACE_STREAM_DEFAULTS.precision),
    keyInterval: config.keyInterval ?? FACE_STREAM_DEFAULTS.keyInterval,
  };
  if (!strict) {
    if (!c.fps) c.fps = FACE_STREAM_DEFAULTS.fps;
    if (!c.precision) c.precision = FACE_STREAM_DEFAULTS.precision;
    if (!c.keyInterval) c.keyInterval = FACE_STREAM_DEFAULTS.keyInterval;
  }
  const ok = c.width >= 1 && c.width <= FACE_STREAM_LIMITS.maxSide && c.height >= 1 && c.height <= FACE_STREAM_LIMITS.maxSide &&
    c.fps >= 1 && c.fps <= 120 && c.precision >= FACE_STREAM_LIMITS.minPrecision && c.precision <= FACE_STREAM_LIMITS.maxPrecision &&
    c.keyInterval >= 1 && c.keyInterval <= 255;
  return ok ? c : null;
}

function validInput(input) {
  if (!input || !Array.isArray(input.points) || input.points.length < COUNT) return false;
  for (let i = 0; i < COUNT; i += 1) {
    const p = input.points[i];
    if (!p || !finite(p[0]) || !finite(p[1])) return false;
  }
  const pose = input.pose;
  if (!pose || !Array.isArray(pose.rotation) || pose.rotation.length < 4 || !pose.rotation.every(finite)) return false;
  if (!finite(pose.x) || !finite(pose.y) || !finite(pose.scale)) return false;
  const time = input.time;
  if (!Number.isInteger(time) || time < 0 || time >= TWO32) return false;
  const expression = input.expression || {};
  return EXPRESSION.every(([name]) => expression[name] === undefined || finite(expression[name]));
}

function headerByte(kind, id) {
  return (kind << 6) | (FACE_STREAM_VERSION << 4) | (id & 15);
}

function pushU16(out, value) {
  out.push((value >>> 8) & 255, value & 255);
}

function pushU32(out, value) {
  out.push((value >>> 24) & 255, (value >>> 16) & 255, (value >>> 8) & 255, value & 255);
}

function readU16(data, at) {
  return data[at] * 256 + data[at + 1];
}

function readU32(data, at) {
  return ((data[at] * 16777216) + (data[at + 1] << 16) + (data[at + 2] << 8) + data[at + 3]) >>> 0;
}

function emptyPoints() {
  return Array.from({ length: COUNT }, () => [0, 0]);
}

export function createFaceStreamEncoder(config) {
  const cfg = normalizedConfig(config);
  if (!cfg) fail("config");
  const s = {
    keyId: 15,
    keyPose: null,
    seq: 255,
    time: 0,
    chain: null,
    recon: emptyPoints(),
    projected: emptyPoints(),
  };
  let started = false;
  let sinceKey = 0;
  let forceKey = false;
  let last = null;

  function encode(input) {
    if (!validInput(input)) fail("input");
    const nominal = nominalInterval(cfg.fps);
    const time = input.time >>> 0;
    const dt = ((time - s.time) >>> 0) | 0;
    const key = !started || forceKey || sinceKey >= cfg.keyInterval || dt - nominal > MAX_TIME_JUMP || dt - nominal < -MAX_TIME_JUMP;
    const qp = quantizePose(input.pose);
    const pose = poseOf(qp);
    const projected = projectFaceStreamPose(pose);
    const seq = (s.seq + 1) & 255;
    const keyId = key ? (s.keyId + 1) & 15 : s.keyId;
    const out = [];
    if (key) {
      out.push(headerByte(FACE_STREAM_KIND.key, keyId), seq);
      pushU16(out, cfg.width);
      pushU16(out, cfg.height);
      out.push(cfg.fps, cfg.precision);
      pushU32(out, time);
    } else {
      out.push(headerByte(FACE_STREAM_KIND.delta, keyId), seq);
    }
    const enc = new RangeEncoder(out);
    const values = [qp.qx, qp.qy, qp.qz, qp.tx, qp.ty, qp.s];
    if (key) {
      enc.direct(qp.qx + QUAT_STEPS, 11);
      enc.direct(qp.qy + QUAT_STEPS, 11);
      enc.direct(qp.qz + QUAT_STEPS, 11);
      enc.direct(qp.tx + 32768, 16);
      enc.direct(qp.ty + 32768, 16);
      enc.direct(qp.s, 15);
    } else {
      writeInt(enc, intModel(PROB_HALF), 0, dt - nominal);
      const base = [s.keyPose.qx, s.keyPose.qy, s.keyPose.qz, s.keyPose.tx, s.keyPose.ty, s.keyPose.s];
      const models = freshModels(6);
      values.forEach((v, i) => writeInt(enc, models[i], 0, v - base[i]));
    }
    const qx = quantizeExpression(input.expression);
    EXPRESSION.forEach(([, , , , signed, prior], i) => {
      const model = intModel(prior);
      if (signed) writeInt(enc, model, 0, qx[i]);
      else writeUint(enc, model, 0, qx[i]);
    });
    let models;
    if (key) {
      s.chain = freshModels(12);
      models = freshModels(12);
    } else {
      models = s.chain;
    }
    const step = pose.scale / cfg.precision;
    const recon = emptyPoints();
    const nonzero = new Array(12).fill(0);
    for (let i = 0; i < COUNT; i += 1) {
      for (let axis = 0; axis < 2; axis += 1) {
        const pred = key ? projected[i][axis] : s.recon[i][axis] + (projected[i][axis] - s.projected[i][axis]);
        const r = clampInt(floorHalf((input.points[i][axis] - pred) / step), -MAX_INT, MAX_INT);
        const cls = landmarkGroup(i) * 2 + axis;
        writeInt(enc, models[cls], nonzero[cls], r);
        nonzero[cls] = r !== 0 ? 1 : 0;
        recon[i][axis] = pred + r * step;
      }
    }
    const packet = Uint8Array.from(enc.finish());
    if (key) {
      s.keyId = keyId;
      s.keyPose = qp;
      sinceKey = 0;
      forceKey = false;
    }
    started = true;
    sinceKey += 1;
    s.seq = seq;
    s.time = time;
    s.recon = recon;
    s.projected = projected;
    last = {
      key, stale: false, seq, keyId, time, width: cfg.width, height: cfg.height, precision: cfg.precision,
      pose, expression: expressionOf(qx), points: recon,
    };
    return packet;
  }

  return {
    encode,
    requestKey() {
      forceKey = true;
    },
    get last() {
      return last;
    },
    get config() {
      return { ...cfg };
    },
  };
}

export function faceStreamPacketKind(packet) {
  return packet && packet.length ? packet[0] >> 6 : -1;
}

export function createFaceStreamDecoder() {
  let s = null;
  let haveKey = false;
  let chainOK = false;
  let lastGood = emptyPoints();
  let goodProj = emptyPoints();

  function decode(packet) {
    if (!packet || packet.length < 2) fail("short");
    if (packet.length > FACE_STREAM_LIMITS.maxFramePacket) fail("long");
    if (((packet[0] >> 4) & 3) !== FACE_STREAM_VERSION) fail("version");
    const kind = packet[0] >> 6;
    if (kind !== FACE_STREAM_KIND.key && kind !== FACE_STREAM_KIND.delta) fail(kind === FACE_STREAM_KIND.reference || kind === FACE_STREAM_KIND.atlas ? "not-a-frame" : "kind");
    const keyId = packet[0] & 15;
    const seq = packet[1];
    const key = kind === FACE_STREAM_KIND.key;
    let cfg;
    let time;
    let body;
    if (key) {
      if (packet.length < KEY_HEADER) fail("short");
      cfg = normalizedConfig({ width: readU16(packet, 2), height: readU16(packet, 4), fps: packet[6], precision: packet[7], keyInterval: 1 }, true);
      if (!cfg) fail("header");
      time = readU32(packet, 8);
      body = packet.subarray(KEY_HEADER);
    } else {
      if (!haveKey || keyId !== s.keyId) fail("need-key");
      const ahead = (seq - s.seq) & 255;
      if (ahead === 0 || ahead > 128) fail("old");
      cfg = s.cfg;
      body = packet.subarray(2);
    }
    const dec = new RangeDecoder(body);
    const inSeq = key || (chainOK && seq === ((s.seq + 1) & 255));
    let qp;
    if (key) {
      qp = {
        qx: dec.direct(11) - QUAT_STEPS,
        qy: dec.direct(11) - QUAT_STEPS,
        qz: dec.direct(11) - QUAT_STEPS,
        tx: dec.direct(16) - 32768,
        ty: dec.direct(16) - 32768,
        s: dec.direct(15),
      };
      if (qp.qx > QUAT_STEPS || qp.qy > QUAT_STEPS || qp.qz > QUAT_STEPS || qp.s < 1) fail("corrupt");
    } else {
      const nominal = nominalInterval(cfg.fps);
      const dt = readInt(dec, intModel(PROB_HALF), 0) + nominal;
      const gap = (seq - s.seq) & 255;
      time = (s.time + (((gap - 1) * nominal + dt) | 0)) >>> 0;
      const models = freshModels(6);
      const base = [s.keyPose.qx, s.keyPose.qy, s.keyPose.qz, s.keyPose.tx, s.keyPose.ty, s.keyPose.s];
      const v = base.map((b, i) => b + readInt(dec, models[i], 0));
      qp = { qx: v[0], qy: v[1], qz: v[2], tx: v[3], ty: v[4], s: v[5] };
      const inside = (value, low, high) => value >= low && value <= high;
      if (!inside(qp.qx, -QUAT_STEPS, QUAT_STEPS) || !inside(qp.qy, -QUAT_STEPS, QUAT_STEPS) || !inside(qp.qz, -QUAT_STEPS, QUAT_STEPS) ||
        !inside(qp.tx, -32768, 32767) || !inside(qp.ty, -32768, 32767) || !inside(qp.s, 1, 32767)) fail("corrupt");
    }
    const qx = EXPRESSION.map(([, , low, high, signed, prior]) => {
      const model = intModel(prior);
      const value = signed ? readInt(dec, model, 0) : readUint(dec, model, 0);
      if (value < low || value > high) fail("corrupt");
      return value;
    });
    const pose = poseOf(qp);
    const projected = projectFaceStreamPose(pose);
    const frame = {
      key, stale: false, seq, keyId, time, width: cfg.width, height: cfg.height, precision: cfg.precision,
      pose, expression: expressionOf(qx), points: null,
    };
    if (inSeq) {
      const models = key ? freshModels(12) : s.chain.map((m) => ({ zero: m.zero.slice(), sign: m.sign.slice(), length: m.length.slice(), mant: m.mant.slice() }));
      const step = pose.scale / cfg.precision;
      const nonzero = new Array(12).fill(0);
      const recon = emptyPoints();
      for (let i = 0; i < COUNT; i += 1) {
        for (let axis = 0; axis < 2; axis += 1) {
          const pred = key ? projected[i][axis] : s.recon[i][axis] + (projected[i][axis] - s.projected[i][axis]);
          const cls = landmarkGroup(i) * 2 + axis;
          const r = readInt(dec, models[cls], nonzero[cls]);
          nonzero[cls] = r !== 0 ? 1 : 0;
          recon[i][axis] = pred + r * step;
        }
      }
      if (dec.overrun()) fail("corrupt");
      if (key) s = { cfg, keyId, keyPose: qp, seq, time, chain: freshModels(12), recon, projected };
      else Object.assign(s, { chain: models, recon, projected });
      lastGood = recon;
      goodProj = projected;
      chainOK = true;
      frame.points = recon;
    } else {
      if (dec.overrun()) fail("corrupt");
      chainOK = false;
      frame.stale = true;
      frame.points = lastGood.map((p, i) => [p[0] + (projected[i][0] - goodProj[i][0]), p[1] + (projected[i][1] - goodProj[i][1])]);
    }
    if (key) haveKey = true;
    s.seq = seq;
    s.time = time;
    return frame;
  }

  return {
    decode,
    reset() {
      s = null;
      haveKey = false;
      chainOK = false;
    },
    get ready() {
      return haveKey;
    },
  };
}

function maskContext(bits, w, x, y) {
  const at = (px, py) => (px < 0 || py < 0 || px >= w ? 0 : bits[py * w + px]);
  return at(x - 1, y) | (at(x, y - 1) << 1) | (at(x - 1, y - 1) << 2) | (at(x + 1, y - 1) << 3);
}

function encodeMask(mask, w, h) {
  const models = new Array(16).fill(PROB_HALF);
  const bits = new Uint8Array(w * h);
  for (let i = 0; i < w * h; i += 1) bits[i] = mask[i] ? 1 : 0;
  const enc = new RangeEncoder([]);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) enc.bit(models, maskContext(bits, w, x, y), bits[y * w + x]);
  }
  return enc.finish();
}

function decodeMask(data, w, h) {
  const models = new Array(16).fill(PROB_HALF);
  const out = new Uint8Array(w * h);
  const dec = new RangeDecoder(data);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) out[y * w + x] = dec.bit(models, maskContext(out, w, x, y));
  }
  return out;
}

function validReference(r) {
  const int = Number.isInteger;
  if (![r.id, r.format, r.width, r.height, r.cropX, r.cropY, r.cropW, r.cropH, r.maskW, r.maskH].every(int)) return false;
  if (r.id < 0 || r.id > 15 || r.format < 0 || r.format > 2) return false;
  if (r.width < 1 || r.width > FACE_STREAM_LIMITS.maxReferenceSide || r.height < 1 || r.height > FACE_STREAM_LIMITS.maxReferenceSide) return false;
  if (r.cropW < 1 || r.cropH < 1 || r.cropX < 0 || r.cropY < 0 || r.cropX + r.cropW > 65535 || r.cropY + r.cropH > 65535) return false;
  if ((r.maskW === 0) !== (r.maskH === 0) || r.maskW < 0 || r.maskW > FACE_STREAM_LIMITS.maxMaskSide || r.maskH < 0 || r.maskH > FACE_STREAM_LIMITS.maxMaskSide) return false;
  return Boolean(r.image && r.image.length > 0);
}

export function encodeFaceStreamReference(reference) {
  const r = { maskW: 0, maskH: 0, mask: null, format: 0, ...reference };
  if (!validReference(r) || (r.maskW > 0 && !(r.mask && r.mask.length >= r.maskW * r.maskH))) fail("input");
  if (!Array.isArray(r.points) || r.points.length < COUNT) fail("input");
  const out = [headerByte(FACE_STREAM_KIND.reference, r.id)];
  pushU32(out, (r.time >>> 0));
  for (const v of [r.cropX, r.cropY, r.cropW, r.cropH, r.width, r.height]) pushU16(out, v);
  for (let i = 0; i < COUNT; i += 1) {
    for (let axis = 0; axis < 2; axis += 1) {
      const v = r.points[i][axis];
      if (!finite(v)) fail("input");
      pushU16(out, clampInt(floorHalf(v * POINT_STEPS), -32768, 32767) & 0xFFFF);
    }
  }
  out.push(r.format, r.maskW, r.maskH);
  const mask = r.maskW > 0 ? encodeMask(r.mask, r.maskW, r.maskH) : [];
  pushU16(out, mask.length);
  const packet = new Uint8Array(out.length + mask.length + r.image.length);
  packet.set(out, 0);
  packet.set(mask, out.length);
  packet.set(r.image, out.length + mask.length);
  if (packet.length > FACE_STREAM_LIMITS.maxReferencePacket) fail("long");
  return packet;
}

export function decodeFaceStreamReference(packet) {
  if (!packet || packet.length < REFERENCE_FIXED + 1) fail("short");
  if (packet.length > FACE_STREAM_LIMITS.maxReferencePacket) fail("long");
  if (((packet[0] >> 4) & 3) !== FACE_STREAM_VERSION) fail("version");
  if (packet[0] >> 6 !== FACE_STREAM_KIND.reference) fail("not-a-reference");
  let at = 5;
  const u16 = () => {
    const v = readU16(packet, at);
    at += 2;
    return v;
  };
  const r = { id: packet[0] & 15, time: readU32(packet, 1) };
  r.cropX = u16();
  r.cropY = u16();
  r.cropW = u16();
  r.cropH = u16();
  r.width = u16();
  r.height = u16();
  r.points = [];
  for (let i = 0; i < COUNT; i += 1) {
    const x = u16();
    const y = u16();
    r.points.push([(x >= 32768 ? x - 65536 : x) / POINT_STEPS, (y >= 32768 ? y - 65536 : y) / POINT_STEPS]);
  }
  r.format = packet[at];
  r.maskW = packet[at + 1];
  r.maskH = packet[at + 2];
  at += 3;
  const maskLength = u16();
  if (at + maskLength >= packet.length) fail("corrupt");
  r.mask = null;
  if (r.maskW > 0 && r.maskH > 0) r.mask = decodeMask(packet.subarray(at, at + maskLength), r.maskW, r.maskH);
  else if (maskLength !== 0) fail("corrupt");
  at += maskLength;
  r.image = packet.subarray(at);
  if (!validReference(r)) fail("corrupt");
  return r;
}

function validAtlas(a) {
  const int = Number.isInteger;
  if (![a.id, a.format, a.tileW, a.tileH].every(int)) return false;
  if (a.id < 0 || a.id > 15 || a.format < 0 || a.format > 2) return false;
  const { minAtlasSide: low, maxAtlasSide: high, maxAtlasTiles } = FACE_STREAM_LIMITS;
  if (a.tileW < low || a.tileW > high || a.tileH < low || a.tileH > high) return false;
  if (!Array.isArray(a.tiles) || a.tiles.length < 1 || a.tiles.length > maxAtlasTiles) return false;
  if (!a.tiles.every((t) => int(t.state) && t.state >= 0 && t.state <= 3)) return false;
  return Boolean(a.image && a.image.length > 0);
}

export function encodeFaceStreamAtlas(atlas) {
  const a = { format: 0, ...atlas };
  if (!validAtlas(a)) fail("input");
  const out = [headerByte(FACE_STREAM_KIND.atlas, a.id)];
  pushU32(out, a.time >>> 0);
  out.push(a.tileW, a.tileH, a.tiles.length, a.format);
  for (const t of a.tiles) {
    if (!finite(t.mouth) || !finite(t.smile) || !Array.isArray(t.points) || t.points.length < MOUTH_POINTS) fail("input");
    out.push(t.state, clampInt(floorHalf(t.mouth * ATLAS_STEPS), 0, ATLAS_STEPS), clampInt(floorHalf(t.smile * ATLAS_STEPS), 0, ATLAS_STEPS));
    for (let i = 0; i < MOUTH_POINTS; i += 1) {
      for (let axis = 0; axis < 2; axis += 1) {
        const v = t.points[i][axis];
        if (!finite(v)) fail("input");
        pushU16(out, clampInt(floorHalf(v * ATLAS_STEPS), -32768, 32767) & 0xFFFF);
      }
    }
  }
  const packet = new Uint8Array(out.length + a.image.length);
  packet.set(out, 0);
  packet.set(a.image, out.length);
  if (packet.length > FACE_STREAM_LIMITS.maxAtlasPacket) fail("long");
  return packet;
}

export function decodeFaceStreamAtlas(packet) {
  if (!packet || packet.length < ATLAS_FIXED + ATLAS_TILE + 1) fail("short");
  if (packet.length > FACE_STREAM_LIMITS.maxAtlasPacket) fail("long");
  if (((packet[0] >> 4) & 3) !== FACE_STREAM_VERSION) fail("version");
  if (packet[0] >> 6 !== FACE_STREAM_KIND.atlas) fail("not-an-atlas");
  const a = { id: packet[0] & 15, time: readU32(packet, 1), tileW: packet[5], tileH: packet[6], format: packet[8], tiles: [] };
  const n = packet[7];
  if (n < 1 || n > FACE_STREAM_LIMITS.maxAtlasTiles || packet.length <= ATLAS_FIXED + n * ATLAS_TILE) fail("corrupt");
  let at = ATLAS_FIXED;
  for (let k = 0; k < n; k += 1) {
    const state = packet[at];
    const mouth = packet[at + 1];
    const smile = packet[at + 2];
    if (mouth > ATLAS_STEPS || smile > ATLAS_STEPS) fail("corrupt");
    at += 3;
    const points = [];
    for (let i = 0; i < MOUTH_POINTS; i += 1) {
      const x = readU16(packet, at);
      const y = readU16(packet, at + 2);
      at += 4;
      points.push([(x >= 32768 ? x - 65536 : x) / ATLAS_STEPS, (y >= 32768 ? y - 65536 : y) / ATLAS_STEPS]);
    }
    a.tiles.push({ state, mouth: mouth / ATLAS_STEPS, smile: smile / ATLAS_STEPS, points });
  }
  a.image = packet.subarray(at);
  if (!validAtlas(a)) fail("corrupt");
  return a;
}

export function quatFromRows(r) {
  const trace = r[0][0] + r[1][1] + r[2][2];
  let q;
  if (trace > 0) {
    const s = Math.sqrt(trace + 1) * 2;
    q = [s / 4, (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s];
  } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
    const s = Math.sqrt(1 + r[0][0] - r[1][1] - r[2][2]) * 2;
    q = [(r[2][1] - r[1][2]) / s, s / 4, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s];
  } else if (r[1][1] > r[2][2]) {
    const s = Math.sqrt(1 + r[1][1] - r[0][0] - r[2][2]) * 2;
    q = [(r[0][2] - r[2][0]) / s, (r[0][1] + r[1][0]) / s, s / 4, (r[1][2] + r[2][1]) / s];
  } else {
    const s = Math.sqrt(1 + r[2][2] - r[0][0] - r[1][1]) * 2;
    q = [(r[1][0] - r[0][1]) / s, (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, s / 4];
  }
  const n = Math.hypot(...q) || 1;
  const sign = q[0] < 0 ? -1 : 1;
  return q.map((v) => (sign * v) / n);
}

export function faceStreamPose(points) {
  const pose = solvePose(FACE_STREAM_TEMPLATE, points);
  return {
    rotation: quatFromRows(pose.rows),
    x: pose.t[0],
    y: pose.t[1],
    scale: pose.scale,
    yaw: pose.yaw,
    pitch: pose.pitch,
    roll: pose.roll,
  };
}

export function faceStreamSmile(points) {
  const frame = faceFrame(points);
  const width = Math.hypot(points[54][0] - points[48][0], points[54][1] - points[48][1]) / frame.span;
  return clampValue((width - 0.96) / 0.3, 0, 1);
}

export function faceStreamExpression(reading, points) {
  const close = reading?.close || [0, 0];
  const look = reading?.look || [[0, 0], [0, 0]];
  const pick = (side, k) => (Number.isFinite(look[side]?.[k]) ? look[side][k] : 0);
  return {
    lidLeft: clampValue(close[0] || 0, 0, 1),
    lidRight: clampValue(close[1] || 0, 0, 1),
    gazeX: (pick(0, 0) + pick(1, 0)) / 2,
    gazeY: (pick(0, 1) + pick(1, 1)) / 2,
    mouth: clampValue(mouthGap(points) / 0.5, 0, 1),
    smile: faceStreamSmile(points),
    tongue: clampValue(reading?.tongue || 0, 0, 1),
  };
}

export function faceStreamEyeOpening(points) {
  const d = (a, b) => Math.hypot(points[a][0] - points[b][0], points[a][1] - points[b][1]);
  const eye = (o) => (d(o + 1, o + 5) + d(o + 2, o + 4)) / (2 * Math.max(d(o, o + 3), 1e-6));
  return (eye(36) + eye(42)) / 2;
}

export function faceLight(pixels, width, height, points) {
  const xs = [36, 39, 42, 45, 48, 54].map((i) => points[i][0]);
  const ys = [19, 24, 33, 57].map((i) => points[i][1]);
  const x0 = clampValue(Math.floor(Math.min(...xs)), 0, width - 1);
  const x1 = clampValue(Math.ceil(Math.max(...xs)), 0, width - 1);
  const y0 = clampValue(Math.floor(Math.min(...ys)), 0, height - 1);
  const y1 = clampValue(Math.ceil(Math.max(...ys)), 0, height - 1);
  const stepX = Math.max(1, Math.floor((x1 - x0) / 24));
  const stepY = Math.max(1, Math.floor((y1 - y0) / 24));
  let sum = 0;
  let squares = 0;
  let n = 0;
  for (let y = y0; y <= y1; y += stepY) {
    for (let x = x0; x <= x1; x += stepX) {
      const at = (y * width + x) * 4;
      const luma = (0.299 * pixels[at] + 0.587 * pixels[at + 1] + 0.114 * pixels[at + 2]) / 255;
      sum += luma;
      squares += luma * luma;
      n += 1;
    }
  }
  if (!n) return { mean: 0, contrast: 0 };
  const mean = sum / n;
  return { mean, contrast: Math.sqrt(Math.max(0, squares / n - mean * mean)) };
}

export function referenceScore({ pose, expression, light = null, span }, policy = FACE_STREAM_REFERENCE) {
  if (!pose || !expression || !(span >= policy.minSpan)) return 0;
  const turn = Math.max(Math.abs(pose.yaw || 0) / policy.maxYaw, Math.abs(pose.pitch || 0) / policy.maxPitch, Math.abs(pose.roll || 0) / policy.maxRoll);
  const lids = Math.max(expression.lidLeft || 0, expression.lidRight || 0) / policy.maxLid;
  const mouth = (expression.mouth || 0) / policy.maxMouth;
  if (turn >= 1 || lids >= 1 || mouth >= 1 || (expression.tongue || 0) > 0.1) return 0;
  let lit = 1;
  if (light) {
    if (light.mean < policy.luma[0] || light.mean > policy.luma[1] || light.contrast < policy.minContrast) return 0;
    lit = 1 - Math.abs(light.mean - 0.5) / 0.5;
  }
  return clampValue(0.45 * (1 - turn) + 0.2 * (1 - lids) + 0.2 * (1 - mouth) + 0.15 * lit, 0, 1);
}

export function referenceDue(last, time, score, policy = FACE_STREAM_REFERENCE) {
  if (!(score > 0)) return false;
  if (!last) return true;
  const age = time - last.time;
  if (age < policy.minIntervalMs) return false;
  if (age >= policy.refreshMs) return true;
  return score > last.score + policy.better;
}

export function referenceCrop(points, frameWidth, frameHeight, policy = FACE_STREAM_REFERENCE) {
  const frame = faceFrame(points);
  const aspect = policy.width / policy.height;
  let h = policy.cropHeight * frame.span;
  let w = h * aspect;
  const fit = Math.min(1, frameWidth / w, frameHeight / h);
  w = Math.max(1, Math.floor(w * fit));
  h = Math.max(1, Math.floor(h * fit));
  const cx = frame.mid[0] + frame.uy[0] * policy.cropDrop * frame.span;
  const cy = frame.mid[1] + frame.uy[1] * policy.cropDrop * frame.span;
  const x = clampValue(Math.round(cx - w / 2), 0, Math.max(0, frameWidth - w));
  const y = clampValue(Math.round(cy - h / 2), 0, Math.max(0, frameHeight - h));
  return { x, y, w, h };
}

export function referencePoints(points, crop, width, height) {
  return points.slice(0, COUNT).map(([x, y]) => [(x - crop.x) * (width / crop.w), (y - crop.y) * (height / crop.h)]);
}

export function referenceMaskBits(mask, crop, maskW, maskH) {
  if (!mask?.bytes || !(mask.width > 0) || !(mask.height > 0)) return null;
  const sx = mask.width / (mask.frameWidth || mask.width);
  const sy = mask.height / (mask.frameHeight || mask.height);
  const out = new Uint8Array(maskW * maskH);
  for (let y = 0; y < maskH; y += 1) {
    for (let x = 0; x < maskW; x += 1) {
      const fx = crop.x + ((x + 0.5) / maskW) * crop.w;
      const fy = crop.y + ((y + 0.5) / maskH) * crop.h;
      const mx = clampValue(Math.floor(fx * sx), 0, mask.width - 1);
      const my = clampValue(Math.floor(fy * sy), 0, mask.height - 1);
      out[y * maskW + x] = mask.bytes[my * mask.width + mx] >= 128 ? 1 : 0;
    }
  }
  return out;
}

function canvasBytes(canvas, type, quality) {
  if (typeof canvas.convertToBlob === "function") return canvas.convertToBlob({ type, quality }).then((blob) => blob.arrayBuffer());
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => (blob ? resolve(blob.arrayBuffer()) : reject(new Error("encode failed"))), type, quality);
  });
}

export function mouthTileCrop(points, policy = FACE_STREAM_ATLAS) {
  let cx = 0;
  let cy = 0;
  for (let i = 48; i < 60; i += 1) {
    cx += points[i][0] / 12;
    cy += points[i][1] / 12;
  }
  const dx = points[54][0] - points[48][0];
  const dy = points[54][1] - points[48][1];
  const width = Math.hypot(dx, dy);
  const ux = width > 0 ? [dx / width, dy / width] : [1, 0];
  return { center: [cx, cy], ux, uy: [-ux[1], ux[0]], width, scale: Math.max(1e-6, (policy.reach * width) / policy.tileW) };
}

export function mouthTilePoints(points, crop, policy = FACE_STREAM_ATLAS) {
  const out = [];
  for (let i = 48; i < 68; i += 1) {
    const dx = points[i][0] - crop.center[0];
    const dy = points[i][1] - crop.center[1];
    out.push([(dx * crop.ux[0] + dy * crop.ux[1]) / crop.scale + policy.tileW / 2, (dx * crop.uy[0] + dy * crop.uy[1]) / crop.scale + policy.tileH / 2]);
  }
  return out;
}

export function faceStreamMouthWidth(points) {
  return Math.hypot(points[54][0] - points[48][0], points[54][1] - points[48][1]) / faceFrame(points).span;
}

export function faceStreamLipRatio(points, offset = 0) {
  const at = (i) => points[i - offset];
  const width = Math.hypot(at(64)[0] - at(60)[0], at(64)[1] - at(60)[1]);
  const gap = Math.hypot(at(66)[0] - at(62)[0], at(66)[1] - at(62)[1]);
  return width > 0 ? gap / width : 0;
}

export function mouthState({ pose, expression, width = null, restWidth = null }, policy = FACE_STREAM_ATLAS) {
  if (!pose || !expression) return null;
  const turn = Math.max(Math.abs(pose.yaw || 0) / policy.maxYaw, Math.abs(pose.pitch || 0) / policy.maxPitch, Math.abs(pose.roll || 0) / policy.maxRoll);
  if (turn >= 1) return null;
  const m = expression.mouth || 0;
  const s = restWidth === null || width === null ? 0 : width / restWidth - 1;
  let state;
  let fit;
  if (m <= policy.closed) {
    state = FACE_STREAM_MOUTH.closed;
    fit = 1 - m / policy.closed;
  } else if (m >= policy.open) {
    state = FACE_STREAM_MOUTH.open;
    fit = Math.min(1, 0.5 + (m - policy.open) / 0.4);
  } else if (m >= policy.mid && restWidth !== null && s >= policy.wide) {
    state = FACE_STREAM_MOUTH.wide;
    fit = Math.min(1, 0.5 + (s - policy.wide) / 0.1);
  } else if (m >= policy.mid && restWidth !== null && s <= -policy.round) {
    state = FACE_STREAM_MOUTH.round;
    fit = Math.min(1, 0.5 + (-s - policy.round) / 0.1);
  } else {
    return null;
  }
  return { state, score: clampValue(fit, 0, 1) * (1 - 0.5 * turn) };
}

export function createFaceStreamSender({ width, height, fps, precision, keyInterval, policy = FACE_STREAM_REFERENCE, atlasPolicy = FACE_STREAM_ATLAS, documentRef = globalThis.document } = {}) {
  const encoder = createFaceStreamEncoder({ width, height, fps, precision, keyInterval });
  let last = null;
  let nextId = 0;
  let canvas = null;
  let restMouth = null;
  let openMax = null;
  let restWidth = null;
  const tiles = new Map();
  let tileCanvas = null;
  let atlasId = 0;
  let atlasTime = null;
  let atlasDirty = false;
  let credit = policy.budgetBurst;
  let creditTime = null;
  let referenceSize = policy.expectReference;
  let atlasSize = policy.expectAtlas;

  function refill(time) {
    if (creditTime !== null) credit = Math.min(policy.budgetBurst, credit + (policy.budgetRate * Math.max(0, time - creditTime)) / 1000);
    creditTime = time;
    return credit;
  }

  return {
    encoder,
    frame({ time, points, pose = null, reading = null, expression = null }) {
      const solved = pose || faceStreamPose(points);
      return encoder.encode({ time, points, pose: solved, expression: expression || faceStreamExpression(reading, points) });
    },
    score({ points, pose = null, reading = null, expression = null, light = null }) {
      const solved = pose || faceStreamPose(points);
      const measured = expression || faceStreamExpression(reading, points);
      restMouth = restMouth === null ? 0 : Math.min(measured.mouth, restMouth + policy.restRise);
      const opening = faceStreamEyeOpening(points);
      openMax = openMax === null ? Math.max(opening, policy.eyeOpen) : Math.max(opening, openMax - policy.eyeFall);
      const shut = clampValue(1 - opening / openMax, 0, 1);
      const relative = { ...measured, mouth: Math.max(0, measured.mouth - restMouth), lidLeft: Math.max(measured.lidLeft || 0, shut), lidRight: Math.max(measured.lidRight || 0, shut) };
      return referenceScore({ pose: solved, expression: relative, light, span: faceFrame(points).span }, policy);
    },
    due(time, score) {
      return referenceDue(last, time, score, policy) && refill(time) >= Math.min(referenceSize, policy.budgetBurst);
    },
    get credit() {
      return credit;
    },
    async reference({ time, source, points, mask = null, score = 1 }) {
      const crop = referenceCrop(points, width, height, policy);
      if (!canvas) {
        canvas = documentRef.createElement("canvas");
        canvas.width = policy.width;
        canvas.height = policy.height;
      }
      const ctx = canvas.getContext("2d");
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(source, crop.x, crop.y, crop.w, crop.h, 0, 0, policy.width, policy.height);
      const image = new Uint8Array(await canvasBytes(canvas, "image/jpeg", policy.quality));
      const bits = referenceMaskBits(mask, crop, policy.mask[0], policy.mask[1]);
      const packet = encodeFaceStreamReference({
        id: nextId,
        format: FACE_STREAM_FORMAT.jpeg,
        time: time >>> 0,
        cropX: crop.x,
        cropY: crop.y,
        cropW: crop.w,
        cropH: crop.h,
        width: policy.width,
        height: policy.height,
        points: referencePoints(points, crop, policy.width, policy.height),
        maskW: bits ? policy.mask[0] : 0,
        maskH: bits ? policy.mask[1] : 0,
        mask: bits,
        image,
      });
      nextId = (nextId + 1) & 15;
      last = { time, score };
      refill(time);
      credit -= packet.length;
      referenceSize = packet.length;
      return packet;
    },
    mouthTile({ time, source, points, pose = null, reading = null, expression = null }) {
      const solved = pose || faceStreamPose(points);
      const measured = expression || faceStreamExpression(reading, points);
      const width = faceStreamMouthWidth(points);
      if (measured.mouth <= atlasPolicy.closed) restWidth = restWidth === null ? width : restWidth + atlasPolicy.restRate * (width - restWidth);
      const pick = mouthState({ pose: solved, expression: measured, width, restWidth }, atlasPolicy);
      if (!pick || !(pick.score > 0)) return false;
      const crop = mouthTileCrop(points, atlasPolicy);
      if (crop.width < atlasPolicy.minWidth) return false;
      const have = tiles.get(pick.state);
      if (have && time - have.time < atlasPolicy.refreshMs && pick.score <= have.score + atlasPolicy.better) return false;
      if (!tileCanvas) {
        tileCanvas = documentRef.createElement("canvas");
        tileCanvas.width = atlasPolicy.tileW;
        tileCanvas.height = atlasPolicy.tileH;
      }
      const ctx = tileCanvas.getContext("2d", { willReadFrequently: true });
      const { center, ux, uy, scale } = crop;
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.clearRect(0, 0, atlasPolicy.tileW, atlasPolicy.tileH);
      ctx.imageSmoothingQuality = "high";
      ctx.setTransform(ux[0] / scale, uy[0] / scale, ux[1] / scale, uy[1] / scale,
        atlasPolicy.tileW / 2 - (center[0] * ux[0] + center[1] * ux[1]) / scale,
        atlasPolicy.tileH / 2 - (center[0] * uy[0] + center[1] * uy[1]) / scale);
      ctx.drawImage(source, 0, 0);
      ctx.setTransform(1, 0, 0, 1, 0, 0);
      tiles.set(pick.state, {
        state: pick.state,
        time,
        score: pick.score,
        mouth: measured.mouth,
        smile: measured.smile,
        points: mouthTilePoints(points, crop, atlasPolicy),
        pixels: ctx.getImageData(0, 0, atlasPolicy.tileW, atlasPolicy.tileH),
      });
      if (pick.state !== FACE_STREAM_MOUTH.closed || tiles.size > 1) atlasDirty = true;
      return true;
    },
    atlasDue(time) {
      const useful = [...tiles.keys()].some((state) => state !== FACE_STREAM_MOUTH.closed);
      return atlasDirty && useful && (atlasTime === null || time - atlasTime >= atlasPolicy.minIntervalMs) && refill(time) >= atlasSize;
    },
    async atlas(time) {
      const list = [...tiles.values()].sort((a, b) => a.state - b.state);
      const sheet = documentRef.createElement("canvas");
      sheet.width = atlasPolicy.tileW * list.length;
      sheet.height = atlasPolicy.tileH;
      const ctx = sheet.getContext("2d");
      list.forEach((tile, k) => ctx.putImageData(tile.pixels, k * atlasPolicy.tileW, 0));
      const image = new Uint8Array(await canvasBytes(sheet, "image/jpeg", atlasPolicy.quality));
      const packet = encodeFaceStreamAtlas({
        id: atlasId,
        time: time >>> 0,
        tileW: atlasPolicy.tileW,
        tileH: atlasPolicy.tileH,
        format: FACE_STREAM_FORMAT.jpeg,
        tiles: list.map(({ state, mouth, smile, points }) => ({ state, mouth, smile, points })),
        image,
      });
      atlasId = (atlasId + 1) & 15;
      atlasTime = time;
      atlasDirty = false;
      refill(time);
      credit -= packet.length;
      atlasSize = packet.length;
      return packet;
    },
    requestAtlas() {
      atlasDirty = tiles.size > 0;
      atlasTime = null;
    },
    get tiles() {
      return tiles.size;
    },
    requestKey() {
      encoder.requestKey();
    },
  };
}


export function faceStreamLivePoints(frame, reference, width, height) {
  if (!frame?.points || !reference) return null;
  const sx = width / reference.cropW;
  const sy = height / reference.cropH;
  const r3 = rotationRows(frame.pose.rotation)[2];
  const depth = frame.pose.scale * (sx + sy) * 0.5;
  return frame.points.map(([x, y], i) => [(x - reference.cropX) * sx, (y - reference.cropY) * sy, depth * dot3(r3, FACE_STREAM_TEMPLATE[i])]);
}

export function faceStreamMouthFrame(points) {
  const frame = faceFrame(points);
  let cx = 0;
  let cy = 0;
  for (let i = 60; i < 68; i += 1) {
    cx += points[i][0] / 8;
    cy += points[i][1] / 8;
  }
  const dx = points[64][0] - points[60][0];
  const dy = points[64][1] - points[60][1];
  const width = Math.hypot(dx, dy) || frame.span * 0.8;
  const ux = [dx / width, dy / width];
  const uy = [-ux[1], ux[0]];
  const gap = Math.abs((points[66][0] - points[62][0]) * uy[0] + (points[66][1] - points[62][1]) * uy[1]);
  const halfX = width / 2;
  const halfY = Math.max(gap / 2, 0.04 * frame.span);
  return { center: [cx, cy], x: [ux[0] / halfX, ux[1] / halfX], y: [uy[0] / halfY, uy[1] / halfY] };
}

export function faceStreamMouthShape(points) {
  const frame = faceFrame(points);
  let cx = 0;
  let cy = 0;
  for (let i = 60; i < 68; i += 1) {
    cx += points[i][0] / 8;
    cy += points[i][1] / 8;
  }
  const dx = points[64][0] - points[60][0];
  const dy = points[64][1] - points[60][1];
  const width = Math.hypot(dx, dy) || frame.span * 0.8;
  const axis = [dx / width, dy / width];
  const uy = [-axis[1], axis[0]];
  let gap = 0;
  for (const [up, down] of [[61, 67], [62, 66], [63, 65]]) {
    gap = Math.max(gap, (points[down][0] - points[up][0]) * uy[0] + (points[down][1] - points[up][1]) * uy[1]);
  }
  return { center: [cx, cy], axis, halfWidth: Math.max(width / 2, 1e-3), halfGap: Math.max(gap / 2, 0), span: frame.span };
}

const CONCEAL_MOUTH = Object.freeze([[56, 1], [57, 1], [58, 1], [65, 1], [66, 1], [67, 1], [55, 0.6], [59, 0.6], [7, 0.7], [8, 0.8], [9, 0.7], [6, 0.4], [10, 0.4]]);

export function faceStreamConceal(frame, goodMouth) {
  if (!frame?.stale || !Number.isFinite(goodMouth)) return frame;
  const delta = (frame.expression.mouth - goodMouth) * 0.5;
  if (Math.abs(delta) < 1e-6) return frame;
  const r = rotationRows(frame.pose.rotation);
  const down = [frame.pose.scale * r[0][1], frame.pose.scale * r[1][1]];
  const points = frame.points.map((p) => p.slice());
  for (const [i, weight] of CONCEAL_MOUTH) {
    points[i][0] += down[0] * delta * weight;
    points[i][1] += down[1] * delta * weight;
  }
  return { ...frame, points };
}

export function createFaceStreamReceiver() {
  const decoder = createFaceStreamDecoder();
  let frame = null;
  let reference = null;
  let atlas = null;
  let errors = 0;
  let goodMouth = NaN;
  return {
    receive(packet) {
      try {
        const kind = faceStreamPacketKind(packet);
        if (kind === FACE_STREAM_KIND.reference) {
          reference = decodeFaceStreamReference(packet);
          return { type: "reference", reference };
        }
        if (kind === FACE_STREAM_KIND.atlas) {
          atlas = decodeFaceStreamAtlas(packet);
          return { type: "atlas", atlas };
        }
        const decoded = decoder.decode(packet);
        if (!decoded.stale) goodMouth = decoded.expression.mouth;
        frame = faceStreamConceal(decoded, goodMouth);
        return { type: "frame", frame, needsKey: decoded.stale };
      } catch (error) {
        errors += 1;
        return { type: "error", error: error?.code || String(error) };
      }
    },
    get frame() {
      return frame;
    },
    get reference() {
      return reference;
    },
    get atlas() {
      return atlas;
    },
    get errors() {
      return errors;
    },
    livePoints(width, height) {
      return faceStreamLivePoints(frame, reference, width, height);
    },
  };
}

function smoothstep(a, b, x) {
  const t = clampValue((x - a) / (b - a), 0, 1);
  return t * t * (3 - 2 * t);
}

function boxBlurRGBA(data, w, h, radius) {
  const tmp = new Float32Array(data.length);
  const out = new Float32Array(data.length);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const sum = [0, 0, 0, 0];
      for (let k = -radius; k <= radius; k += 1) {
        const at = (y * w + clampValue(x + k, 0, w - 1)) * 4;
        for (let c = 0; c < 4; c += 1) sum[c] += data[at + c];
      }
      for (let c = 0; c < 4; c += 1) tmp[(y * w + x) * 4 + c] = sum[c] / (2 * radius + 1);
    }
  }
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const sum = [0, 0, 0, 0];
      for (let k = -radius; k <= radius; k += 1) {
        const at = (clampValue(y + k, 0, h - 1) * w + x) * 4;
        for (let c = 0; c < 4; c += 1) sum[c] += tmp[at + c];
      }
      for (let c = 0; c < 4; c += 1) out[(y * w + x) * 4 + c] = sum[c] / (2 * radius + 1);
    }
  }
  return out;
}

export function faceStreamBoxMean(values, w, h, radius) {
  const tmp = new Float32Array(w * h);
  const out = new Float32Array(w * h);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      let sum = 0;
      let n = 0;
      for (let k = Math.max(0, x - radius); k <= Math.min(w - 1, x + radius); k += 1) {
        sum += values[y * w + k];
        n += 1;
      }
      tmp[y * w + x] = sum / n;
    }
  }
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      let sum = 0;
      let n = 0;
      for (let k = Math.max(0, y - radius); k <= Math.min(h - 1, y + radius); k += 1) {
        sum += tmp[k * w + x];
        n += 1;
      }
      out[y * w + x] = sum / n;
    }
  }
  return out;
}

function maxFilter(values, w, h, radius) {
  const tmp = new Float32Array(w * h);
  const out = new Float32Array(w * h);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      let m = 0;
      for (let k = Math.max(0, x - radius); k <= Math.min(w - 1, x + radius); k += 1) m = Math.max(m, values[y * w + k]);
      tmp[y * w + x] = m;
    }
  }
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      let m = 0;
      for (let k = Math.max(0, y - radius); k <= Math.min(h - 1, y + radius); k += 1) m = Math.max(m, tmp[k * w + x]);
      out[y * w + x] = m;
    }
  }
  return out;
}

export function faceStreamCoarseMask(reference) {
  const { width: w, height: h, maskW: mw, maskH: mh, mask } = reference;
  const out = new Float32Array(w * h);
  if (!mask || !mw || !mh) return out;
  const at = (x, y) => mask[clampValue(y, 0, mh - 1) * mw + clampValue(x, 0, mw - 1)];
  for (let y = 0; y < h; y += 1) {
    const fy = ((y + 0.5) / h) * mh - 0.5;
    const y0 = Math.floor(fy);
    const ty = fy - y0;
    for (let x = 0; x < w; x += 1) {
      const fx = ((x + 0.5) / w) * mw - 0.5;
      const x0 = Math.floor(fx);
      const tx = fx - x0;
      const top = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
      const bottom = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
      out[y * w + x] = top * (1 - ty) + bottom * ty;
    }
  }
  return out;
}

export function faceStreamMatte(reference, pixels) {
  const { width: w, height: h } = reference;
  if (!reference.mask || !reference.maskW) return null;
  const coarse = faceStreamCoarseMask(reference);
  const guide = new Float32Array(w * h);
  for (let i = 0; i < w * h; i += 1) guide[i] = (0.299 * pixels[i * 4] + 0.587 * pixels[i * 4 + 1] + 0.114 * pixels[i * 4 + 2]) / 255;
  const radius = Math.max(2, Math.round(w / 64));
  const eps = 0.002;
  const ii = new Float32Array(w * h);
  const ip = new Float32Array(w * h);
  for (let i = 0; i < w * h; i += 1) {
    ii[i] = guide[i] * guide[i];
    ip[i] = guide[i] * coarse[i];
  }
  const meanI = faceStreamBoxMean(guide, w, h, radius);
  const meanP = faceStreamBoxMean(coarse, w, h, radius);
  const meanII = faceStreamBoxMean(ii, w, h, radius);
  const meanIP = faceStreamBoxMean(ip, w, h, radius);
  const a = new Float32Array(w * h);
  const b = new Float32Array(w * h);
  for (let i = 0; i < w * h; i += 1) {
    const variance = meanII[i] - meanI[i] * meanI[i];
    a[i] = (meanIP[i] - meanI[i] * meanP[i]) / (variance + eps);
    b[i] = meanP[i] - a[i] * meanI[i];
  }
  const meanA = faceStreamBoxMean(a, w, h, radius);
  const meanB = faceStreamBoxMean(b, w, h, radius);
  const band = faceStreamBoxMean(coarse, w, h, Math.max(1, Math.round(w / reference.maskW)));
  const out = new Float32Array(w * h);
  for (let i = 0; i < w * h; i += 1) {
    const q = clampValue(meanA[i] * guide[i] + meanB[i], 0, 1);
    out[i] = Math.max(q * smoothstep(0, 0.3, band[i]), smoothstep(0.7, 1, band[i]));
  }
  return out;
}

export function faceStreamLayers(reference, alpha) {
  const { width: w, height: h } = reference;
  const frame = faceFrame(reference.points);
  const chin = ((reference.points[8][0] - frame.mid[0]) * frame.uy[0] + (reference.points[8][1] - frame.mid[1]) * frame.uy[1]) / frame.span;
  const head = new Float32Array(w * h);
  const removal = new Float32Array(w * h);
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const i = y * w + x;
      const qy = ((x + 0.5 - frame.mid[0]) * frame.uy[0] + (y + 0.5 - frame.mid[1]) * frame.uy[1]) / frame.span;
      head[i] = alpha[i] * (1 - smoothstep(chin + 0.25, chin + 0.9, qy));
      removal[i] = alpha[i] * (1 - smoothstep(chin + 0.15, chin + 0.6, qy));
    }
  }
  return { head, removal };
}

export function faceStreamRemoval(reference, maskAt) {
  const frame = faceFrame(reference.points);
  const { width: w, height: h } = reference;
  const out = new Float32Array(w * h);
  if (!maskAt) return out;
  for (let y = 0; y < h; y += 1) {
    for (let x = 0; x < w; x += 1) {
      const dx = x + 0.5 - frame.mid[0];
      const dy = y + 0.5 - frame.mid[1];
      const qx = (dx * frame.ux[0] + dy * frame.ux[1]) / frame.span;
      const qy = (dx * frame.uy[0] + dy * frame.uy[1]) / frame.span;
      const zone = 1 - smoothstep(0.85, 1.1, Math.hypot(qx / 2.55, (qy + 0.35) / 2.9));
      const above = 1 - smoothstep(1.3, 1.8, qy);
      out[y * w + x] = maskAt(x, y) * zone * above;
    }
  }
  return out;
}

export function faceStreamGrow(values, w, h) {
  const radius = Math.max(1, Math.round(w / 128));
  const grown = maxFilter(values, w, h, radius);
  const soft = faceStreamBoxMean(grown, w, h, radius);
  const out = new Float32Array(w * h);
  for (let i = 0; i < w * h; i += 1) out[i] = Math.min(1, Math.max(grown[i], soft[i] * 1.5));
  return out;
}

function pushPull(pixels, w, h, removal) {
  const levels = [];
  let lw = w;
  let lh = h;
  let level = new Float32Array(w * h * 4);
  for (let i = 0; i < w * h; i += 1) {
    const keep = Math.pow(1 - clampValue(removal[i], 0, 1), 3);
    for (let c = 0; c < 3; c += 1) level[i * 4 + c] = (pixels[i * 4 + c] / 255) * keep;
    level[i * 4 + 3] = keep;
  }
  levels.push({ data: level, w: lw, h: lh });
  while (lw > 1 || lh > 1) {
    const nw = Math.max(1, Math.ceil(lw / 2));
    const nh = Math.max(1, Math.ceil(lh / 2));
    const next = new Float32Array(nw * nh * 4);
    for (let y = 0; y < nh; y += 1) {
      for (let x = 0; x < nw; x += 1) {
        for (let k = 0; k < 4; k += 1) {
          const sx = Math.min(lw - 1, x * 2 + (k & 1));
          const sy = Math.min(lh - 1, y * 2 + (k >> 1));
          for (let c = 0; c < 4; c += 1) next[(y * nw + x) * 4 + c] += level[(sy * lw + sx) * 4 + c] / 4;
        }
      }
    }
    level = next;
    lw = nw;
    lh = nh;
    levels.push({ data: level, w: lw, h: lh });
  }
  let coarse = null;
  for (let li = levels.length - 1; li >= 0; li -= 1) {
    const { data, w: cw, h: ch } = levels[li];
    const filled = new Float32Array(cw * ch * 4);
    for (let y = 0; y < ch; y += 1) {
      for (let x = 0; x < cw; x += 1) {
        const at = (y * cw + x) * 4;
        const a = data[at + 3];
        const trust = coarse ? clampValue(a * 3, 0, 1) : 1;
        let fx = 0;
        let fy = 0;
        let x0 = 0;
        let y0 = 0;
        if (coarse) {
          const sx = clampValue((x + 0.5) / 2 - 0.5, 0, coarse.w - 1);
          const sy = clampValue((y + 0.5) / 2 - 0.5, 0, coarse.h - 1);
          x0 = Math.floor(sx);
          y0 = Math.floor(sy);
          fx = sx - x0;
          fy = sy - y0;
        }
        for (let c = 0; c < 3; c += 1) {
          const mine = a > 1e-4 ? data[at + c] / a : 0.5;
          let under = mine;
          if (coarse) {
            const x1 = Math.min(coarse.w - 1, x0 + 1);
            const y1 = Math.min(coarse.h - 1, y0 + 1);
            const tap = (px, py) => coarse.data[(py * coarse.w + px) * 4 + c];
            under = (tap(x0, y0) * (1 - fx) + tap(x1, y0) * fx) * (1 - fy) + (tap(x0, y1) * (1 - fx) + tap(x1, y1) * fx) * fy;
          }
          filled[at + c] = under + (mine - under) * trust;
        }
        filled[at + 3] = 1;
      }
    }
    coarse = { data: filled, w: cw, h: ch };
  }
  return coarse.data;
}

export function faceStreamBackdrop(pixels, w, h, removal, blur = 2) {
  const filled = pushPull(pixels, w, h, removal);
  const soft = blur > 0 ? boxBlurRGBA(boxBlurRGBA(filled, w, h, blur), w, h, blur) : filled;
  const out = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i += 1) {
    for (let c = 0; c < 3; c += 1) out[i * 4 + c] = Math.round(soft[i * 4 + c] * 255);
    out[i * 4 + 3] = 255;
  }
  return out;
}

export function faceStreamPlate(pixels, w, h, removal) {
  const filled = pushPull(pixels, w, h, removal);
  const radius = Math.max(1, Math.round(w / 100));
  const soft = boxBlurRGBA(boxBlurRGBA(filled, w, h, radius), w, h, radius);
  const out = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i += 1) {
    const r = clampValue(removal[i], 0, 1);
    for (let c = 0; c < 3; c += 1) out[i * 4 + c] = Math.round(pixels[i * 4 + c] * (1 - r) + soft[i * 4 + c] * 255 * r);
    out[i * 4 + 3] = 255;
  }
  return out;
}

export function faceStreamUnmix(pixels, plate, alpha) {
  const out = new Uint8ClampedArray(pixels.length);
  for (let i = 0; i < alpha.length; i += 1) {
    const a = alpha[i];
    for (let c = 0; c < 3; c += 1) {
      const at = i * 4 + c;
      out[at] = a > 0.98 || a < 0.01 ? pixels[at] : Math.round((pixels[at] - (1 - a) * plate[at]) / Math.max(a, 0.05));
    }
    out[i * 4 + 3] = 255;
  }
  return out;
}

function insideRing(points, ring, x, y) {
  let inside = false;
  for (let i = 0, j = ring.length - 1; i < ring.length; j = i, i += 1) {
    const [xi, yi] = points[ring[i]];
    const [xj, yj] = points[ring[j]];
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

const OUTER_RING = Object.freeze([48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59]);
const INNER_RING = Object.freeze([60, 61, 62, 63, 64, 65, 66, 67]);

function lipRingPoints(points, offset = 0) {
  const at = (i) => points[i - offset];
  let cx = 0;
  let cy = 0;
  for (const i of OUTER_RING) {
    cx += at(i)[0] / 12;
    cy += at(i)[1] / 12;
  }
  const out = [];
  for (let i = 0; i < 68; i += 1) out.push(i >= offset && i < offset + points.length ? at(i) : [0, 0]);
  const feather = OUTER_RING.map((i) => [cx + (at(i)[0] - cx) * 1.35, cy + (at(i)[1] - cy) * 1.35]);
  return { points: out, feather };
}

function regionMean(pixels, w, h, test, bounds) {
  const sum = [0, 0, 0];
  let n = 0;
  const x0 = clampValue(Math.floor(bounds[0]), 0, w - 1);
  const x1 = clampValue(Math.ceil(bounds[2]), 0, w - 1);
  const y0 = clampValue(Math.floor(bounds[1]), 0, h - 1);
  const y1 = clampValue(Math.ceil(bounds[3]), 0, h - 1);
  const values = [];
  for (let y = y0; y <= y1; y += 1) {
    for (let x = x0; x <= x1; x += 1) {
      if (!test(x + 0.5, y + 0.5)) continue;
      const at = (y * w + x) * 4;
      values.push([pixels[at] / 255, pixels[at + 1] / 255, pixels[at + 2] / 255]);
      for (let c = 0; c < 3; c += 1) sum[c] += pixels[at + c] / 255;
      n += 1;
    }
  }
  return n ? { mean: sum.map((v) => v / n), values } : null;
}

function boundsOf(list) {
  const xs = list.map((p) => p[0]);
  const ys = list.map((p) => p[1]);
  return [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)];
}

function featherTest(ring) {
  return (x, y) => {
    let inside = false;
    for (let i = 0, j = ring.length - 1; i < ring.length; j = i, i += 1) {
      const [xi, yi] = ring[i];
      const [xj, yj] = ring[j];
      if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
    }
    return inside;
  };
}

function lipSkinMean(pixels, w, h, points, offset = 0) {
  const { points: all, feather } = lipRingPoints(points, offset);
  const outside = featherTest(feather);
  return regionMean(pixels, w, h, (x, y) => outside(x, y) && !insideRing(all, OUTER_RING, x, y), boundsOf(feather))?.mean || null;
}

export function faceStreamMouthColours(pixels, w, h, points) {
  const lips = regionMean(pixels, w, h, (x, y) => insideRing(points, OUTER_RING, x, y) && !insideRing(points, INNER_RING, x, y), boundsOf(OUTER_RING.map((i) => points[i])));
  const skin = lipSkinMean(pixels, w, h, points) || [0.62, 0.48, 0.4];
  const luma = (c) => 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2];
  const lip = lips?.mean || [skin[0] * 0.85, skin[1] * 0.6, skin[2] * 0.62];
  let teeth = null;
  if (mouthGap(points) > 0.06) {
    const inner = regionMean(pixels, w, h, (x, y) => insideRing(points, INNER_RING, x, y), boundsOf(INNER_RING.map((i) => points[i])));
    if (inner && inner.values.length >= 6) {
      const sorted = inner.values.slice().sort((a, b) => luma(b) - luma(a));
      const top = sorted.slice(0, Math.max(2, Math.floor(sorted.length * 0.3)));
      const mean = [0, 1, 2].map((c) => top.reduce((sum, v) => sum + v[c], 0) / top.length);
      if (luma(mean) > luma(skin) * 0.9) teeth = mean;
    }
  }
  if (!teeth) {
    const level = clampValue(luma(skin) / 0.55, 0.55, 1.15);
    teeth = [0.86 * level, 0.83 * level, 0.76 * level].map((v) => clampValue(v, 0, 1));
  }
  return { lip, teeth, skin };
}

export function faceStreamAtlasGains(atlasPixels, atlas, pixels, w, h, points) {
  const reference = lipSkinMean(pixels, w, h, points);
  return atlas.tiles.map((tile, k) => {
    if (!reference) return [1, 1, 1];
    const tilePixels = new Uint8ClampedArray(atlas.tileW * atlas.tileH * 4);
    for (let y = 0; y < atlas.tileH; y += 1) {
      for (let x = 0; x < atlas.tileW; x += 1) {
        const from = (y * atlasPixels.width + k * atlas.tileW + x) * 4;
        const to = (y * atlas.tileW + x) * 4;
        for (let c = 0; c < 4; c += 1) tilePixels[to + c] = atlasPixels.data[from + c];
      }
    }
    const mine = lipSkinMean(tilePixels, atlas.tileW, atlas.tileH, tile.points, 48);
    if (!mine) return [1, 1, 1];
    return mine.map((v, c) => clampValue((reference[c] + 0.01) / (v + 0.01), 0.6, 1.6));
  });
}

export function faceStreamMouthBlend(tiles, expression, points = null) {
  if (!tiles?.length || !expression) return null;
  const m = expression.mouth || 0;
  const s = expression.smile || 0;
  const r = points ? faceStreamLipRatio(points) : null;
  const floor = Math.max(0.08, 0.5 * m);
  const ranked = tiles.map((tile, i) => ({
    i,
    d: Math.abs(m - tile.mouth) + 0.5 * Math.abs(s - tile.smile) + (r === null ? 0 : 0.5 * Math.abs(r - faceStreamLipRatio(tile.points, 48))),
    open: tile.mouth >= floor,
  })).filter((entry) => entry.open).sort((a, b) => a.d - b.d || a.i - b.i);
  if (!ranked.length) return null;
  const a = ranked[0];
  const b = ranked[1] || a;
  const mix = a === b || a.d + b.d < 1e-9 ? 0 : a.d / (a.d + b.d);
  const opacity = smoothstep(0.08, 0.2, m) * (1 - smoothstep(0.3, 0.6, a.d));
  return { a: a.i, b: b.i, mix, opacity };
}

const MOUTH_BAND = Object.freeze([[48, 60], [49, 61], [50, 61], [51, 62], [52, 63], [53, 63], [54, 64], [55, 65], [56, 65], [57, 66], [58, 67], [59, 67]]);
export const MOUTH_VERTEX_FLOATS = 7;

export function faceStreamMouthMesh(points, tileA, tileB, tileW, tileH, lipAlpha = 0.85) {
  const live = lipRingPoints(points.slice(48, 68), 48);
  const a = lipRingPoints(tileA.points, 48);
  const b = lipRingPoints(tileB.points, 48);
  const centre = (set) => {
    let cx = 0;
    let cy = 0;
    for (const i of INNER_RING) {
      cx += set.points[i][0] / 8;
      cy += set.points[i][1] / 8;
    }
    return [cx, cy];
  };
  const vertex = (key) => {
    if (key === "c") return [centre(live), centre(a), centre(b), 1];
    if (key[0] === "f") {
      const k = Number(key.slice(1));
      return [live.feather[k], a.feather[k], b.feather[k], 0];
    }
    return [live.points[key], a.points[key], b.points[key], key >= 60 ? 1 : lipAlpha];
  };
  const triangles = [];
  for (let k = 0; k < 8; k += 1) triangles.push(["c", INNER_RING[k], INNER_RING[(k + 1) % 8]]);
  for (let j = 0; j < 12; j += 1) {
    const [o1, i1] = MOUTH_BAND[j];
    const [o2, i2] = MOUTH_BAND[(j + 1) % 12];
    triangles.push([o1, o2, i2]);
    if (i1 !== i2) triangles.push([o1, i2, i1]);
  }
  for (let j = 0; j < 12; j += 1) {
    const o1 = OUTER_RING[j];
    const o2 = OUTER_RING[(j + 1) % 12];
    triangles.push([o1, o2, `f${(j + 1) % 12}`]);
    triangles.push([o1, `f${(j + 1) % 12}`, `f${j}`]);
  }
  const out = new Float32Array(triangles.length * 3 * MOUTH_VERTEX_FLOATS);
  let at = 0;
  for (const triangle of triangles) {
    for (const key of triangle) {
      const [p, ua, ub, alpha] = vertex(key);
      out[at++] = p[0];
      out[at++] = p[1];
      out[at++] = ua[0] / tileW;
      out[at++] = ua[1] / tileH;
      out[at++] = ub[0] / tileW;
      out[at++] = ub[1] / tileH;
      out[at++] = alpha;
    }
  }
  return out;
}

export function faceStreamWarpPoints(headPoints, drawn, mouthStart, anchor = null) {
  const out = headPoints.slice(0, mouthStart).map((p) => [p[0], p[1]]);
  if (anchor) {
    const live = faceFrame(drawn);
    const turn = live.roll - anchor.roll;
    const grow = live.span / anchor.span;
    const c = Math.cos(turn) * grow;
    const sn = Math.sin(turn) * grow;
    for (let v = 0; v < mouthStart; v += 1) {
      const [dx, dy] = anchor.offsets[v];
      out[v] = [out[v][0] + c * dx - sn * dy, out[v][1] + sn * dx + c * dy];
    }
  }
  for (let i = 17; i < COUNT; i += 1) out[i] = [drawn[i][0], drawn[i][1]];
  const frame = faceFrame(out);
  for (const [up, down] of [[61, 67], [62, 66], [63, 65]]) {
    const gap = (out[down][0] - out[up][0]) * frame.uy[0] + (out[down][1] - out[up][1]) * frame.uy[1];
    if (gap < 0) {
      const mid = [(out[up][0] + out[down][0]) / 2, (out[up][1] + out[down][1]) / 2];
      out[up] = mid.slice();
      out[down] = mid.slice();
    }
  }
  return out.concat(mouthExtras(out));
}

export function faceStreamAnchor(face, place, width, height) {
  const sx = width / place.width;
  const sy = height / place.height;
  const pose = place.pose;
  const r3 = rotationRows(pose.rotation)[2];
  const depth = pose.scale * (sx + sy) * 0.5;
  const live = place.points.map(([x, y], i) => [x * sx, y * sy, depth * dot3(r3, FACE_STREAM_TEMPLATE[i])]);
  const head = liveHead(face.model, live);
  const [a, b, c, d, e, f] = place.matrix;
  const det = a * d - b * c || 1e-9;
  const mouthStart = face.mesh.mouthStart ?? head.points.length - MOUTH_EXTRA_COUNT;
  const offsets = [];
  for (let v = 0; v < mouthStart; v += 1) {
    const [lx, ly] = face.layout[v];
    const px = (d * (lx - e) - c * (ly - f)) / det - place.pad;
    const py = (-b * (lx - e) + a * (ly - f)) / det - place.pad;
    offsets.push([px * sx - head.points[v][0], py * sy - head.points[v][1]]);
  }
  const frame = faceFrame(live);
  return { offsets, roll: frame.roll, span: frame.span, width, height };
}

const STREAM_QUAD = `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const STREAM_MOUTH_VERTEX = `
attribute vec2 aPos;
attribute vec2 aUvA;
attribute vec2 aUvB;
attribute float aAlpha;
uniform vec2 uSize;
varying vec2 vUvA;
varying vec2 vUvB;
varying float vAlpha;
void main() {
  vUvA = aUvA;
  vUvB = aUvB;
  vAlpha = aAlpha;
  gl_Position = vec4(aPos / uSize * 2.0 - 1.0, 0.0, 1.0);
}`;

const STREAM_MOUTH_FRAGMENT = `
precision highp float;
uniform sampler2D uAtlas;
uniform float uTiles;
uniform float uTileA;
uniform float uTileB;
uniform float uMix;
uniform vec2 uHalf;
uniform vec3 uGainA;
uniform vec3 uGainB;
varying vec2 vUvA;
varying vec2 vUvB;
varying float vAlpha;
vec3 tile(float index, vec2 uv) {
  vec2 q = clamp(uv, uHalf, vec2(1.0) - uHalf);
  return texture2D(uAtlas, vec2((index + q.x) / uTiles, q.y)).rgb;
}
void main() {
  vec3 a = tile(uTileA, vUvA) * uGainA;
  vec3 b = tile(uTileB, vUvB) * uGainB;
  gl_FragColor = vec4(clamp(mix(a, b, uMix), 0.0, 1.0), vAlpha);
}`;

const STREAM_COMPOSITE = `
precision highp float;
uniform sampler2D uBackdrop;
uniform sampler2D uWarp;
uniform sampler2D uAux;
uniform sampler2D uMouth;
uniform vec2 uPixels;
uniform vec2 uMouthCenter;
uniform vec2 uMouthAxis;
uniform vec3 uMouthSize;
uniform vec3 uLip;
uniform vec3 uTeeth;
uniform float uTongue;
uniform float uFace;
uniform float uSeed;
uniform float uMouthOn;
varying vec2 vUv;
float grain(vec2 at) {
  return fract(sin(dot(floor(at) + vec2(uSeed, uSeed * 0.61), vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
}
vec3 interior(vec2 p, float cavity) {
  vec2 d = p - uMouthCenter;
  vec2 ay = vec2(-uMouthAxis.y, uMouthAxis.x);
  float x = dot(d, uMouthAxis) / uMouthSize.x;
  float y = dot(d, ay);
  float opening = uMouthSize.y;
  float span = uMouthSize.z;
  float top = y + opening;
  float bottom = opening - y;
  float gap = 2.0 * opening / span;
  vec3 c = mix(uLip * 0.3, uLip * 0.07, smoothstep(0.0, 1.0, cavity));
  c *= mix(0.55, 1.0, smoothstep(0.0, 0.6, abs(x)));
  float floorShow = max(uTongue, 0.6 * smoothstep(0.18, 0.4, gap));
  vec2 tq = vec2(x / 0.7, (bottom - 0.3 * opening) / (0.55 * opening + 0.5));
  float tongue = (1.0 - smoothstep(0.5, 1.0, length(tq))) * floorShow;
  c = mix(c, uLip * vec3(0.95, 0.62, 0.66), tongue);
  float height = 0.12 * span * max(1.0 - 1.2 * x * x, 0.0);
  float upper = (1.0 - smoothstep(height * 0.75, height, top)) * (1.0 - smoothstep(0.6, 0.85, abs(x))) * smoothstep(0.04, 0.16, gap);
  float gaps = 1.0 - 0.15 * smoothstep(0.9, 1.0, cos(x * 18.85));
  vec3 tooth = uTeeth * mix(0.55, 0.85, smoothstep(0.0, height + 0.01, top)) * (1.0 - 0.45 * x * x) * gaps;
  c = mix(c, tooth, upper);
  float lowHeight = 0.07 * span * max(1.0 - 1.6 * x * x, 0.0);
  float lower = (1.0 - smoothstep(lowHeight * 0.75, lowHeight, bottom)) * smoothstep(0.22, 0.4, gap) * (1.0 - smoothstep(0.45, 0.65, abs(x)));
  c = mix(c, uTeeth * 0.55, lower * 0.6);
  c *= mix(0.6, 1.0, smoothstep(0.0, 0.03 * span, top));
  return mix(uLip * 0.62, c, smoothstep(0.03, 0.1, gap));
}
void main() {
  vec2 p = vUv * uPixels;
  vec3 color = texture2D(uBackdrop, vUv).rgb;
  if (uFace > 0.5) {
    vec4 warp = texture2D(uWarp, vUv);
    vec4 aux = texture2D(uAux, vUv);
    float hole = min(aux.b * 2.0, 1.0);
    float cavity = max(aux.b * 2.0 - 1.0, 0.0);
    color = mix(color, interior(p, cavity), hole);
    color *= 1.0 - 0.18 * hole * (1.0 - hole) * 4.0;
    vec3 face = clamp(warp.rgb + grain(p) * 0.006, 0.0, 1.0);
    color = mix(color, face, warp.a);
    if (uMouthOn > 0.001) {
      vec4 mouth = texture2D(uMouth, vUv);
      color = mix(color, mouth.rgb, mouth.a * uMouthOn);
    }
  }
  gl_FragColor = vec4(color, 1.0);
}`;

export const FACE_STREAM_WARP_FRAGMENT = FACE_WARP.fragment.replace(/const float SOFTEN = [0-9.]+;/, "const float SOFTEN = 0.0;");

export function createFaceStreamRenderer(canvas) {
  const gl = faceWebGL(canvas);
  if (!gl) throw new Error("WebGL is unavailable");
  const { compile, texture, target } = FACE_WARP;
  const warp = compile(gl, FACE_WARP.vertex, FACE_STREAM_WARP_FRAGMENT);
  const mouthProgram = compile(gl, STREAM_MOUTH_VERTEX, STREAM_MOUTH_FRAGMENT);
  const composite = compile(gl, STREAM_QUAD, STREAM_COMPOSITE);
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const meshBuffer = gl.createBuffer();
  const mouthBuffer = gl.createBuffer();
  const photo = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  const aux = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  const backdrop = texture(gl);
  const atlasTexture = texture(gl);
  gl.bindTexture(gl.TEXTURE_2D, backdrop);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([44, 46, 52, 255]));
  let face = null;
  let targets = null;
  let width = 0;
  let height = 0;
  let count = 0;
  let atlas = null;
  let colours = { lip: [0.55, 0.3, 0.3], teeth: [0.86, 0.83, 0.76] };

  function resize(w, h) {
    if (targets && w === width && h === height) return;
    width = w;
    height = h;
    canvas.width = w;
    canvas.height = h;
    targets = { warp: target(gl, w, h, gl.UNSIGNED_BYTE, true), aux: target(gl, w, h, gl.UNSIGNED_BYTE, true), mouth: target(gl, w, h, gl.UNSIGNED_BYTE) };
  }

  function bind(program, entries) {
    entries.forEach(([name, handle], unit) => {
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_2D, handle);
      if (program.uniforms[name]) gl.uniform1i(program.uniforms[name], unit);
    });
  }

  function upload(handle, image, mip) {
    gl.bindTexture(gl.TEXTURE_2D, handle);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    if (image.data && image.width) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, image.width, image.height, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(image.data.buffer, image.data.byteOffset, image.data.byteLength));
    } else {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image);
    }
    if (mip) gl.generateMipmap(gl.TEXTURE_2D);
  }

  function attributes(program, layout, stride) {
    return layout.map(([name, size, offset]) => {
      const at = gl.getAttribLocation(program.program, name);
      if (at >= 0) {
        gl.enableVertexAttribArray(at);
        gl.vertexAttribPointer(at, size, gl.FLOAT, false, stride * 4, offset);
      }
      return at;
    }).filter((at) => at >= 0);
  }

  function drawMouth(points, mouth) {
    const tileA = atlas.atlas.tiles[mouth.a];
    const tileB = atlas.atlas.tiles[mouth.b];
    const vertices = faceStreamMouthMesh(points, tileA, tileB, atlas.atlas.tileW, atlas.atlas.tileH);
    gl.bindFramebuffer(gl.FRAMEBUFFER, targets.mouth.framebuffer);
    gl.viewport(0, 0, width, height);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.useProgram(mouthProgram.program);
    gl.bindBuffer(gl.ARRAY_BUFFER, mouthBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
    const attrs = attributes(mouthProgram, [["aPos", 2, 0], ["aUvA", 2, 8], ["aUvB", 2, 16], ["aAlpha", 1, 24]], MOUTH_VERTEX_FLOATS);
    bind(mouthProgram, [["uAtlas", atlasTexture]]);
    const u = mouthProgram.uniforms;
    gl.uniform2f(u.uSize, width, height);
    gl.uniform1f(u.uTiles, atlas.atlas.tiles.length);
    gl.uniform1f(u.uTileA, mouth.a);
    gl.uniform1f(u.uTileB, mouth.b);
    gl.uniform1f(u.uMix, mouth.mix);
    gl.uniform2f(u.uHalf, 0.5 / atlas.atlas.tileW, 0.5 / atlas.atlas.tileH);
    gl.uniform3f(u.uGainA, ...(atlas.gains[mouth.a] || [1, 1, 1]));
    gl.uniform3f(u.uGainB, ...(atlas.gains[mouth.b] || [1, 1, 1]));
    gl.drawArrays(gl.TRIANGLES, 0, vertices.length / MOUTH_VERTEX_FLOATS);
    for (const at of attrs) gl.disableVertexAttribArray(at);
  }

  return {
    gl,
    setFace({ photo: image, matte, mesh, layout, model, size, place = null }) {
      upload(photo, image, true);
      upload(aux, matte, true);
      face = { mesh, layout, model, size, eyes: photoEyes(layout, size), place, anchor: null };
    },
    clearFace() {
      face = null;
    },
    setBackdrop(image) {
      upload(backdrop, image, false);
    },
    setColours(next) {
      colours = { ...colours, ...next };
    },
    setReference(prepared) {
      this.setBackdrop(prepared.backdrop);
      this.setFace({ photo: prepared.built.photo, matte: prepared.built.matte, mesh: prepared.head.mesh, layout: prepared.head.layout, model: prepared.head.model, size: 512, place: prepared.place || null });
      if (prepared.colours) this.setColours(prepared.colours);
    },
    setAtlas(prepared, gains) {
      if (!prepared) {
        atlas = null;
        return;
      }
      upload(atlasTexture, prepared.image, false);
      atlas = { atlas: prepared.atlas, gains: gains || prepared.atlas.tiles.map(() => [1, 1, 1]) };
    },
    get hasAtlas() {
      return Boolean(atlas);
    },
    draw(w, h, live, expression = null, { mouth = null } = {}) {
      if (gl.isContextLost()) return null;
      resize(w, h);
      gl.disable(gl.BLEND);
      let head = null;
      let points = null;
      const showFace = Boolean(face && live);
      if (showFace) {
        const close = [expression?.lidLeft || 0, expression?.lidRight || 0];
        const drawn = closeLids(live, close);
        head = liveHead(face.model, drawn);
        if (face.place && (!face.anchor || face.anchor.width !== w || face.anchor.height !== h)) face.anchor = faceStreamAnchor(face, face.place, w, h);
        points = faceStreamWarpPoints(head.points, drawn, face.mesh.mouthStart ?? head.points.length - MOUTH_EXTRA_COUNT, face.anchor);
        const vertices = warpVertices(face.mesh, face.layout, points, face.size, vertexDepths(face.model, head.pose));
        gl.useProgram(warp.program);
        gl.bindBuffer(gl.ARRAY_BUFFER, meshBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
        const attrs = attributes(warp, [["aPos", 2, 0], ["aUv", 2, 8], ["aHole", 1, 16], ["aRegion", 1, 20], ["aDepth", 1, 24]], VERTEX_FLOATS);
        bind(warp, [["uPhoto", photo], ["uAux", aux]]);
        const look = [[expression?.gazeX || 0, expression?.gazeY || 0], [expression?.gazeX || 0, expression?.gazeY || 0]];
        gl.uniform2f(warp.uniforms.uSize, w, h);
        gl.uniform4f(warp.uniforms.uEyeL, ...face.eyes[0]);
        gl.uniform4f(warp.uniforms.uEyeR, ...face.eyes[1]);
        gl.uniform4f(warp.uniforms.uLook, ...lookUniform(look));
        gl.uniform2f(warp.uniforms.uClose, close[0], close[1]);
        for (const [out, pass] of [[targets.warp, 0], [targets.aux, 1]]) {
          gl.bindFramebuffer(gl.FRAMEBUFFER, out.framebuffer);
          gl.viewport(0, 0, w, h);
          gl.clearColor(0, 0, 0, 0);
          if (out.depth) {
            gl.enable(gl.DEPTH_TEST);
            gl.depthFunc(gl.LESS);
            gl.depthMask(true);
            gl.clearDepth(1);
            gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
          } else {
            gl.clear(gl.COLOR_BUFFER_BIT);
          }
          gl.uniform1f(warp.uniforms.uAuxPass, pass);
          gl.drawArrays(gl.TRIANGLES, 0, vertices.length / VERTEX_FLOATS);
          gl.disable(gl.DEPTH_TEST);
        }
        for (const at of attrs) gl.disableVertexAttribArray(at);
      }
      const mouthOn = showFace && atlas && mouth && mouth.opacity > 0.001 ? mouth.opacity : 0;
      if (mouthOn > 0) drawMouth(points, mouth);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, w, h);
      gl.useProgram(composite.program);
      bind(composite, [["uBackdrop", backdrop], ["uWarp", targets.warp.texture], ["uAux", targets.aux.texture], ["uMouth", targets.mouth.texture]]);
      const shape = points ? faceStreamMouthShape(points) : { center: [-1e5, -1e5], axis: [1, 0], halfWidth: 1, halfGap: 0, span: 1 };
      const u = composite.uniforms;
      gl.uniform2f(u.uPixels, w, h);
      gl.uniform2f(u.uMouthCenter, shape.center[0], shape.center[1]);
      gl.uniform2f(u.uMouthAxis, shape.axis[0], shape.axis[1]);
      gl.uniform3f(u.uMouthSize, shape.halfWidth, shape.halfGap, shape.span);
      gl.uniform3f(u.uLip, ...colours.lip);
      gl.uniform3f(u.uTeeth, ...colours.teeth);
      gl.uniform1f(u.uTongue, showFace ? clampValue(expression?.tongue || 0, 0, 1) : 0);
      gl.uniform1f(u.uFace, showFace ? 1 : 0);
      gl.uniform1f(u.uMouthOn, mouthOn);
      count = (count + 1) % 4096;
      gl.uniform1f(u.uSeed, (count * 7.31) % 97);
      gl.bindBuffer(gl.ARRAY_BUFFER, quad);
      const at = gl.getAttribLocation(composite.program, "aPos");
      gl.enableVertexAttribArray(at);
      gl.vertexAttribPointer(at, 2, gl.FLOAT, false, 0, 0);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      gl.disableVertexAttribArray(at);
      return head ? { ...head, points } : null;
    },
    dispose() {
      gl.getExtension("WEBGL_lose_context")?.loseContext();
    },
  };
}

const MIME = ["image/jpeg", "image/heic", "image/webp"];

async function decodeImage(bytes, format, ImageImpl) {
  const blob = new Blob([bytes], { type: MIME[format] || "image/jpeg" });
  if (typeof globalThis.createImageBitmap === "function") return globalThis.createImageBitmap(blob);
  const url = globalThis.URL.createObjectURL(blob);
  try {
    const image = new ImageImpl();
    image.src = url;
    await image.decode();
    return image;
  } finally {
    globalThis.URL.revokeObjectURL(url);
  }
}

function pixelCanvas(documentRef, data, w, h) {
  const canvas = documentRef.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d");
  const image = ctx.createImageData(w, h);
  image.data.set(data);
  ctx.putImageData(image, 0, 0);
  return canvas;
}

export const FACE_STREAM_PAD = 0.2;

function padFloats(values, w, h, pad) {
  const pw = w + 2 * pad;
  const ph = h + 2 * pad;
  const out = new Float32Array(pw * ph);
  for (let y = 0; y < ph; y += 1) {
    const sy = clampValue(y - pad, 0, h - 1);
    for (let x = 0; x < pw; x += 1) out[y * pw + x] = values[sy * w + clampValue(x - pad, 0, w - 1)];
  }
  return out;
}

function padPixels(pixels, w, h, pad) {
  const pw = w + 2 * pad;
  const ph = h + 2 * pad;
  const out = new Uint8ClampedArray(pw * ph * 4);
  for (let y = 0; y < ph; y += 1) {
    const sy = clampValue(y - pad, 0, h - 1);
    for (let x = 0; x < pw; x += 1) {
      const from = (sy * w + clampValue(x - pad, 0, w - 1)) * 4;
      const to = (y * pw + x) * 4;
      for (let c = 0; c < 4; c += 1) out[to + c] = pixels[from + c];
    }
  }
  return out;
}

export const FACE_STREAM_PHOTO = Object.freeze({ size: 512, span: 96, mid: Object.freeze([256, 210]) });

export function faceStreamPhotoTransform(points, photo = FACE_STREAM_PHOTO) {
  const frame = faceFrame(points);
  const scale = photo.span / frame.span;
  const cos = Math.cos(-frame.roll) * scale;
  const sin = Math.sin(-frame.roll) * scale;
  return [cos, sin, -sin, cos, photo.mid[0] - (cos * frame.mid[0] - sin * frame.mid[1]), photo.mid[1] - (sin * frame.mid[0] + cos * frame.mid[1])];
}

function placeLayer(documentRef, source, w, h, matrix, size, background = "#000") {
  const canvas = documentRef.createElement("canvas");
  canvas.width = size;
  canvas.height = size;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.fillStyle = background;
  ctx.fillRect(0, 0, size, size);
  ctx.setTransform(...matrix);
  ctx.imageSmoothingQuality = "high";
  ctx.drawImage(source, 0, 0, w, h);
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  return canvas;
}

function greyCanvas(documentRef, values, w, h) {
  const grey = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i += 1) {
    const v = Math.round(values[i] * 255);
    grey[i * 4] = v;
    grey[i * 4 + 1] = v;
    grey[i * 4 + 2] = v;
    grey[i * 4 + 3] = 255;
  }
  return pixelCanvas(documentRef, grey, w, h);
}

export function buildFaceStreamFace({ image, width: w, height: h, points, alpha, head, documentRef = globalThis.document, photo: layout = FACE_STREAM_PHOTO }) {
  const size = layout.size;
  const matrix = faceStreamPhotoTransform(points, layout);
  const photo = placeLayer(documentRef, image, w, h, matrix, size);
  const pixels = photo.getContext("2d", { willReadFrequently: true }).getImageData(0, 0, size, size).data;
  const read = (values) => {
    const data = placeLayer(documentRef, greyCanvas(documentRef, values, w, h), w, h, matrix, size).getContext("2d", { willReadFrequently: true }).getImageData(0, 0, size, size).data;
    const out = new Float32Array(size * size);
    for (let i = 0; i < out.length; i += 1) out[i] = data[i * 4] / 255;
    return out;
  };
  const person = read(alpha);
  const headMask = read(head);
  const placed = points.map((p) => applyTransform(matrix, p));
  const parts = buildCustomMatte({ pixels, size, points: placed, person, documentRef });
  const matte = documentRef.createElement("canvas");
  matte.width = size;
  matte.height = size;
  const mctx = matte.getContext("2d");
  const out = mctx.createImageData(size, size);
  for (let i = 0; i < size * size; i += 1) {
    out.data[i * 4] = Math.round(headMask[i] * 255);
    out.data[i * 4 + 1] = Math.round(parts.skin[i] * 255);
    out.data[i * 4 + 2] = Math.round(parts.hair[i] * 255);
    out.data[i * 4 + 3] = 255;
  }
  mctx.putImageData(out, 0, 0);
  const frame = faceFrame(placed);
  return {
    id: "face-stream",
    custom: true,
    points: placed.flatMap(([x, y]) => [Math.round(x * 10) / 10, Math.round(y * 10) / 10]),
    head: headRing(headMask, size, frame.mid, frame.span),
    photo,
    matte,
    matrix,
  };
}

export function faceStreamHead(built) {
  const points = pointsFromFlat(built.points);
  const ring = pointsFromFlat(built.head);
  const model = headModel(points, ring);
  return { layout: headLayout(points, ring), model, mesh: buildHeadMesh(model, faceTemplate()) };
}

export async function prepareFaceStreamReference(reference, { documentRef = globalThis.document, ImageImpl = globalThis.Image, image = null } = {}) {
  const picture = image || await decodeImage(reference.image, reference.format, ImageImpl);
  const { width: w, height: h } = reference;
  const canvas = documentRef.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(picture, 0, 0, w, h);
  const pixels = ctx.getImageData(0, 0, w, h).data;
  const alpha = faceStreamMatte(reference, pixels);
  let backdrop;
  let built;
  let place = null;
  if (alpha) {
    const layers = faceStreamLayers(reference, alpha);
    const plate = faceStreamPlate(pixels, w, h, faceStreamGrow(layers.removal, w, h));
    const unmixed = faceStreamUnmix(pixels, plate, alpha);
    backdrop = { data: plate, width: w, height: h };
    const pad = Math.round(FACE_STREAM_PAD * w);
    const pw = w + 2 * pad;
    const ph = h + 2 * pad;
    const shifted = reference.points.map(([x, y]) => [x + pad, y + pad]);
    built = buildFaceStreamFace({
      image: pixelCanvas(documentRef, padPixels(unmixed, w, h, pad), pw, ph),
      width: pw,
      height: ph,
      points: shifted,
      alpha: padFloats(alpha, w, h, pad),
      head: padFloats(layers.head, w, h, pad),
      documentRef,
    });
    place = { points: reference.points, pose: faceStreamPose(reference.points), matrix: built.matrix, pad, width: w, height: h };
  } else {
    backdrop = { data: faceStreamBackdrop(pixels, w, h, new Float32Array(w * h)), width: w, height: h };
    built = buildCustomFace({ image: canvas, width: w, height: h, points: reference.points, mask: null, documentRef });
    place = { points: reference.points, pose: faceStreamPose(reference.points), matrix: customFaceTransform(reference.points).matrix, pad: 0, width: w, height: h };
  }
  const head = alpha ? faceStreamHead(built) : faceHead(built);
  return {
    id: reference.id,
    built,
    head,
    backdrop,
    place,
    reference,
    pixels,
    alpha,
    colours: faceStreamMouthColours(pixels, w, h, reference.points),
  };
}

export async function prepareFaceStreamAtlas(atlas, { documentRef = globalThis.document, ImageImpl = globalThis.Image, image = null } = {}) {
  const picture = image || await decodeImage(atlas.image, atlas.format, ImageImpl);
  const w = atlas.tileW * atlas.tiles.length;
  const h = atlas.tileH;
  const canvas = documentRef.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(picture, 0, 0, w, h);
  return { atlas, image: canvas, pixels: ctx.getImageData(0, 0, w, h) };
}

export function createFaceStreamView({ documentRef = globalThis.document, width = 320, height = 400, ImageImpl = globalThis.Image } = {}) {
  const element = documentRef.createElement("div");
  element.className = "fm-face-stream";
  element.setAttribute?.("style", "position:relative;display:inline-block;line-height:0");
  const canvas = documentRef.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  canvas.setAttribute?.("aria-label", `${FACE_STREAM_LABEL}. ${FACE_STREAM_DETAIL}`);
  const badge = documentRef.createElement("span");
  badge.className = "fm-face-stream-badge";
  badge.textContent = FACE_STREAM_LABEL;
  badge.title = FACE_STREAM_DETAIL;
  badge.setAttribute?.("role", "note");
  badge.setAttribute?.("style", "position:absolute;left:8px;bottom:8px;padding:2px 8px;border-radius:999px;font:500 12px/18px system-ui,sans-serif;color:#fff;background:rgba(0,0,0,0.55)");
  element.append?.(canvas, badge);
  const renderer = createFaceStreamRenderer(canvas);
  const receiver = createFaceStreamReceiver();
  let prepared = null;
  let mouths = null;
  let loading = 0;
  let atlasLoading = 0;
  let lastHead = null;

  function applyAtlas() {
    if (!mouths) {
      renderer.setAtlas(null);
      return;
    }
    const gains = prepared ? faceStreamAtlasGains(mouths.pixels, mouths.atlas, prepared.pixels, prepared.reference.width, prepared.reference.height, prepared.reference.points) : null;
    renderer.setAtlas(mouths, gains);
  }

  function draw(override = null) {
    const frame = override || receiver.frame;
    const live = prepared ? faceStreamLivePoints(frame, prepared.reference, width, height) : null;
    const mouth = live && mouths ? faceStreamMouthBlend(mouths.atlas.tiles, frame.expression, live) : null;
    lastHead = renderer.draw(width, height, live, frame?.expression || null, { mouth });
    const stale = Boolean(frame?.stale);
    badge.textContent = stale ? FACE_STREAM_STALE_DETAIL : FACE_STREAM_LABEL;
    return Boolean(live);
  }

  async function useReference(reference, image = null) {
    const token = ++loading;
    const next = await prepareFaceStreamReference(reference, { documentRef, ImageImpl, image });
    if (token !== loading) return false;
    prepared = next;
    renderer.setReference(next);
    applyAtlas();
    draw();
    return true;
  }

  async function useAtlas(atlas, image = null) {
    const token = ++atlasLoading;
    const next = await prepareFaceStreamAtlas(atlas, { documentRef, ImageImpl, image });
    if (token !== atlasLoading) return false;
    mouths = next;
    applyAtlas();
    return true;
  }

  return {
    element,
    canvas,
    badge,
    receiver,
    renderer,
    async receive(packet) {
      const result = receiver.receive(packet);
      if (result.type === "reference") await useReference(result.reference).catch(() => false);
      else if (result.type === "atlas") await useAtlas(result.atlas).catch(() => false);
      else if (result.type === "frame") draw();
      return result;
    },
    useReference,
    useAtlas,
    draw,
    get prepared() {
      return prepared;
    },
    get lastHead() {
      return lastHead;
    },
    get ready() {
      return Boolean(prepared && receiver.frame);
    },
    dispose() {
      renderer.dispose();
      element.remove?.();
    },
  };
}
