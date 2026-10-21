import { clampValue, faceFrame, liveHead, mouthGap, solvePose, vertexDepths } from "./chat-call-face-geometry.js";
import { closeLids } from "./chat-call-face-expression.js";
import { buildCustomFace } from "./chat-call-face-custom.js";
import { faceHead } from "./chat-call-faces.js";
import { FACE_WARP, VERTEX_FLOATS, faceWebGL, lookUniform, photoEyes, warpVertices } from "./chat-call-face-render.js";

export const FACE_STREAM_VERSION = 1;
export const FACE_STREAM_KIND = Object.freeze({ delta: 0, key: 1, reference: 2 });
export const FACE_STREAM_FORMAT = Object.freeze({ jpeg: 0, heic: 1, webp: 2 });
export const FACE_STREAM_DEFAULTS = Object.freeze({ fps: 30, precision: 96, keyInterval: 30 });
export const FACE_STREAM_LIMITS = Object.freeze({
  maxFramePacket: 1024,
  maxReferencePacket: 256 * 1024,
  maxSide: 4096,
  maxReferenceSide: 1024,
  maxMaskSide: 128,
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
  minIntervalMs: 2000,
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
  mask: Object.freeze([64, 80]),
  cropHeight: 4.6,
  cropDrop: 0.35,
  restRise: 0.002,
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
const QUAT_STEPS = 512;
const TRANS_STEPS = 2;
const SCALE_STEPS = 8;
const MAX_TIME_JUMP = 4000;
const KEY_HEADER = 12;
const POINT_STEPS = 16;
const REFERENCE_FIXED = 1 + 4 + 12 + COUNT * 4 + 3 + 2;
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
    let end = this.out.length;
    while (end > 0 && this.out[end - 1] === 0) end -= 1;
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
    return this.pos > this.data.length + 8;
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
    if (kind !== FACE_STREAM_KIND.key && kind !== FACE_STREAM_KIND.delta) fail(kind === FACE_STREAM_KIND.reference ? "not-a-frame" : "kind");
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

export function createFaceStreamSender({ width, height, fps, precision, keyInterval, policy = FACE_STREAM_REFERENCE, documentRef = globalThis.document } = {}) {
  const encoder = createFaceStreamEncoder({ width, height, fps, precision, keyInterval });
  let last = null;
  let nextId = 0;
  let canvas = null;
  let restMouth = null;

  return {
    encoder,
    frame({ time, points, pose = null, reading = null, expression = null }) {
      const solved = pose || faceStreamPose(points);
      return encoder.encode({ time, points, pose: solved, expression: expression || faceStreamExpression(reading, points) });
    },
    score({ points, pose = null, reading = null, expression = null, light = null }) {
      const solved = pose || faceStreamPose(points);
      const measured = expression || faceStreamExpression(reading, points);
      restMouth = restMouth === null ? measured.mouth : Math.min(measured.mouth, restMouth + policy.restRise);
      const relative = { ...measured, mouth: Math.max(0, measured.mouth - restMouth) };
      return referenceScore({ pose: solved, expression: relative, light, span: faceFrame(points).span }, policy);
    },
    due(time, score) {
      return referenceDue(last, time, score, policy);
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
      return packet;
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
  let errors = 0;
  let goodMouth = NaN;
  return {
    receive(packet) {
      try {
        if (faceStreamPacketKind(packet) === FACE_STREAM_KIND.reference) {
          reference = decodeFaceStreamReference(packet);
          return { type: "reference", reference };
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

export function faceStreamBackdrop(pixels, w, h, removal, blur = 2) {
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
        for (let c = 0; c < 3; c += 1) {
          const mine = a > 1e-4 ? data[at + c] / a : 0.5;
          let under = mine;
          if (coarse) {
            const px = Math.min(coarse.w - 1, x >> 1);
            const py = Math.min(coarse.h - 1, y >> 1);
            under = coarse.data[(py * coarse.w + px) * 4 + c];
          }
          filled[at + c] = under + (mine - under) * trust;
        }
        filled[at + 3] = 1;
      }
    }
    coarse = { data: filled, w: cw, h: ch };
  }
  const soft = blur > 0 ? boxBlurRGBA(boxBlurRGBA(coarse.data, w, h, blur), w, h, blur) : coarse.data;
  const out = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i += 1) {
    for (let c = 0; c < 3; c += 1) out[i * 4 + c] = Math.round(soft[i * 4 + c] * 255);
    out[i * 4 + 3] = 255;
  }
  return out;
}

const STREAM_QUAD = `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const STREAM_COMPOSITE = `
precision highp float;
uniform sampler2D uBackdrop;
uniform sampler2D uWarp;
uniform sampler2D uAux;
uniform vec2 uPixels;
uniform vec2 uMouthCenter;
uniform vec2 uMouthX;
uniform vec2 uMouthY;
uniform float uTongue;
uniform float uFace;
uniform float uSeed;
varying vec2 vUv;
float grain(vec2 at) {
  return fract(sin(dot(floor(at) + vec2(uSeed, uSeed * 0.61), vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
}
void main() {
  vec3 color = texture2D(uBackdrop, vUv).rgb;
  if (uFace > 0.5) {
    vec4 warp = texture2D(uWarp, vUv);
    vec4 aux = texture2D(uAux, vUv);
    float hole = min(aux.b * 2.0, 1.0);
    float cavity = max(aux.b * 2.0 - 1.0, 0.0);
    vec2 d = vUv * uPixels - uMouthCenter;
    vec2 q = vec2(dot(d, uMouthX), dot(d, uMouthY));
    vec3 inside = mix(vec3(0.32, 0.12, 0.12), vec3(0.09, 0.03, 0.04), smoothstep(0.0, 1.0, cavity));
    float teeth = (1.0 - smoothstep(-0.5, -0.15, q.y)) * (1.0 - smoothstep(0.45, 0.85, abs(q.x)));
    inside = mix(inside, vec3(0.86, 0.84, 0.79), teeth * 0.85);
    float lower = smoothstep(0.55, 0.9, q.y) * (1.0 - smoothstep(0.3, 0.65, abs(q.x)));
    inside = mix(inside, vec3(0.76, 0.74, 0.69), lower * 0.3);
    float tongue = uTongue * (1.0 - smoothstep(0.55, 1.0, length((q - vec2(0.0, 0.55)) / vec2(0.75, 0.6))));
    inside = mix(inside, vec3(0.78, 0.36, 0.4), tongue);
    color = mix(color, inside, hole);
    color *= 1.0 - 0.22 * hole * (1.0 - hole) * 4.0;
    vec3 face = clamp(warp.rgb + grain(vUv * uPixels) * 0.008, 0.0, 1.0);
    color = mix(color, face, warp.a);
  }
  gl_FragColor = vec4(color, 1.0);
}`;

export function createFaceStreamRenderer(canvas) {
  const gl = faceWebGL(canvas);
  if (!gl) throw new Error("WebGL is unavailable");
  const { compile, texture, target } = FACE_WARP;
  const warp = compile(gl, FACE_WARP.vertex, FACE_WARP.fragment);
  const composite = compile(gl, STREAM_QUAD, STREAM_COMPOSITE);
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const meshBuffer = gl.createBuffer();
  const photo = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  const aux = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  const backdrop = texture(gl);
  gl.bindTexture(gl.TEXTURE_2D, backdrop);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([44, 46, 52, 255]));
  let face = null;
  let targets = null;
  let width = 0;
  let height = 0;
  let count = 0;

  function resize(w, h) {
    if (targets && w === width && h === height) return;
    width = w;
    height = h;
    canvas.width = w;
    canvas.height = h;
    targets = { warp: target(gl, w, h, gl.UNSIGNED_BYTE, true), aux: target(gl, w, h, gl.UNSIGNED_BYTE, true) };
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

  return {
    gl,
    setFace({ photo: image, matte, mesh, layout, model, size }) {
      upload(photo, image, true);
      upload(aux, matte, true);
      face = { mesh, layout, model, size, eyes: photoEyes(layout, size) };
    },
    clearFace() {
      face = null;
    },
    setBackdrop(image) {
      upload(backdrop, image, false);
    },
    draw(w, h, live, expression = null) {
      if (gl.isContextLost()) return null;
      resize(w, h);
      gl.disable(gl.BLEND);
      let head = null;
      const showFace = Boolean(face && live);
      if (showFace) {
        const close = [expression?.lidLeft || 0, expression?.lidRight || 0];
        const drawn = closeLids(live, close);
        head = liveHead(face.model, drawn);
        const vertices = warpVertices(face.mesh, face.layout, head.points, face.size, vertexDepths(face.model, head.pose));
        gl.useProgram(warp.program);
        gl.bindBuffer(gl.ARRAY_BUFFER, meshBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
        const attrs = [["aPos", 2, 0], ["aUv", 2, 8], ["aHole", 1, 16], ["aRegion", 1, 20], ["aDepth", 1, 24]].map(([name, size, offset]) => {
          const at = gl.getAttribLocation(warp.program, name);
          if (at >= 0) {
            gl.enableVertexAttribArray(at);
            gl.vertexAttribPointer(at, size, gl.FLOAT, false, VERTEX_FLOATS * 4, offset);
          }
          return at;
        }).filter((at) => at >= 0);
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
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, w, h);
      gl.useProgram(composite.program);
      bind(composite, [["uBackdrop", backdrop], ["uWarp", targets.warp.texture], ["uAux", targets.aux.texture]]);
      const mouth = head ? faceStreamMouthFrame(head.points) : { center: [-1e5, -1e5], x: [0, 0], y: [0, 0] };
      const u = composite.uniforms;
      gl.uniform2f(u.uPixels, w, h);
      gl.uniform2f(u.uMouthCenter, mouth.center[0], mouth.center[1]);
      gl.uniform2f(u.uMouthX, mouth.x[0], mouth.x[1]);
      gl.uniform2f(u.uMouthY, mouth.y[0], mouth.y[1]);
      gl.uniform1f(u.uTongue, showFace ? clampValue(expression?.tongue || 0, 0, 1) : 0);
      gl.uniform1f(u.uFace, showFace ? 1 : 0);
      count = (count + 1) % 4096;
      gl.uniform1f(u.uSeed, (count * 7.31) % 97);
      gl.bindBuffer(gl.ARRAY_BUFFER, quad);
      const at = gl.getAttribLocation(composite.program, "aPos");
      gl.enableVertexAttribArray(at);
      gl.vertexAttribPointer(at, 2, gl.FLOAT, false, 0, 0);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      gl.disableVertexAttribArray(at);
      return head;
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

export async function prepareFaceStreamReference(reference, { documentRef = globalThis.document, ImageImpl = globalThis.Image, image = null } = {}) {
  const picture = image || await decodeImage(reference.image, reference.format, ImageImpl);
  const { width: w, height: h } = reference;
  const canvas = documentRef.createElement("canvas");
  canvas.width = w;
  canvas.height = h;
  const ctx = canvas.getContext("2d", { willReadFrequently: true });
  ctx.drawImage(picture, 0, 0, w, h);
  const pixels = ctx.getImageData(0, 0, w, h).data;
  let mask = null;
  let maskAt = null;
  if (reference.mask) {
    const bytes = new Uint8Array(reference.mask.length);
    for (let i = 0; i < bytes.length; i += 1) bytes[i] = reference.mask[i] ? 255 : 0;
    mask = { bytes, width: reference.maskW, height: reference.maskH };
    maskAt = (x, y) => {
      const mx = clampValue(Math.floor(((x + 0.5) / w) * reference.maskW), 0, reference.maskW - 1);
      const my = clampValue(Math.floor(((y + 0.5) / h) * reference.maskH), 0, reference.maskH - 1);
      return reference.mask[my * reference.maskW + mx];
    };
  }
  const removal = faceStreamRemoval(reference, maskAt);
  const soft = new Float32Array(removal.length);
  if (maskAt) {
    const plane = new Float32Array(removal.length * 4);
    for (let i = 0; i < removal.length; i += 1) plane[i * 4] = removal[i];
    const spread = boxBlurRGBA(plane, w, h, 3);
    for (let i = 0; i < removal.length; i += 1) soft[i] = Math.max(removal[i], Math.min(1, spread[i * 4] * 1.6));
  }
  const backdrop = { data: faceStreamBackdrop(pixels, w, h, soft), width: w, height: h };
  const built = buildCustomFace({ image: canvas, width: w, height: h, points: reference.points, mask, documentRef });
  const head = faceHead(built);
  return { id: reference.id, built, head, backdrop, reference };
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
  let loading = 0;
  let lastHead = null;

  function draw() {
    const live = prepared ? faceStreamLivePoints(receiver.frame, prepared.reference, width, height) : null;
    lastHead = renderer.draw(width, height, live, receiver.frame?.expression || null);
    const stale = Boolean(receiver.frame?.stale);
    badge.textContent = stale ? FACE_STREAM_STALE_DETAIL : FACE_STREAM_LABEL;
    return Boolean(live);
  }

  async function useReference(reference, image = null) {
    const token = ++loading;
    const next = await prepareFaceStreamReference(reference, { documentRef, ImageImpl, image });
    if (token !== loading) return false;
    prepared = next;
    renderer.setBackdrop(next.backdrop);
    renderer.setFace({ photo: next.built.photo, matte: next.built.matte, mesh: next.head.mesh, layout: next.head.layout, model: next.head.model, size: 512 });
    draw();
    return true;
  }

  return {
    element,
    canvas,
    badge,
    receiver,
    async receive(packet) {
      const result = receiver.receive(packet);
      if (result.type === "reference") await useReference(result.reference).catch(() => false);
      else if (result.type === "frame") draw();
      return result;
    },
    useReference,
    draw,
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
