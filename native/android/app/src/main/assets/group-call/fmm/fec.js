import {
  FmmError, Repair, MediaHeaderLen, RecoveryOffset, RepairHeaderLen, MaxDatagramLen, MaxFECK, MaxFECM,
  MaxRepairSpan, SchemeXOR, SchemeRS, FlagKeyframe, Version, KindMedia, setFECFlag, seqOf, setSeq, parseMedia,
} from "./wire.js";

const expTable = new Uint8Array(512);
const logTable = new Uint8Array(256);
const mulTable = new Uint8Array(65536);

{
  let x = 1;
  for (let i = 0; i < 255; i++) {
    expTable[i] = x;
    logTable[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  for (let i = 255; i < 512; i++) expTable[i] = expTable[i - 255];
  for (let a = 1; a < 256; a++) {
    for (let b = 1; b < 256; b++) mulTable[(a << 8) | b] = expTable[logTable[a] + logTable[b]];
  }
}

function gfMul(a, b) {
  return mulTable[(a << 8) | b];
}

function gfInv(a) {
  return expTable[255 - logTable[a]];
}

function mulAdd(dst, src, c) {
  if (c === 0) return;
  const n = src.length;
  if (c === 1) {
    for (let i = 0; i < n; i++) dst[i] ^= src[i];
    return;
  }
  const t = c << 8;
  for (let i = 0; i < n; i++) dst[i] ^= mulTable[t | src[i]];
}

export const ErrTooFew = new FmmError("fec: not enough symbols to recover");
export const ErrGeometry = new FmmError("fec: bad code geometry");

const codeCache = new Map();

function coefficients(k, m) {
  const key = (k << 8) | m;
  const cached = codeCache.get(key);
  if (cached) return cached;
  const c = new Array(m);
  for (let j = 0; j < m; j++) {
    c[j] = new Uint8Array(k);
    for (let i = 0; i < k; i++) c[j][i] = gfInv((j & 0xff) ^ ((m + i) & 0xff));
  }
  for (let i = 0; i < k; i++) {
    const s = gfInv(c[0][i]);
    for (let j = 0; j < m; j++) c[j][i] = gfMul(c[j][i], s);
  }
  codeCache.set(key, c);
  return c;
}

export function encode(blocks, m) {
  const k = blocks.length;
  const size = blocks[0].length;
  const c = coefficients(k, m);
  const out = new Array(m);
  for (let j = 0; j < m; j++) {
    out[j] = new Uint8Array(size);
    for (let i = 0; i < k; i++) mulAdd(out[j], blocks[i], c[j][i]);
  }
  return out;
}

function invert(a) {
  const n = a.length;
  const w = new Array(n);
  for (let i = 0; i < n; i++) {
    w[i] = new Uint8Array(2 * n);
    w[i].set(a[i]);
    w[i][n + i] = 1;
  }
  for (let col = 0; col < n; col++) {
    let piv = -1;
    for (let r = col; r < n; r++) {
      if (w[r][col] !== 0) {
        piv = r;
        break;
      }
    }
    if (piv < 0) return null;
    const tmp = w[col];
    w[col] = w[piv];
    w[piv] = tmp;
    const s = gfInv(w[col][col]);
    for (let x = 0; x < w[col].length; x++) w[col][x] = gfMul(w[col][x], s);
    for (let r = 0; r < n; r++) {
      if (r !== col && w[r][col] !== 0) mulAdd(w[r], w[col], w[r][col]);
    }
  }
  const out = new Array(n);
  for (let i = 0; i < n; i++) out[i] = w[i].subarray(n);
  return out;
}

export function reconstruct(data, repair) {
  const k = data.length;
  const m = repair.length;
  if (k === 0 || m === 0 || k + m > 256) throw ErrGeometry;
  const missing = [];
  const rows = [];
  let size = -1;
  for (let i = 0; i < k; i++) {
    const d = data[i];
    if (d == null) missing.push(i);
    else if (size < 0) size = d.length;
  }
  if (missing.length === 0) return;
  for (let j = 0; j < m; j++) {
    const r = repair[j];
    if (r != null && rows.length < missing.length) {
      rows.push(j);
      if (size < 0) size = r.length;
    }
  }
  if (rows.length < missing.length) throw ErrTooFew;
  for (const d of data) {
    if (d != null && d.length !== size) throw ErrGeometry;
  }
  for (const j of rows) {
    if (repair[j].length !== size) throw ErrGeometry;
  }
  const c = coefficients(k, m);
  const e = missing.length;
  const syn = new Array(e);
  for (let q = 0; q < e; q++) {
    const j = rows[q];
    syn[q] = repair[j].slice();
    for (let i = 0; i < k; i++) {
      if (data[i] != null) mulAdd(syn[q], data[i], c[j][i]);
    }
  }
  const a = new Array(e);
  for (let q = 0; q < e; q++) {
    a[q] = new Uint8Array(e);
    for (let p = 0; p < e; p++) a[q][p] = c[rows[q]][missing[p]];
  }
  const inv = invert(a);
  if (!inv) throw ErrGeometry;
  for (let p = 0; p < e; p++) {
    const d = new Uint8Array(size);
    for (let q = 0; q < e; q++) mulAdd(d, syn[q], inv[p][q]);
    data[missing[p]] = d;
  }
}

const audioKs = [1, 2, 3];
const videoKs = [2, 3, 4, 6, 8, 12, 16, 24, 32, 48];
const audioOff = 0.01;
const videoOff = 0.005;
const audioTarget = 0.005;
const audioOverhead = 2.0;
const videoTarget = 0.005;
const videoOverhead = 0.5;
const burstThreshold = 1.5;
const importantTarget = 0.001;
const importantOverhead = 1.0;
const otherTarget = 0.01;
const otherOverhead = 0.5;

const powView = new DataView(new ArrayBuffer(8));

function frexp(x) {
  let e = 0;
  if (Math.abs(x) < 2.2250738585072014e-308) {
    x *= 4503599627370496;
    e = -52;
  }
  powView.setFloat64(0, x);
  const hi = powView.getUint32(0);
  e += ((hi >>> 20) & 0x7ff) - 1022;
  powView.setUint32(0, (hi & 0x800fffff) | (1022 << 20));
  return [powView.getFloat64(0), e];
}

function ldexp(frac, exp) {
  if (frac === 0 || !Number.isFinite(frac)) return frac;
  if (exp > 1023) return frac * 2 ** 1023 * 2 ** (exp - 1023);
  if (exp < -1022) return frac * 2 ** (exp + 53) * 2 ** -53;
  return frac * 2 ** exp;
}

function isOddInt(y) {
  return Number.isInteger(y) && Math.abs(y) < 2 ** 53 && Math.abs(y % 2) === 1;
}

function goPow(x, y) {
  if (y === 0 || x === 1) return 1;
  if (y === 1) return x;
  if (Number.isNaN(x) || Number.isNaN(y)) return NaN;
  if (x === 0) {
    if (y < 0) return Object.is(x, -0) && isOddInt(y) ? -Infinity : Infinity;
    if (y > 0) return Object.is(x, -0) && isOddInt(y) ? x : 0;
  }
  if (!Number.isFinite(y)) {
    if (x === -1) return 1;
    return (Math.abs(x) < 1) === (y > 0) ? 0 : Infinity;
  }
  if (!Number.isFinite(x)) {
    if (x < 0) return goPow(1 / x, -y);
    return y < 0 ? 0 : Infinity;
  }
  if (y === 0.5) return Math.sqrt(x);
  if (y === -0.5) return 1 / Math.sqrt(x);
  let yi = Math.trunc(Math.abs(y));
  let yf = Math.abs(y) - yi;
  if (yf !== 0 && x < 0) return NaN;
  if (yi >= 2 ** 63) {
    if (x === -1) return 1;
    return (Math.abs(x) < 1) === (y > 0) ? 0 : Infinity;
  }
  let a1 = 1.0;
  let ae = 0;
  if (yf !== 0) {
    if (yf > 0.5) {
      yf--;
      yi++;
    }
    a1 = Math.exp(yf * Math.log(x));
  }
  let [x1, xe] = frexp(x);
  for (let i = yi; i !== 0; i = Math.floor(i / 2)) {
    if (xe < -(1 << 12) || (1 << 12) < xe) {
      ae += xe;
      break;
    }
    if (i % 2 === 1) {
      a1 *= x1;
      ae += xe;
    }
    x1 *= x1;
    xe *= 2;
    if (x1 < 0.5) {
      x1 += x1;
      xe--;
    }
  }
  if (y < 0) {
    a1 = 1 / a1;
    ae = -ae;
  }
  return ldexp(a1, ae);
}

export class Params {
  constructor(init = {}) {
    this.k = init.k ?? 0;
    this.m = init.m ?? 0;
  }

  on() {
    return this.k > 0 && this.m > 0;
  }

  ratio() {
    if (!this.on()) return 0;
    return this.m / this.k;
  }
}

export function residual(k, m, p, burst) {
  const n = k + m;
  const b = Math.max(burst, 1);
  const pe = p / b;
  if (pe <= 0) return 0;
  if (pe >= 1) return 1;
  const q = 1 - pe;
  let term = goPow(q, n);
  let sum = 0.0;
  for (let j = 1; j <= n; j++) {
    term *= (n - j + 1) / j * pe / q;
    if (j * b > m) sum += term * Math.min(1, j * b / n);
  }
  return sum;
}

export function choose(audio, loss, burst, maxK) {
  if (audio) {
    if (loss < audioOff) return new Params();
    return chooseFrom(audioKs, loss, burst, maxK, audioTarget, audioOverhead);
  }
  if (loss < videoOff) return new Params();
  return chooseFrom(videoKs, loss, burst, maxK, videoTarget, videoOverhead);
}

export function chooseVideo(loss, burst, maxKImportant, maxKOther) {
  if (loss < videoOff) return [new Params(), new Params()];
  return [
    chooseFrom(videoKs, loss, burst, maxKImportant, importantTarget, importantOverhead),
    chooseFrom(videoKs, loss, burst, maxKOther, otherTarget, otherOverhead),
  ];
}

function chooseFrom(ks, loss, burst, maxK, target, capRatio) {
  const p = Math.min(loss, 0.5);
  maxK = Math.max(maxK, ks[0]);
  if (burst < burstThreshold) burst = 1;
  let best = new Params();
  let bestRes = Infinity;
  let bestRatio = Infinity;
  let fallback = new Params();
  let fallbackRes = Infinity;
  for (const k of ks) {
    if (k > maxK) break;
    const limit = Math.min(16, Math.trunc(Math.floor(capRatio * k + 1e-9)));
    for (let m = 1; m <= limit; m++) {
      const res = residual(k, m, p, burst);
      const ratio = m / k;
      if (res <= target && (ratio < bestRatio || (ratio === bestRatio && res < bestRes))) {
        best = new Params({ k, m });
        bestRes = res;
        bestRatio = ratio;
      }
      if (res < fallbackRes) {
        fallback = new Params({ k, m });
        fallbackRes = res;
      }
    }
  }
  if (best.on()) return best;
  return fallback;
}

function int16(v) {
  return (v << 16) >> 16;
}

function clampParams(p) {
  return new Params({ k: Math.min(Math.max(p.k, 0), MaxFECK), m: Math.min(Math.max(p.m, 0), MaxFECM) });
}

function newOpenGroup() {
  return { members: [], started: 0, maxBlock: 0, low: 0, high: 0 };
}

function importance(pkt) {
  return (pkt[10] & FlagKeyframe) !== 0 || (pkt[10] & 0x07) === 0;
}

function recoveryBlock(pkt) {
  const body = pkt.subarray(RecoveryOffset);
  const b = new Uint8Array(2 + body.length);
  b[0] = (body.length >> 8) & 0xff;
  b[1] = body.length & 0xff;
  b.set(body, 2);
  return b;
}

export class Encoder {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this._params = init.params ?? new Params();
    this.other = init.other ?? new Params();
    this.unequal = init.unequal ?? false;
    this.maxDelay = init.maxDelay ?? 0;
    this.maxDatagram = init.maxDatagram ?? 0;
    this.open = [newOpenGroup(), newOpenGroup()];
    this.groups = init.groups ?? 0;
    this.repairs = init.repairs ?? 0;
  }

  setUnequal(on) {
    this.unequal = on;
  }

  params() {
    return new Params({ k: this._params.k, m: this._params.m });
  }

  ratio(importantShare) {
    if (!this.unequal) return this._params.ratio();
    return importantShare * this._params.ratio() + (1 - importantShare) * this.other.ratio();
  }

  setParams(p) {
    this._params = clampParams(p);
    this.other = new Params({ k: this._params.k, m: this._params.m });
  }

  setClassParams(important, other) {
    this._params = clampParams(important);
    this.other = clampParams(other);
  }

  scale(f) {
    const scale = (p) => {
      if (!p.on()) return p;
      const out = new Params({ k: p.k, m: Math.trunc(p.m * f) });
      if (out.m === 0) return new Params();
      return out;
    };
    this._params = scale(this._params);
    this.other = scale(this.other);
  }

  classParams(which) {
    return which === 1 ? this.other : this._params;
  }

  add(pkt, now) {
    if (pkt.length < MediaHeaderLen) return [];
    let block = recoveryBlock(pkt);
    let which = 0;
    if (this.unequal && !importance(pkt)) which = 1;
    const params = this.classParams(which);
    if (!params.on() || RepairHeaderLen + 1 + block.length > this.maxDatagram) {
      setFECFlag(pkt, false);
      if (!params.on()) return this.emit(which);
      return [];
    }
    const g = this.open[which];
    const seq = seqOf(pkt);
    let out = [];
    if (g.members.length > 0) {
      let lo = g.low;
      let hi = g.high;
      if (int16(seq - lo) < 0) lo = seq;
      if (int16(seq - hi) > 0) hi = seq;
      let dup = false;
      for (const m of g.members) {
        if (m.seq === seq) dup = true;
      }
      if (dup || ((hi - lo) & 0xffff) > MaxRepairSpan
        || RepairHeaderLen + g.members.length + 1 + Math.max(g.maxBlock, block.length) > this.maxDatagram) {
        out = this.emit(which);
      }
    }
    setFECFlag(pkt, true);
    block = recoveryBlock(pkt);
    const h = this.open[which];
    if (h.members.length === 0) {
      h.started = now;
      h.low = seq;
      h.high = seq;
    }
    if (int16(seq - h.low) < 0) h.low = seq;
    if (int16(seq - h.high) > 0) h.high = seq;
    h.members.push({ seq, block });
    h.maxBlock = Math.max(h.maxBlock, block.length);
    if (h.members.length >= params.k) out = out.concat(this.emit(which));
    return out;
  }

  flush(now) {
    let out = [];
    for (let i = 0; i < this.open.length; i++) {
      if (this.open[i].members.length > 0 && now - this.open[i].started >= this.maxDelay) {
        out = out.concat(this.emit(i));
      }
    }
    return out;
  }

  emit(which) {
    const g = this.open[which];
    const k = g.members.length;
    if (k === 0) return [];
    let m = 1;
    const p = this.classParams(which);
    if (p.on()) m = Math.max(1, Math.trunc((p.m * k + p.k - 1) / p.k));
    m = Math.min(m, MaxFECM);
    g.members.sort((a, b) => int16(a.seq - b.seq));
    const base = g.members[0].seq;
    const offsets = new Uint8Array(k);
    const blocks = new Array(k);
    for (let i = 0; i < k; i++) {
      const mb = g.members[i];
      offsets[i] = (mb.seq - base) & 0xff;
      let b = mb.block;
      if (b.length < g.maxBlock) {
        const padded = new Uint8Array(g.maxBlock);
        padded.set(b);
        b = padded;
      }
      blocks[i] = b;
    }
    const syms = encode(blocks, m);
    const scheme = m === 1 ? SchemeXOR : SchemeRS;
    const out = new Array(m);
    for (let j = 0; j < syms.length; j++) {
      const r = new Repair({ stream: this.stream, baseSeq: base, scheme, k, m, index: j, offsets, symbol: syms[j] });
      out[j] = r.append(new Uint8Array(0));
    }
    this.open[which] = newOpenGroup();
    this.groups++;
    this.repairs += m;
    return out;
  }
}

export function newEncoder(stream, maxDatagram, maxDelayUs) {
  return new Encoder({ stream, maxDatagram: Math.min(maxDatagram, MaxDatagramLen), maxDelay: maxDelayUs });
}

const slotCount = 1024;
const maxGroups = 64;
const groupLife = 1_000_000;

function emptySlot() {
  return { seq: 0, valid: false, block: null };
}

function sameOffsets(g, r) {
  for (let i = 0; i < r.offsets.length; i++) {
    if (g.members[i] !== ((r.baseSeq + r.offsets[i]) & 0xffff)) return false;
  }
  return true;
}

function rebuild(stream, seq, block) {
  if (block.length < 2) return null;
  const n = (block[0] << 8) | block[1];
  if (n < MediaHeaderLen - RecoveryOffset || 2 + n > block.length) return null;
  const pkt = new Uint8Array(RecoveryOffset + n);
  pkt[0] = (Version << 4) | KindMedia;
  pkt[1] = stream;
  setSeq(pkt, seq);
  pkt.set(block.subarray(2, 2 + n), RecoveryOffset);
  try {
    parseMedia(pkt);
  } catch (_) {
    return null;
  }
  return pkt;
}

export class Decoder {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.slots = new Array(slotCount);
    for (let i = 0; i < slotCount; i++) this.slots[i] = emptySlot();
    this.groups = init.groups ?? [];
    this.recovered = init.recovered ?? 0;
    this.failed = init.failed ?? 0;
  }

  addMedia(pkt, protectedPkt, now) {
    const seq = seqOf(pkt);
    const i = seq % slotCount;
    const s = this.slots[i];
    if (s.valid && s.seq === seq) return [];
    if (!protectedPkt) {
      this.slots[i] = emptySlot();
      return [];
    }
    this.slots[i] = { seq, valid: true, block: recoveryBlock(pkt) };
    let out = [];
    for (const g of this.groups) {
      if (!g.done && g.members.includes(seq)) out = out.concat(this.try(g));
    }
    this.prune(now);
    return out;
  }

  addRepair(r, now) {
    let g = null;
    for (const x of this.groups) {
      if (x.base === r.baseSeq && x.k === r.k && x.m === r.m && x.scheme === r.scheme && sameOffsets(x, r)) {
        g = x;
        break;
      }
    }
    if (!g) {
      this.prune(now);
      if (this.groups.length >= maxGroups) this.groups = this.groups.slice(1);
      const members = [];
      for (const o of r.offsets) members.push((r.baseSeq + o) & 0xffff);
      g = {
        base: r.baseSeq, members, k: r.k, m: r.m, scheme: r.scheme, size: r.symbol.length,
        repairs: new Array(r.m).fill(null), created: now, done: false,
      };
      this.groups.push(g);
    }
    if (g.done || r.symbol.length !== g.size || g.repairs[r.index] != null) return [];
    g.repairs[r.index] = r.symbol.slice();
    return this.try(g);
  }

  try(g) {
    const data = new Array(g.k).fill(null);
    let missing = 0;
    for (let i = 0; i < g.k; i++) {
      const seq = g.members[i];
      const s = this.slots[seq % slotCount];
      if (s.valid && s.seq === seq) {
        if (s.block.length > g.size) {
          g.done = true;
          this.failed++;
          return [];
        }
        let b = s.block;
        if (b.length < g.size) {
          const padded = new Uint8Array(g.size);
          padded.set(b);
          b = padded;
        }
        data[i] = b;
      } else {
        missing++;
      }
    }
    if (missing === 0) {
      g.done = true;
      return [];
    }
    let reps = 0;
    for (const r of g.repairs) if (r != null) reps++;
    if (reps < missing) return [];
    try {
      reconstruct(data, g.repairs);
    } catch (_) {
      g.done = true;
      this.failed++;
      return [];
    }
    g.done = true;
    const out = [];
    for (let i = 0; i < g.k; i++) {
      const seq = g.members[i];
      const idx = seq % slotCount;
      const s = this.slots[idx];
      if (s.valid && s.seq === seq) continue;
      const pkt = rebuild(this.stream, seq, data[i]);
      if (!pkt) {
        this.failed++;
        continue;
      }
      this.slots[idx] = { seq, valid: true, block: recoveryBlock(pkt) };
      this.recovered++;
      out.push(pkt);
    }
    return out;
  }

  prune(now) {
    this.groups = this.groups.filter((g) => !g.done && now - g.created < groupLife);
  }
}

export function newDecoder(stream) {
  return new Decoder({ stream });
}
