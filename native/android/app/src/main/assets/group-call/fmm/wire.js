export class FmmError extends globalThis.Error {
  constructor(message) {
    super(message);
    this.name = "FmmError";
  }
}

export const Version = 1;

export const KindMedia = 0;
export const KindRepair = 1;
export const KindPadding = 2;
export const KindFeedback = 3;

export const MediaHeaderLen = 16;
export const RepairHeaderLen = 10;
export const PaddingHeaderLen = 4;
export const FeedbackHeaderLen = 4;

export const RecoveryOffset = 6;
export const RepairExpansion = RepairHeaderLen + 2 - RecoveryOffset;
export const FECReserveGroup = 16;
export const FECReserve = RepairExpansion + FECReserveGroup;
export const MaxRepairSpan = 255;

export const MaxDatagramLen = 1500;
export const MaxFragments = 255;
export const MaxLayer = 7;

export const SchemeXOR = 0;
export const SchemeRS = 1;
export const MaxFECK = 48;
export const MaxFECM = 16;

export const FlagKeyframe = 0x80;
export const FlagFEC = 0x40;
export const FlagRecovery = 0x20;
export const FlagLongTerm = 0x10;
export const FlagFace = 0x08;
const layerMask = 0x07;

export const DepthUnitUs = 10000;

export const ErrShort = new FmmError("fmm: datagram too short");
export const ErrLong = new FmmError("fmm: datagram too long");
export const ErrVersion = new FmmError("fmm: unknown version");
export const ErrKind = new FmmError("fmm: unknown datagram kind");
export const ErrFragment = new FmmError("fmm: bad fragment index or count");
export const ErrFlags = new FmmError("fmm: bad frame flags");
export const ErrRepair = new FmmError("fmm: bad repair header");
export const ErrPadding = new FmmError("fmm: bad padding header");
export const ErrFeedback = new FmmError("fmm: bad feedback datagram");

export const MsgJoin = 0x01;
export const MsgJoined = 0x02;
export const MsgPublish = 0x03;
export const MsgUnpublish = 0x04;
export const MsgTrackAdded = 0x05;
export const MsgTrackRemoved = 0x06;
export const MsgSubscribe = 0x07;
export const MsgUnsubscribe = 0x08;
export const MsgKeyframeRequest = 0x09;
export const MsgReport = 0x0a;
export const MsgBitrateHint = 0x0b;
export const MsgLeave = 0x0c;
export const MsgError = 0x0d;
export const MsgNack = 0x0e;
export const MsgAck = 0x0f;
export const MsgRecovery = 0x10;
const msgLast = MsgRecovery;

export const ControlHeaderLen = 3;
export const MaxControlBody = 8192;
export const MaxTicket = 2048;
export const MaxName = 64;
export const MaxText = 256;
export const MaxReportPackets = 2048;
export const MaxReaderBuffer = 1 << 20;
export const MaxNack = 128;
export const MaxAck = 16;
export const MaxEncodings = 4;

export const TrackAudio = 1;
export const TrackVideo = 2;
export const TrackAuth = 3;

export const AllStreams = 0xff;

export const ErrIncomplete = new FmmError("fmm: control message incomplete");
export const ErrType = new FmmError("fmm: unknown control message");
export const ErrBody = new FmmError("fmm: malformed control message");
export const ErrBound = new FmmError("fmm: control field out of bounds");

const EMPTY = new Uint8Array(0);
const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder("utf-8", { fatal: true });
const looseDecoder = new TextDecoder("utf-8");

class Builder {
  constructor(dst, extra = 64) {
    const start = dst ? dst.length : 0;
    this.buf = new Uint8Array(Math.max(16, start + extra));
    if (dst && start) this.buf.set(dst, 0);
    this.len = start;
  }

  grow(n) {
    if (this.len + n <= this.buf.length) return;
    const next = new Uint8Array(Math.max(this.buf.length * 2, this.len + n));
    next.set(this.buf.subarray(0, this.len), 0);
    this.buf = next;
  }

  u8(v) {
    this.grow(1);
    this.buf[this.len++] = v & 0xff;
  }

  u16(v) {
    this.grow(2);
    this.buf[this.len++] = (v >>> 8) & 0xff;
    this.buf[this.len++] = v & 0xff;
  }

  u32(v) {
    this.grow(4);
    this.buf[this.len++] = (v >>> 24) & 0xff;
    this.buf[this.len++] = (v >>> 16) & 0xff;
    this.buf[this.len++] = (v >>> 8) & 0xff;
    this.buf[this.len++] = v & 0xff;
  }

  bytes(b) {
    if (!b || !b.length) return;
    this.grow(b.length);
    this.buf.set(b, this.len);
    this.len += b.length;
  }

  zeros(n) {
    if (n <= 0) return;
    this.grow(n);
    this.buf.fill(0, this.len, this.len + n);
    this.len += n;
  }

  done() {
    return this.buf.slice(0, this.len);
  }
}

function be16(b, o) {
  return (b[o] << 8) | b[o + 1];
}

function be32(b, o) {
  return ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
}

function put16(b, o, v) {
  b[o] = (v >>> 8) & 0xff;
  b[o + 1] = v & 0xff;
}

export class Media {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.transportSeq = init.transportSeq ?? 0;
    this.seq = init.seq ?? 0;
    this.frameID = init.frameID ?? 0;
    this.fragIndex = init.fragIndex ?? 0;
    this.fragCount = init.fragCount ?? 0;
    this.keyframe = init.keyframe ?? false;
    this.fec = init.fec ?? false;
    this.recovery = init.recovery ?? false;
    this.longTerm = init.longTerm ?? false;
    this.face = init.face ?? false;
    this.layer = init.layer ?? 0;
    this.refDelta = init.refDelta ?? 0;
    this.timestamp = init.timestamp ?? 0;
    this.payload = init.payload ?? null;
  }

  validate() {
    if (this.fragCount === 0 || this.fragIndex >= this.fragCount) throw ErrFragment;
    if (this.layer > MaxLayer) throw ErrFlags;
    if (this.keyframe && (this.layer !== 0 || this.refDelta !== 0)) throw ErrFlags;
    if (this.recovery && (this.keyframe || this.layer !== 0 || this.refDelta === 0)) throw ErrFlags;
    if (MediaHeaderLen + (this.payload ? this.payload.length : 0) > MaxDatagramLen) throw ErrLong;
  }

  append(b) {
    let flags = this.layer & layerMask;
    if (this.keyframe) flags |= FlagKeyframe;
    if (this.fec) flags |= FlagFEC;
    if (this.recovery) flags |= FlagRecovery;
    if (this.longTerm) flags |= FlagLongTerm;
    if (this.face) flags |= FlagFace;
    const w = new Builder(b, MediaHeaderLen + (this.payload ? this.payload.length : 0));
    w.u8((Version << 4) | KindMedia);
    w.u8(this.stream);
    w.u16(this.transportSeq);
    w.u16(this.seq);
    w.u16(this.frameID);
    w.u8(this.fragIndex);
    w.u8(this.fragCount);
    w.u8(flags);
    w.u8(this.refDelta);
    w.u32(this.timestamp);
    w.bytes(this.payload);
    return w.done();
  }
}

export class Repair {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.transportSeq = init.transportSeq ?? 0;
    this.baseSeq = init.baseSeq ?? 0;
    this.scheme = init.scheme ?? 0;
    this.k = init.k ?? 0;
    this.m = init.m ?? 0;
    this.index = init.index ?? 0;
    this.offsets = init.offsets ?? null;
    this.symbol = init.symbol ?? null;
  }

  validate() {
    if (this.k === 0 || this.k > MaxFECK || this.m === 0 || this.m > MaxFECM || this.index >= this.m) throw ErrRepair;
    if (this.scheme === SchemeXOR) {
      if (this.m !== 1) throw ErrRepair;
    } else if (this.scheme !== SchemeRS) {
      throw ErrRepair;
    }
    const offsets = this.offsets || EMPTY;
    if (offsets.length !== this.k || offsets[0] !== 0) throw ErrRepair;
    for (let i = 1; i < offsets.length; i += 1) {
      if (offsets[i] <= offsets[i - 1]) throw ErrRepair;
    }
    const symbol = this.symbol || EMPTY;
    if (symbol.length < 2 + MediaHeaderLen - RecoveryOffset) throw ErrShort;
    if (RepairHeaderLen + offsets.length + symbol.length > MaxDatagramLen) throw ErrLong;
  }

  members(dst) {
    const out = dst || [];
    for (const o of this.offsets || EMPTY) out.push((this.baseSeq + o) & 0xffff);
    return out;
  }

  append(b) {
    const offsets = this.offsets || EMPTY;
    const symbol = this.symbol || EMPTY;
    const w = new Builder(b, RepairHeaderLen + offsets.length + symbol.length);
    w.u8((Version << 4) | KindRepair);
    w.u8(this.stream);
    w.u16(this.transportSeq);
    w.u16(this.baseSeq);
    w.u8(this.scheme);
    w.u8(this.k);
    w.u8(this.m);
    w.u8(this.index);
    w.bytes(offsets);
    w.bytes(symbol);
    return w.done();
  }
}

export function kind(b) {
  if (b.length < PaddingHeaderLen) throw ErrShort;
  if (b.length > MaxDatagramLen) throw ErrLong;
  if (b[0] >> 4 !== Version) throw ErrVersion;
  const k = b[0] & 0x0f;
  if (k > KindFeedback) throw ErrKind;
  return k;
}

export function parseMedia(b) {
  const k = kind(b);
  if (k !== KindMedia) throw ErrKind;
  if (b.length < MediaHeaderLen) throw ErrShort;
  const flags = b[10];
  const m = new Media({
    stream: b[1],
    transportSeq: be16(b, 2),
    seq: be16(b, 4),
    frameID: be16(b, 6),
    fragIndex: b[8],
    fragCount: b[9],
    keyframe: (flags & FlagKeyframe) !== 0,
    fec: (flags & FlagFEC) !== 0,
    recovery: (flags & FlagRecovery) !== 0,
    longTerm: (flags & FlagLongTerm) !== 0,
    face: (flags & FlagFace) !== 0,
    layer: flags & layerMask,
    refDelta: b[11],
    timestamp: be32(b, 12),
    payload: b.subarray(MediaHeaderLen),
  });
  m.validate();
  return m;
}

export function parseRepair(b) {
  const k = kind(b);
  if (k !== KindRepair) throw ErrKind;
  if (b.length < RepairHeaderLen) throw ErrShort;
  const n = b[7];
  if (b.length < RepairHeaderLen + n) throw ErrShort;
  const r = new Repair({
    stream: b[1],
    transportSeq: be16(b, 2),
    baseSeq: be16(b, 4),
    scheme: b[6],
    k: b[7],
    m: b[8],
    index: b[9],
    offsets: b.subarray(RepairHeaderLen, RepairHeaderLen + n),
    symbol: b.subarray(RepairHeaderLen + n),
  });
  r.validate();
  return r;
}

export function appendPadding(b, transportSeq, size) {
  const n = Math.max(size, PaddingHeaderLen);
  const w = new Builder(b, n);
  w.u8((Version << 4) | KindPadding);
  w.u8(0);
  w.u16(transportSeq);
  w.zeros(n - PaddingHeaderLen);
  return w.done();
}

export function parsePadding(b) {
  const k = kind(b);
  if (k !== KindPadding) throw ErrKind;
  if (b[1] !== 0) throw ErrPadding;
  return be16(b, 2);
}

export function transportSeqOf(b) {
  return be16(b, 2);
}

export function setTransportSeq(b, s) {
  put16(b, 2, s);
}

export function streamOf(b) {
  return b[1];
}

export function setStream(b, id) {
  b[1] = id & 0xff;
}

export function seqOf(b) {
  return be16(b, 4);
}

export function setSeq(b, s) {
  put16(b, 4, s);
}

export function setFECFlag(b, on) {
  if (on) b[10] |= FlagFEC;
  else b[10] &= ~FlagFEC & 0xff;
}

export function flagsOf(b) {
  return b[10];
}

export function frameIDOf(b) {
  return be16(b, 6);
}

export function setFrameID(b, id) {
  put16(b, 6, id);
}

export function auxOf(b) {
  return b[11];
}

export function appendFeedback(b, msgs) {
  const w = new Builder(b, FeedbackHeaderLen + (msgs ? msgs.length : 0));
  w.u8((Version << 4) | KindFeedback);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  w.bytes(msgs);
  return w.done();
}

export function parseFeedback(b) {
  const k = kind(b);
  if (k !== KindFeedback || b[1] !== 0 || b[2] !== 0 || b[3] !== 0 || b.length === FeedbackHeaderLen) throw ErrFeedback;
  const out = [];
  let rest = b.subarray(FeedbackHeaderLen);
  while (rest.length > 0) {
    let m;
    let n;
    try {
      [m, n] = decode(rest);
    } catch (_) {
      throw ErrFeedback;
    }
    if (!feedbackType(m.type())) throw ErrFeedback;
    out.push(m);
    rest = rest.subarray(n);
  }
  return out;
}

export function feedbackType(t) {
  return t === MsgReport || t === MsgNack || t === MsgAck || t === MsgRecovery;
}

export function dedupKey(b) {
  if (b.length < 4 || b[0] >> 4 !== Version) return null;
  const k = b[0] & 0x0f;
  if (k !== KindMedia && k !== KindRepair && k !== KindPadding) return null;
  return k * 65536 + be16(b, 2);
}

export class Join {
  constructor(init = {}) {
    this.ticket = init.ticket ?? "";
  }

  type() {
    return MsgJoin;
  }
}

export class Joined {
  constructor(init = {}) {
    this.peer = init.peer ?? "";
  }

  type() {
    return MsgJoined;
  }
}

export class Publish {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.kind = init.kind ?? 0;
    this.layers = init.layers ?? 0;
    this.name = init.name ?? "";
    this.track = init.track ?? 0;
    this.rank = init.rank ?? 0;
  }

  type() {
    return MsgPublish;
  }
}

export class Unpublish {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
  }

  type() {
    return MsgUnpublish;
  }
}

export class TrackAdded {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.kind = init.kind ?? 0;
    this.layers = init.layers ?? 0;
    this.peer = init.peer ?? "";
    this.name = init.name ?? "";
  }

  type() {
    return MsgTrackAdded;
  }
}

export class TrackRemoved {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
  }

  type() {
    return MsgTrackRemoved;
  }
}

export class Subscribe {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.maxLayer = init.maxLayer ?? 0;
  }

  type() {
    return MsgSubscribe;
  }
}

export class Unsubscribe {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
  }

  type() {
    return MsgUnsubscribe;
  }
}

export class KeyframeRequest {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
  }

  type() {
    return MsgKeyframeRequest;
  }
}

export class Report {
  constructor(init = {}) {
    this.base = init.base ?? 0;
    this.refTime = init.refTime ?? 0;
    this.delay = init.delay ?? 0;
    this.received = init.received ?? null;
    this.arrival = init.arrival ?? null;
  }

  type() {
    return MsgReport;
  }
}

export class BitrateHint {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.bitrate = init.bitrate ?? 0;
  }

  type() {
    return MsgBitrateHint;
  }
}

export class Leave {
  constructor(init = {}) {
    this.reason = init.reason ?? "";
  }

  type() {
    return MsgLeave;
  }
}

class ErrorMessage {
  constructor(init = {}) {
    this.code = init.code ?? 0;
    this.text = init.text ?? "";
  }

  type() {
    return MsgError;
  }
}

export { ErrorMessage as Error, ErrorMessage };

export class Nack {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.seqs = init.seqs ?? null;
  }

  type() {
    return MsgNack;
  }
}

export class Ack {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.frames = init.frames ?? null;
  }

  type() {
    return MsgAck;
  }
}

export class Recovery {
  constructor(init = {}) {
    this.stream = init.stream ?? 0;
    this.frame = init.frame ?? 0;
  }

  type() {
    return MsgRecovery;
  }
}

function validTrack(k, layers) {
  if (k === TrackAudio || k === TrackAuth) return layers === 1;
  if (k === TrackVideo) return layers >= 1 && layers <= MaxLayer + 1;
  return false;
}

export function audioLike(k) {
  return k === TrackAudio || k === TrackAuth;
}

function validEncoding(v) {
  if (v.track === 0) return v.rank === 0;
  if (v.kind !== TrackVideo || v.rank >= MaxEncodings) return false;
  return v.track !== v.stream || v.rank !== 0;
}

function appendString(w, s, limit, allowEmpty) {
  if (typeof s !== "string" || !s.isWellFormed()) throw ErrBound;
  const bytes = utf8Encoder.encode(s);
  if (bytes.length > limit || (!allowEmpty && bytes.length === 0)) throw ErrBound;
  w.u16(bytes.length);
  w.bytes(bytes);
}

function appendReport(w, r) {
  const received = r.received || [];
  const arrival = r.arrival || [];
  const n = received.length;
  if (n === 0 || n > MaxReportPackets || arrival.length !== n) throw ErrBound;
  w.u16(r.base);
  w.u16(n);
  w.u32(r.refTime);
  w.u16(r.delay);
  const mapStart = w.len;
  w.zeros(Math.trunc((n + 7) / 8));
  let prev = 0;
  let first = true;
  for (let i = 0; i < n; i += 1) {
    if (!received[i]) {
      if (arrival[i] !== 0) throw ErrBound;
      continue;
    }
    w.buf[mapStart + (i >> 3)] |= 0x80 >> (i % 8);
    if (first) {
      if (arrival[i] !== 0) throw ErrBound;
      first = false;
      continue;
    }
    const d = arrival[i] - prev;
    prev = arrival[i];
    if (d >= 0 && d <= 254) {
      w.u8(d);
    } else if (d >= -32768 && d <= 32767) {
      w.u8(0xff);
      w.u16(d & 0xffff);
    } else {
      throw ErrBound;
    }
  }
  if (first) throw ErrBound;
}

export function append(dst, m) {
  const w = new Builder(dst, 64);
  const start = w.len;
  if (!m || typeof m.type !== "function") throw ErrType;
  w.u8(m.type());
  w.u8(0);
  w.u8(0);
  if (m instanceof Join) {
    const ticket = utf8Encoder.encode(m.ticket);
    if (ticket.length === 0 || ticket.length > MaxTicket) throw ErrBound;
    w.u16(ticket.length);
    w.bytes(ticket);
  } else if (m instanceof Joined) {
    appendString(w, m.peer, MaxName, false);
  } else if (m instanceof Publish) {
    if (!validTrack(m.kind, m.layers) || !validEncoding(m)) throw ErrBound;
    w.u8(m.stream);
    w.u8(m.kind);
    w.u8(m.layers);
    appendString(w, m.name, MaxName, true);
    if (m.track !== 0) {
      w.u8(m.track);
      w.u8(m.rank);
    }
  } else if (m instanceof Unpublish || m instanceof TrackRemoved || m instanceof Unsubscribe || m instanceof KeyframeRequest) {
    w.u8(m.stream);
  } else if (m instanceof TrackAdded) {
    if (!validTrack(m.kind, m.layers)) throw ErrBound;
    w.u8(m.stream);
    w.u8(m.kind);
    w.u8(m.layers);
    appendString(w, m.peer, MaxName, false);
    appendString(w, m.name, MaxName, true);
  } else if (m instanceof Subscribe) {
    if (m.maxLayer > MaxLayer) throw ErrBound;
    w.u8(m.stream);
    w.u8(m.maxLayer);
  } else if (m instanceof Report) {
    appendReport(w, m);
  } else if (m instanceof BitrateHint) {
    w.u8(m.stream);
    w.u32(m.bitrate);
  } else if (m instanceof Leave) {
    appendString(w, m.reason, MaxText, true);
  } else if (m instanceof ErrorMessage) {
    w.u16(m.code);
    appendString(w, m.text, MaxText, true);
  } else if (m instanceof Nack) {
    const seqs = m.seqs || [];
    if (seqs.length === 0 || seqs.length > MaxNack) throw ErrBound;
    w.u8(m.stream);
    w.u8(seqs.length);
    for (const q of seqs) w.u16(q);
  } else if (m instanceof Ack) {
    const frames = m.frames || [];
    if (frames.length === 0 || frames.length > MaxAck) throw ErrBound;
    w.u8(m.stream);
    w.u8(frames.length);
    for (const q of frames) w.u16(q);
  } else if (m instanceof Recovery) {
    w.u8(m.stream);
    w.u16(m.frame);
  } else {
    throw ErrType;
  }
  const body = w.len - start - ControlHeaderLen;
  if (body > MaxControlBody) throw ErrBound;
  put16(w.buf, start + 1, body);
  return w.done();
}

class BodyReader {
  constructor(b) {
    this.b = b;
    this.err = false;
  }

  u8() {
    if (this.b.length < 1) {
      this.err = true;
      return 0;
    }
    const v = this.b[0];
    this.b = this.b.subarray(1);
    return v;
  }

  u16() {
    if (this.b.length < 2) {
      this.err = true;
      return 0;
    }
    const v = be16(this.b, 0);
    this.b = this.b.subarray(2);
    return v;
  }

  u32() {
    if (this.b.length < 4) {
      this.err = true;
      return 0;
    }
    const v = be32(this.b, 0);
    this.b = this.b.subarray(4);
    return v;
  }

  bytes(n) {
    if (n > this.b.length) {
      this.err = true;
      return null;
    }
    const v = this.b.subarray(0, n);
    this.b = this.b.subarray(n);
    return v;
  }

  str(limit, allowEmpty) {
    const n = this.u16();
    if (this.err || n > limit || (!allowEmpty && n === 0)) {
      this.err = true;
      return "";
    }
    const s = this.bytes(n);
    if (this.err) return "";
    try {
      return utf8Decoder.decode(s);
    } catch (_) {
      this.err = true;
      return "";
    }
  }
}

function decodeReport(r) {
  const rep = new Report({ base: r.u16() });
  const n = r.u16();
  rep.refTime = r.u32();
  rep.delay = r.u16();
  if (r.err || n === 0 || n > MaxReportPackets) return null;
  const bitmap = r.bytes(Math.trunc((n + 7) / 8));
  if (r.err) return null;
  if (n % 8 !== 0 && (bitmap[bitmap.length - 1] & (0xff >> (n % 8))) !== 0) return null;
  rep.received = new Array(n).fill(false);
  rep.arrival = new Array(n).fill(0);
  let first = true;
  let prev = 0;
  for (let i = 0; i < n; i += 1) {
    if ((bitmap[i >> 3] & (0x80 >> (i % 8))) === 0) continue;
    rep.received[i] = true;
    if (first) {
      first = false;
      continue;
    }
    const c = r.u8();
    if (r.err) return null;
    let d = c;
    if (c === 0xff) {
      const raw = r.u16();
      d = raw >= 0x8000 ? raw - 0x10000 : raw;
      if (r.err || (d >= 0 && d <= 254)) return null;
    }
    prev += d;
    rep.arrival[i] = prev;
  }
  if (first) return null;
  return rep;
}

export function decode(b) {
  if (b.length < ControlHeaderLen) throw ErrIncomplete;
  const t = b[0];
  if (t < MsgJoin || t > msgLast) throw ErrType;
  const n = be16(b, 1);
  if (n > MaxControlBody) throw ErrBound;
  if (b.length < ControlHeaderLen + n) throw ErrIncomplete;
  const r = new BodyReader(b.subarray(ControlHeaderLen, ControlHeaderLen + n));
  let m;
  switch (t) {
    case MsgJoin: {
      const l = r.u16();
      if (r.err || l === 0 || l > MaxTicket) throw ErrBody;
      const raw = r.bytes(l);
      m = new Join({ ticket: raw ? looseDecoder.decode(raw) : "" });
      break;
    }
    case MsgJoined:
      m = new Joined({ peer: r.str(MaxName, false) });
      break;
    case MsgPublish: {
      const v = new Publish({ stream: r.u8(), kind: r.u8(), layers: r.u8() });
      v.name = r.str(MaxName, true);
      if (r.b.length === 2) {
        v.track = r.u8();
        v.rank = r.u8();
        if (v.track === 0) throw ErrBody;
      }
      if (!validTrack(v.kind, v.layers) || !validEncoding(v)) throw ErrBody;
      m = v;
      break;
    }
    case MsgUnpublish:
      m = new Unpublish({ stream: r.u8() });
      break;
    case MsgTrackAdded: {
      const v = new TrackAdded({ stream: r.u8(), kind: r.u8(), layers: r.u8() });
      v.peer = r.str(MaxName, false);
      v.name = r.str(MaxName, true);
      if (!validTrack(v.kind, v.layers)) throw ErrBody;
      m = v;
      break;
    }
    case MsgTrackRemoved:
      m = new TrackRemoved({ stream: r.u8() });
      break;
    case MsgSubscribe: {
      const v = new Subscribe({ stream: r.u8(), maxLayer: r.u8() });
      if (v.maxLayer > MaxLayer) throw ErrBody;
      m = v;
      break;
    }
    case MsgUnsubscribe:
      m = new Unsubscribe({ stream: r.u8() });
      break;
    case MsgKeyframeRequest:
      m = new KeyframeRequest({ stream: r.u8() });
      break;
    case MsgReport: {
      const rep = decodeReport(r);
      if (!rep) throw ErrBody;
      m = rep;
      break;
    }
    case MsgBitrateHint:
      m = new BitrateHint({ stream: r.u8(), bitrate: r.u32() });
      break;
    case MsgLeave:
      m = new Leave({ reason: r.str(MaxText, true) });
      break;
    case MsgError: {
      const v = new ErrorMessage({ code: r.u16() });
      v.text = r.str(MaxText, true);
      m = v;
      break;
    }
    case MsgNack: {
      const v = new Nack({ stream: r.u8() });
      const count = r.u8();
      if (r.err || count === 0 || count > MaxNack) throw ErrBody;
      v.seqs = new Array(count);
      for (let i = 0; i < count; i += 1) v.seqs[i] = r.u16();
      m = v;
      break;
    }
    case MsgAck: {
      const v = new Ack({ stream: r.u8() });
      const count = r.u8();
      if (r.err || count === 0 || count > MaxAck) throw ErrBody;
      v.frames = new Array(count);
      for (let i = 0; i < count; i += 1) v.frames[i] = r.u16();
      m = v;
      break;
    }
    case MsgRecovery:
      m = new Recovery({ stream: r.u8(), frame: r.u16() });
      break;
    default:
      throw ErrType;
  }
  if (r.err || r.b.length !== 0) throw ErrBody;
  return [m, ControlHeaderLen + n];
}

export class Reader {
  constructor(init = {}) {
    this.buf = init.buf ?? new Uint8Array(0);
    this.len = this.buf.length;
    this.broken = init.broken ?? null;
  }

  write(p) {
    if (this.broken) throw this.broken;
    if (this.len + p.length > MaxReaderBuffer) {
      this.broken = ErrBound;
      throw this.broken;
    }
    if (this.len + p.length > this.buf.length) {
      const next = new Uint8Array(Math.max(this.buf.length * 2, this.len + p.length, 256));
      next.set(this.buf.subarray(0, this.len), 0);
      this.buf = next;
    }
    this.buf.set(p, this.len);
    this.len += p.length;
  }

  next() {
    if (this.broken) throw this.broken;
    let m;
    let n;
    try {
      [m, n] = decode(this.buf.subarray(0, this.len));
    } catch (err) {
      if (err === ErrIncomplete) return null;
      this.broken = err;
      throw err;
    }
    this.buf.copyWithin(0, n, this.len);
    this.len -= n;
    return m;
  }
}
