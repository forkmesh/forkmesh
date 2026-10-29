import {
  FmmError, Media, MaxLayer, MaxFragments, FlagRecovery, FlagLongTerm, FlagFace,
} from "./wire.js";
import { Unwrapper16, Unwrapper32 } from "./seqnum.js";

export const ErrTooLarge = new FmmError("frag: frame needs more than 255 fragments");
export const ErrRef = new FmmError("frag: reference frame too far back, send a keyframe");
export const ErrLayer = new FmmError("frag: bad layer");

export class Opts {
  constructor(init = {}) {
    this.keyframe = init.keyframe ?? false;
    this.layer = init.layer ?? 0;
    this.independent = init.independent ?? false;
    this.ref = init.ref ?? 0;
    this.longTerm = init.longTerm ?? false;
    this.face = init.face ?? false;
    this.aux = init.aux ?? 0;
  }
}

export class Packetizer {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.maxPayload = init.maxPayload ?? 0;
    this.seq = init.seq ?? 0;
    this.frameID = init.frameID ?? 0;
    this.lastAtOrBelow = init.lastAtOrBelow ? init.lastAtOrBelow.slice() : new Array(MaxLayer + 1).fill(0);
    this.haveLayer = init.haveLayer ? init.haveLayer.slice() : new Array(MaxLayer + 1).fill(false);
  }

  maxFrame() {
    return this.maxPayload * MaxFragments;
  }

  nextID() {
    return this.frameID + 1;
  }

  lastID() {
    return this.frameID;
  }

  packetize(data, keyframe, layer, independent, ts) {
    return this.packetizeFrame(data, new Opts({ keyframe, layer, independent, ref: -1 }), ts);
  }

  packetizeFrame(data, o, ts) {
    const keyframe = o.keyframe;
    const layer = o.layer;
    if (layer < 0 || layer > MaxLayer || (keyframe && layer !== 0)) throw ErrLayer;
    if (o.ref >= 0 && (keyframe || o.independent || layer !== 0)) throw ErrLayer;
    const n = Math.max(1, Math.trunc((data.length + this.maxPayload - 1) / this.maxPayload));
    if (n > MaxFragments) throw ErrTooLarge;
    const id = this.frameID + 1;
    let ref = o.aux & 0xff;
    let recovery = false;
    if (keyframe) {
      ref = 0;
    } else if (o.ref >= 0) {
      if (!this.haveLayer[0]) throw ErrRef;
      const d = id - o.ref;
      if (d <= 0 || d > 255) throw ErrRef;
      ref = d;
      recovery = true;
    } else if (!o.independent) {
      const lower = layer > 0 ? layer - 1 : 0;
      if (!this.haveLayer[lower]) throw ErrRef;
      const d = id - this.lastAtOrBelow[lower];
      if (d <= 0 || d > 255) throw ErrRef;
      ref = d;
    }
    this.frameID = id;
    if (keyframe) {
      for (let l = 0; l < this.haveLayer.length; l++) {
        this.haveLayer[l] = true;
        this.lastAtOrBelow[l] = id;
      }
    } else if (!o.independent) {
      for (let l = layer; l <= MaxLayer; l++) {
        this.haveLayer[l] = true;
        this.lastAtOrBelow[l] = id;
      }
    }
    const out = new Array(n);
    let off = 0;
    for (let i = 0; i < n; i++) {
      const size = Math.trunc((data.length - off) / (n - i));
      const m = new Media({
        stream: this.stream, seq: this.seq, frameID: id & 0xffff, fragIndex: i, fragCount: n,
        keyframe, recovery, longTerm: o.longTerm, face: o.face,
        layer, refDelta: ref, timestamp: ts >>> 0, payload: data.subarray(off, off + size),
      });
      off += size;
      this.seq = (this.seq + 1) & 0xffff;
      out[i] = m.append(new Uint8Array(0));
    }
    return out;
  }
}

export function newPacketizer(stream, maxPayload) {
  return new Packetizer({ stream, maxPayload: Math.max(maxPayload, 1), frameID: -1 });
}

export class Frame {
  constructor(init = {}) {
    this.id = init.id ?? 0;
    this.keyframe = init.keyframe ?? false;
    this.recovery = init.recovery ?? false;
    this.longTerm = init.longTerm ?? false;
    this.face = init.face ?? false;
    this.layer = init.layer ?? 0;
    this.ref = init.ref ?? 0;
    this.aux = init.aux ?? 0;
    this.timestamp = init.timestamp ?? 0;
    this.firstSeq = init.firstSeq ?? 0;
    this.lastSeq = init.lastSeq ?? 0;
    this.data = init.data ?? null;
    this.firstArrival = init.firstArrival ?? 0;
    this.complete = init.complete ?? 0;
    this.recovered = init.recovered ?? 0;
  }

  setFlags(flags, aux) {
    this.recovery = (flags & FlagRecovery) !== 0;
    this.longTerm = (flags & FlagLongTerm) !== 0;
    this.face = (flags & FlagFace) !== 0;
    this.aux = aux;
  }
}

export class Lost {
  constructor(init = {}) {
    this.id = init.id ?? 0;
    this.keyframe = init.keyframe ?? false;
    this.layer = init.layer ?? 0;
  }
}

const maxPartials = 64;
const maxBytes = 4 << 20;
const doneRing = 512;
export const DefaultLimit = 1_000_000;

function copyOrNull(b) {
  return b.length === 0 ? null : b.slice();
}

function frameFlags(m) {
  let f = 0;
  if (m.recovery) f |= FlagRecovery;
  if (m.longTerm) f |= FlagLongTerm;
  if (m.face) f |= FlagFace;
  return f;
}

export class Reassembler {
  constructor(init = {}) {
    this.ids = init.ids ?? new Unwrapper16();
    this.seqs = init.seqs ?? new Unwrapper16();
    this.ts = init.ts ?? new Unwrapper16();
    this.tsw = init.tsw ?? new Unwrapper32();
    this.partial = init.partial ?? new Map();
    this.order = init.order ?? [];
    this.bytes = init.bytes ?? 0;
    this.done = new Array(doneRing).fill(0);
    this.doneOK = new Array(doneRing).fill(false);
    this.timeout = init.timeout ?? 0;
    this.completed = init.completed ?? 0;
    this.duplicates = init.duplicates ?? 0;
    this.corrupt = init.corrupt ?? 0;
    this.expired = init.expired ?? 0;
  }

  isDone(id) {
    let i = id % doneRing;
    if (i < 0) i += doneRing;
    return this.doneOK[i] && this.done[i] === id;
  }

  markDone(id) {
    let i = id % doneRing;
    if (i < 0) i += doneRing;
    this.done[i] = id;
    this.doneOK[i] = true;
  }

  add(m, recovered, now) {
    const id = this.ids.unwrap(m.frameID);
    const seq = this.seqs.unwrap(m.seq);
    const ts = this.tsw.unwrap(m.timestamp);
    if (this.isDone(id)) {
      this.duplicates++;
      return null;
    }
    let ref = -1;
    if (m.refDelta !== 0) ref = id - m.refDelta;
    const flags = frameFlags(m);
    if (m.fragCount === 1) {
      this.markDone(id);
      this.completed++;
      const f = new Frame({
        id, keyframe: m.keyframe, layer: m.layer, ref, timestamp: ts,
        firstSeq: seq, lastSeq: seq, data: copyOrNull(m.payload), firstArrival: now, complete: now,
      });
      f.setFlags(flags, m.refDelta);
      if (recovered) f.recovered = 1;
      return f;
    }
    let p = this.partial.get(id);
    if (!p) {
      this.evict(now);
      p = {
        id, count: m.fragCount, have: 0, frags: new Array(m.fragCount).fill(null), keyframe: m.keyframe,
        flags, layer: m.layer, ref, ts, firstSeq: seq - m.fragIndex, first: now, recovered: 0, bytes: 0,
      };
      this.partial.set(id, p);
      this.order.push(id);
    }
    if (p.count !== m.fragCount || p.keyframe !== m.keyframe || p.flags !== flags || p.layer !== m.layer
      || p.ref !== ref || p.firstSeq !== seq - m.fragIndex) {
      this.corrupt++;
      this.drop(id);
      return null;
    }
    if (p.frags[m.fragIndex] !== null) {
      this.duplicates++;
      return null;
    }
    p.frags[m.fragIndex] = copyOrNull(m.payload);
    p.have++;
    p.bytes += m.payload.length;
    this.bytes += m.payload.length;
    if (recovered) p.recovered++;
    if (p.have < p.count) return null;
    const data = new Uint8Array(p.bytes);
    let off = 0;
    for (const f of p.frags) {
      if (f) {
        data.set(f, off);
        off += f.length;
      }
    }
    this.drop(id);
    this.markDone(id);
    this.completed++;
    const f = new Frame({
      id, keyframe: p.keyframe, layer: p.layer, ref: p.ref, timestamp: p.ts,
      firstSeq: p.firstSeq, lastSeq: p.firstSeq + p.count - 1, data,
      firstArrival: p.first, complete: now, recovered: p.recovered,
    });
    f.setFlags(p.flags, m.refDelta);
    return f;
  }

  drop(id) {
    const p = this.partial.get(id);
    if (!p) return;
    this.bytes -= p.bytes;
    this.partial.delete(id);
    const i = this.order.indexOf(id);
    if (i >= 0) this.order.splice(i, 1);
  }

  evict(now) {
    while (this.order.length > 0 && (this.order.length >= maxPartials || this.bytes > maxBytes)) {
      this.expired++;
      this.drop(this.order[0]);
    }
  }

  expire(now) {
    const out = [];
    while (this.order.length > 0) {
      const p = this.partial.get(this.order[0]);
      if (now - p.first < this.timeout) break;
      out.push(new Lost({ id: p.id, keyframe: p.keyframe, layer: p.layer }));
      this.expired++;
      this.markDone(p.id);
      this.drop(p.id);
    }
    return out;
  }
}

export function newReassembler() {
  return new Reassembler({ timeout: DefaultLimit });
}
