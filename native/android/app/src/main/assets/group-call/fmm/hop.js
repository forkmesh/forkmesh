import * as wire from "./wire.js";
import * as cc from "./cc.js";
import * as fec from "./fec.js";
import { Unwrapper16 } from "./seqnum.js";

export const AudioFECDelayUs = 60_000;
export const VideoFECDelayUs = 50_000;
const fecUpdateUs = 200_000;
const rateWindowUs = 1_000_000;
export const MaxPacketSize = 1200;
export const PacketOverhead = 48;
export const DREDCoverUs = 100_000;
export const DREDFECShare = 0.15;

class meter {
  constructor() {
    this.bytes = 0;
    this.packets = 0;
    this.start = 0;
    this.bps = 0;
    this.pps = 0;
  }

  add(n) {
    this.bytes += n;
    this.packets++;
  }

  roll(now) {
    if (this.start === 0) {
      this.start = now;
      return;
    }
    const dt = now - this.start;
    if (dt < rateWindowUs) return;
    const bps = (this.bytes * 8e6) / dt;
    const pps = (this.packets * 1e6) / dt;
    if (this.bps === 0) {
      this.bps = bps;
      this.pps = pps;
    } else {
      this.bps = 0.5 * this.bps + 0.5 * bps;
      this.pps = 0.5 * this.pps + 0.5 * pps;
    }
    this.bytes = 0;
    this.packets = 0;
    this.start = now;
  }
}

const historySize = 512;
const historyAgeUs = 1_000_000;
const rtxShare = 0.3;
const nackReorderUs = 5_000;
const nackMaxAgeUs = 600_000;
const nackTries = 3;
const nackMaxMissing = 256;
const nackMaxJump = 512;

class sentSlot {
  constructor(seq = 0, valid = false, data = null, sentAt = 0) {
    this.seq = seq;
    this.valid = valid;
    this.data = data;
    this.sentAt = sentAt;
    this.rtxAt = 0;
  }
}

class sendStream {
  constructor(audio, enc) {
    this.audio = audio;
    this.depth = 0;
    this.fec = enc;
    this.media = new meter();
    this.repair = new meter();
    this.lower = 0;
    this.upper = 0;
    this.history = new Array(historySize);
    for (let i = 0; i < historySize; i++) this.history[i] = new sentSlot();
  }

  importantShare() {
    const t = this.lower + this.upper;
    if (t < 20) return 0.4;
    const share = this.lower / t;
    if (t > 2000) {
      this.lower = Math.trunc(this.lower / 2);
      this.upper = Math.trunc(this.upper / 2);
    }
    return Math.min(Math.max(share, 0.1), 1);
  }
}

function sendErr(t, b) {
  try {
    return t.sendDatagram(b) ? 1 : 0;
  } catch (_) {
    return 1;
  }
}

export class Sender {
  constructor(t, cfg) {
    this.t = t;
    this.cc = cc.newController(cfg);
    this._pacer = new cc.Pacer();
    this.tseq = 0;
    this.streams = new Map();
    this.order = [];
    this.probeID = 0;
    this.lastFEC = 0;
    this.now = 0;
    this.fixedFEC = null;
    this.audioFECWithDRED = false;
    this.sendErrors = 0;
    this.retransmits = 0;
    this.rtxSkipped = 0;
    this.rtxStart = 0;
    this.rtxBytes = 0;
    this.sentMeter = new meter();
    this.sent = 0;
    this.sentBytes = 0;
    this.padding = 0;
  }

  controller() {
    return this.cc;
  }

  pacer() {
    return this._pacer;
  }

  maxPayload() {
    return Math.min(this.t.maxDatagramSize(), MaxPacketSize) - wire.MediaHeaderLen - wire.FECReserve;
  }

  addStream(id, audio) {
    const delay = audio ? AudioFECDelayUs : VideoFECDelayUs;
    if (!this.streams.get(id)) {
      this.order.push(id);
      this.order.sort((a, b) => a - b);
    }
    const enc = fec.newEncoder(id, Math.min(this.t.maxDatagramSize(), MaxPacketSize), delay);
    enc.setUnequal(!audio);
    this.streams.set(id, new sendStream(audio, enc));
  }

  removeStream(id) {
    this.streams.delete(id);
    this.order = this.order.filter((x) => x !== id);
  }

  hasStream(id) {
    return !!this.streams.get(id);
  }

  send(pkt, now) {
    const st = this.streams.get(wire.streamOf(pkt));
    if (!st) return;
    const klass = st.audio ? cc.ClassAudio : cc.ClassVideo;
    if (st.audio) st.depth = wire.auxOf(pkt);
    const repairs = st.fec.add(pkt, now);
    st.media.add(pkt.length);
    if (!st.audio) {
      const seq = wire.seqOf(pkt);
      st.history[seq % historySize] = new sentSlot(seq, true, pkt, now);
    }
    if (!st.audio && (pkt[10] & (wire.FlagKeyframe | 0x07)) !== 0 && (pkt[10] & wire.FlagKeyframe) === 0) st.upper++;
    else st.lower++;
    this._pacer.push(klass, pkt, now);
    for (const r of repairs) {
      st.repair.add(r.length);
      this._pacer.push(klass, r, now);
    }
  }

  onReport(r, now) {
    this.cc.onReport(r, now);
  }

  onNack(n, now) {
    const st = this.streams.get(n.stream);
    if (!st || st.audio) return;
    this.rtxRoll(now);
    const gap = Math.max(this.cc.rtt(), 20_000);
    const budget = (rtxShare * this.cc.target()) / 8;
    for (const q of n.seqs) {
      const h = st.history[q % historySize];
      if (!h.valid || h.seq !== q || now - h.sentAt > historyAgeUs || (h.rtxAt !== 0 && now - h.rtxAt < gap)) continue;
      if (this.rtxBytes + h.data.length > budget) {
        this.rtxSkipped++;
        continue;
      }
      h.rtxAt = now;
      this.rtxBytes += h.data.length;
      this.retransmits++;
      this._pacer.push(cc.ClassRTX, h.data.slice(), now);
    }
  }

  rtxRoll(now) {
    if (now - this.rtxStart >= 1_000_000) {
      this.rtxStart = now;
      this.rtxBytes = 0;
    }
  }

  tick(now) {
    this.now = now;
    for (const id of this.order) {
      const st = this.streams.get(id);
      const klass = st.audio ? cc.ClassAudio : cc.ClassVideo;
      for (const r of st.fec.flush(now)) {
        st.repair.add(r.length);
        this._pacer.push(klass, r, now);
      }
      st.media.roll(now);
      st.repair.roll(now);
    }
    this.sentMeter.roll(now);
    if (now - this.lastFEC >= fecUpdateUs) {
      this.lastFEC = now;
      this.updateFEC();
    }
    this.cc.checkFeedback(now);
    let factor = cc.PacingFactor;
    if (this.cc.congested(now)) {
      factor = cc.CongestedPacingFactor;
      this._pacer.dropOlder(cc.CongestedVideoQueueUs, now);
    }
    this._pacer.setRate(Math.trunc(this.cc.target() * factor));
    this._pacer.setDrainCap(Math.trunc(this.cc.target() * factor));
    if (this.probeID === 0) {
      const probe = this.cc.startProbe(now);
      if (probe) {
        const [id, rate, bytes] = probe;
        this.probeID = id;
        this._pacer.setProbe(rate, bytes);
      }
    }
    this._pacer.tick(now, (b, probe) => this.transmit(b, probe), (size) => this.pad(size));
    if (this.probeID !== 0 && !this._pacer.probing()) {
      this.cc.endProbe(this.probeID, now);
      this.probeID = 0;
    }
  }

  updateFEC() {
    let avail = this.cc.target() - this.overheadBps();
    for (const id of this.order) avail -= this.streams.get(id).media.bps;
    for (const audio of [true, false]) {
      let want = 0;
      for (const id of this.order) {
        const st = this.streams.get(id);
        if (st.audio !== audio) continue;
        this.chooseFEC(st);
        want += st.media.bps * st.fec.ratio(st.importantShare());
      }
      if (want <= 0) continue;
      let scale = 1;
      if (want > avail && this.cc.congested(this.now)) scale = Math.max(avail, 0) / want;
      for (const id of this.order) {
        const st = this.streams.get(id);
        if (st.audio === audio && scale < 1) st.fec.scale(scale);
      }
      avail -= want * scale;
    }
  }

  chooseFEC(st) {
    if (this.fixedFEC) {
      st.fec.setParams(new fec.Params({ ...this.fixedFEC }));
      return;
    }
    const delay = st.audio ? AudioFECDelayUs : VideoFECDelayUs;
    const maxK = Math.max(1, Math.trunc((st.media.pps * delay) / 1e6));
    if (st.audio) {
      let p = fec.choose(true, this.cc.loss(), this.cc.burst(), maxK);
      if (st.depth * wire.DepthUnitUs >= DREDCoverUs && !this.audioFECWithDRED && st.media.bps * p.ratio() > DREDFECShare * this.cc.target()) {
        p = new fec.Params();
      }
      st.fec.setParams(p);
      return;
    }
    const share = st.importantShare();
    const [imp, oth] = fec.chooseVideo(
      this.cc.loss(),
      this.cc.burst(),
      Math.max(1, Math.trunc(maxK * share)),
      Math.max(1, Math.trunc(maxK * (1 - share))),
    );
    st.fec.setClassParams(imp, oth);
  }

  pad(size) {
    this.padding++;
    return wire.appendPadding(null, 0, Math.min(size, this.t.maxDatagramSize()));
  }

  transmit(b, probe) {
    this.tseq = (this.tseq + 1) & 0xffff;
    wire.setTransportSeq(b, this.tseq);
    const pid = probe ? this.probeID : 0;
    this.cc.onSent(this.tseq, b.length + PacketOverhead, this.now, pid);
    this.sentMeter.add(b.length);
    this.sent++;
    this.sentBytes += b.length;
    this.sendErrors += sendErr(this.t, b);
  }

  target() {
    return this.cc.target();
  }

  overheadBps() {
    return this.sentMeter.pps * PacketOverhead * 8;
  }

  streamOverheadBps(id) {
    const st = this.streams.get(id);
    if (st) return (st.media.pps + st.repair.pps) * PacketOverhead * 8;
    return 0;
  }

  fecRatio(id) {
    const st = this.streams.get(id);
    if (st) return st.fec.ratio(st.importantShare());
    return 0;
  }

  streamRate(id) {
    const st = this.streams.get(id);
    if (st) return [st.media.bps, st.repair.bps];
    return [0, 0];
  }
}

export function newSender(t, cfg) {
  return new Sender(t, cfg);
}

class miss {
  constructor(since) {
    this.since = since;
    this.lastSent = 0;
    this.tries = 0;
  }
}

class nackState {
  constructor() {
    this.seqs = new Unwrapper16();
    this.highest = 0;
    this.init = false;
    this.missing = new Map();
  }

  onMedia(seq, now) {
    const s = this.seqs.unwrap(seq);
    if (!this.init) {
      this.init = true;
      this.highest = s;
      return;
    }
    if (s > this.highest) {
      if (s - this.highest > nackMaxJump) {
        this.missing.clear();
      } else {
        for (let q = this.highest + 1; q < s && this.missing.size < nackMaxMissing; q++) this.missing.set(q, new miss(now));
      }
      this.highest = s;
      return;
    }
    this.missing.delete(s);
  }

  collect(now, rtt) {
    const out = [];
    const gap = Math.max(Math.trunc((rtt * 3) / 2), 20_000);
    for (const q of [...this.missing.keys()].sort((a, b) => a - b)) {
      const m = this.missing.get(q);
      if (now - m.since > nackMaxAgeUs || m.tries >= nackTries) {
        this.missing.delete(q);
        continue;
      }
      if ((m.tries === 0 && now - m.since >= nackReorderUs) || (m.tries > 0 && now - m.lastSent >= gap)) {
        m.tries++;
        m.lastSent = now;
        out.push(q & 0xffff);
        if (out.length === wire.MaxNack) break;
      }
    }
    return out;
  }
}

export class Receiver {
  constructor() {
    this.nack = null;
    this.rtt = 0;
    this.nacked = 0;
    this.rec = new cc.Recorder();
    this.dec = new Map();
    this.media = 0;
    this.recovered = 0;
    this.repairs = 0;
    this.padding = 0;
    this.invalid = 0;
  }

  decoder(id) {
    let d = this.dec.get(id);
    if (!d) {
      d = fec.newDecoder(id);
      this.dec.set(id, d);
    }
    return d;
  }

  handle(p, now) {
    let k;
    try {
      k = wire.kind(p);
    } catch (_) {
      this.invalid++;
      return [];
    }
    if (k === wire.KindMedia) {
      let m;
      try {
        m = wire.parseMedia(p);
      } catch (_) {
        this.invalid++;
        return [];
      }
      this.rec.onPacket(m.transportSeq, now);
      this.media++;
      const rec = this.decoder(m.stream).addMedia(p, m.fec, now);
      this.recovered += rec.length;
      const n = this.nack?.get(m.stream);
      if (n) {
        n.onMedia(m.seq, now);
        for (const x of rec) n.onMedia(wire.seqOf(x), now);
      }
      return [p, ...rec];
    }
    if (k === wire.KindRepair) {
      let rp;
      try {
        rp = wire.parseRepair(p);
      } catch (_) {
        this.invalid++;
        return [];
      }
      this.rec.onPacket(rp.transportSeq, now);
      this.repairs++;
      const rec = this.decoder(rp.stream).addRepair(rp, now);
      this.recovered += rec.length;
      const n = this.nack?.get(rp.stream);
      if (n) for (const x of rec) n.onMedia(wire.seqOf(x), now);
      return rec;
    }
    let ts;
    try {
      ts = wire.parsePadding(p);
    } catch (_) {
      this.invalid++;
      return [];
    }
    this.rec.onPacket(ts, now);
    this.padding++;
    return [];
  }

  removeStream(id) {
    this.dec.delete(id);
    this.nack?.delete(id);
  }

  enableNack(id) {
    if (!this.nack) this.nack = new Map();
    if (!this.nack.get(id)) this.nack.set(id, new nackState());
  }

  setRTT(rtt) {
    this.rtt = rtt;
  }

  nacks(now) {
    const out = [];
    if (!this.nack) return out;
    for (const id of [...this.nack.keys()].sort((a, b) => a - b)) {
      const seqs = this.nack.get(id).collect(now, this.rtt);
      if (seqs.length === 0) continue;
      this.nacked += seqs.length;
      try {
        out.push(wire.append(null, new wire.Nack({ stream: id, seqs })));
      } catch (_) {}
    }
    return out;
  }

  report(now) {
    const rep = this.rec.report(now);
    if (!rep) return null;
    try {
      return wire.append(null, rep);
    } catch (_) {
      return null;
    }
  }

  setRepeat(n) {
    this.rec.repeat = n;
  }
}

export function newReceiver() {
  return new Receiver();
}

function concat(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

export function packFeedback(msgs, maxSize) {
  const datagrams = [];
  const control = [];
  let cur = new Uint8Array(0);
  for (const m of msgs) {
    if (wire.FeedbackHeaderLen + m.length > maxSize) {
      control.push(m);
      continue;
    }
    if (cur.length > 0 && wire.FeedbackHeaderLen + cur.length + m.length > maxSize) {
      datagrams.push(wire.appendFeedback(null, cur));
      cur = new Uint8Array(0);
    }
    cur = concat(cur, m);
  }
  if (cur.length > 0) datagrams.push(wire.appendFeedback(null, cur));
  return [datagrams, control];
}
