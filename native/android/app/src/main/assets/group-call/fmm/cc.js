import { Report, MaxReportPackets } from "./wire.js";
import { near16, near32 } from "./seqnum.js";

export const TickUs = 250;
const recorderSlots = 4096;
export const ReportInterval = 50_000;
export const FinalizeHold = 100_000;
const reportMaxWindow = MaxReportPackets;

const historySlots = 8192;

const stateHold = 0;
const stateIncrease = 1;
const stateDecrease = 2;

export const SignalNormal = 0;
export const SignalOver = 1;
export const SignalUnder = 2;

const groupSpanUs = 5_000;
const trendWindow = 20;
const trendSmoothing = 0.9;
const trendSmoothingRest = 0.1;
const trendGain = 4.0;
const overuseTimeMs = 10.0;
const thresholdInit = 12.5;
const thresholdMin = 6.0;
const thresholdMax = 600.0;
const thresholdUp = 0.0087;
const thresholdDown = 0.039;
const adaptOffsetMs = 15.0;
const beta = 0.85;
const betaMin = 0.5;
const drainBeta = 0.5;
const ackedWindowUs = 500_000;
const lossWindowUs = 1_000_000;
const lossWindowPkts = 200;
const lossLow = 0.02;
const lossHigh = 0.10;
const lossHighRandom = 0.35;
const queueEvidenceUs = 40_000;
const overuseMemoryUs = 2_000_000;
const minRTTWindowUs = 10_000_000;
const probeMinPackets = 5;
const minProbePacket = 250;
const probeDurationUs = 15_000;
const probeEvalDelayUs = 300_000;
const probeInterval = 5_000_000;
const probeQuiet = 3_000_000;
const probeFollowUp = 500_000;
const probeMaxFactor = 2.0;
const fastCutQueueUs = 150_000;
const recentWindowUs = 200_000;
const silenceReports = 3;
const silenceStepUs = 100_000;
const silenceFactor = 0.5;
const drainedUs = 25_000;
const recoveryProbeUs = 300_000;
const appLimitedRoom = 50_000;
const lowProbePacket = 80;
const lowProbeUs = 40_000;
const probeMemoryUs = 5_000_000;
const probeFloor = 0.85;
const standingQueueUs = 40_000;
const standWindowUs = 200_000;
const standRiseUs = 4_000;
const requestGapUs = 2_000_000;
const requestQuietUs = 1_000_000;

export const ClassAudio = 0;
export const ClassRTX = 1;
export const ClassVideo = 2;
const classes = 3;

export const PacingFactor = 2.5;
export const CongestedPacingFactor = 1.25;
export const CongestedVideoQueueUs = 200_000;
export const MaxVideoQueueUs = 400_000;
const maxBurstUs = 10_000;
const drainTargetUs = 500_000;
const maxQueueBytes = 2 << 20;
export const PaddingSize = 1200;
const maxProbeBudget = 64;

const div = (a, b) => Math.trunc(a / b);

class recSlot {
  constructor(init = {}) {
    this.seq = init.seq ?? 0;
    this.has = init.has ?? false;
    this.reported = init.reported ?? 0;
    this.arrival = init.arrival ?? 0;
    this.missingSince = init.missingSince ?? 0;
  }
}

export class Recorder {
  constructor(init = {}) {
    this.slots = Array.from({ length: recorderSlots }, () => new recSlot());
    this.started = init.started ?? false;
    this.base = init.base ?? 0;
    this.highest = init.highest ?? 0;
    this.lastArrival = init.lastArrival ?? 0;
    this.lastReport = init.lastReport ?? 0;
    this.repeat = init.repeat ?? 0;
    this.packets = init.packets ?? 0;
  }

  onPacket(tseq, now) {
    this.packets++;
    if (!this.started) {
      this.started = true;
      this.highest = tseq;
      this.base = this.highest;
      this.lastArrival = now;
      this.lastReport = now - ReportInterval;
      this.slots[this.highest % recorderSlots] = new recSlot({ seq: this.highest, has: true, arrival: now });
      return;
    }
    const s = near16(tseq, this.highest);
    if (s < this.base) return;
    if (s > this.highest + recorderSlots / 2) {
      this.base = s;
      this.highest = s;
    }
    if (s - this.base >= reportMaxWindow) this.base = s - reportMaxWindow + 1;
    const i = s % recorderSlots;
    const sl = this.slots[i];
    if (sl.seq === s && sl.has) return;
    this.slots[i] = new recSlot({ seq: s, has: true, arrival: now });
    if (s > this.highest) {
      this.highest = s;
      this.lastArrival = now;
    }
  }

  due(now) {
    return this.started && this.highest >= this.base && now - this.lastReport >= ReportInterval;
  }

  report(now) {
    if (!this.due(now)) return null;
    this.lastReport = now;
    const n = this.highest - this.base + 1;
    const rep = new Report({
      base: this.base & 0xffff,
      received: new Array(n).fill(false),
      arrival: new Array(n).fill(0),
    });
    let ref = -1;
    let prev = 0;
    for (let i = 0; i < n; i++) {
      const s = this.base + i;
      const k = s % recorderSlots;
      const sl = this.slots[k];
      if (sl.seq === s && sl.has) {
        const t = div(sl.arrival, TickUs);
        if (ref < 0) {
          ref = t;
          rep.refTime = t >>> 0;
        }
        let off = t - ref;
        off = Math.min(Math.max(off, prev - 32768), prev + 32767);
        prev = off;
        rep.received[i] = true;
        rep.arrival[i] = off;
        sl.reported = (sl.reported + 1) & 0xff;
        continue;
      }
      if (sl.seq !== s) this.slots[k] = new recSlot({ seq: s, missingSince: now });
    }
    rep.delay = Math.min(div(now - this.lastArrival, TickUs), 0xffff) & 0xffff;
    while (this.base <= this.highest) {
      const sl = this.slots[this.base % recorderSlots];
      if (sl.seq === this.base && ((sl.has && sl.reported > this.repeat) || (!sl.has && now - sl.missingSince >= FinalizeHold))) {
        this.base++;
        continue;
      }
      break;
    }
    return rep;
  }
}

export class Config {
  constructor(init = {}) {
    this.startBps = init.startBps ?? 0;
    this.minBps = init.minBps ?? 0;
    this.maxBps = init.maxBps ?? 0;
  }

  withDefaults() {
    const c = new Config(this);
    if (c.minBps <= 0) c.minBps = 50_000;
    if (c.maxBps <= 0) c.maxBps = 3_000_000;
    if (c.startBps <= 0) c.startBps = 300_000;
    c.startBps = Math.min(Math.max(c.startBps, c.minBps), c.maxBps);
    return c;
  }
}

const pktPending = 0;
const pktReceived = 1;
const pktLost = 2;

class sentPacket {
  constructor(init = {}) {
    this.seq = init.seq ?? 0;
    this.sendTime = init.sendTime ?? 0;
    this.arrival = init.arrival ?? 0;
    this.size = init.size ?? 0;
    this.probe = init.probe ?? 0;
    this.state = init.state ?? pktPending;
  }
}

const iaGroup = () => ({ firstSend: 0, lastSend: 0, lastArrival: 0, valid: false });

class interArrival {
  constructor() {
    this.cur = iaGroup();
    this.prev = iaGroup();
  }

  add(send, arrival) {
    if (!this.cur.valid) {
      this.cur.firstSend = send;
      this.cur.lastSend = send;
      this.cur.lastArrival = arrival;
      this.cur.valid = true;
      return [0, 0, false];
    }
    if (send < this.cur.firstSend) return [0, 0, false];
    if (send - this.cur.firstSend <= groupSpanUs) {
      this.cur.lastSend = Math.max(this.cur.lastSend, send);
      this.cur.lastArrival = Math.max(this.cur.lastArrival, arrival);
      return [0, 0, false];
    }
    let sendDelta = 0;
    let arrDelta = 0;
    let ok = false;
    if (this.prev.valid) {
      sendDelta = this.cur.lastSend - this.prev.lastSend;
      arrDelta = this.cur.lastArrival - this.prev.lastArrival;
      ok = true;
    }
    this.prev = { ...this.cur };
    this.cur.firstSend = send;
    this.cur.lastSend = send;
    this.cur.lastArrival = arrival;
    return [sendDelta, arrDelta, ok];
  }
}

class trendline {
  constructor() {
    this.acc = 0;
    this.smoothed = 0;
    this.first = 0;
    this.haveFirst = false;
    this.xs = new Array(trendWindow).fill(0);
    this.ys = new Array(trendWindow).fill(0);
    this.n = 0;
    this.head = 0;
    this.count = 0;
    this.trend = 0;
  }

  update(sendDelta, arrDelta, arrival) {
    if (!this.haveFirst) {
      this.first = arrival;
      this.haveFirst = true;
    }
    const d = (arrDelta - sendDelta) / 1000;
    this.count++;
    this.acc += d;
    this.smoothed = trendSmoothing * this.smoothed + trendSmoothingRest * this.acc;
    this.xs[this.head] = (arrival - this.first) / 1000;
    this.ys[this.head] = this.smoothed;
    this.head = (this.head + 1) % trendWindow;
    if (this.n < trendWindow) this.n++;
    if (this.n === trendWindow) {
      let sx = 0;
      let sy = 0;
      for (let i = 0; i < trendWindow; i++) {
        sx += this.xs[i];
        sy += this.ys[i];
      }
      const mx = sx / trendWindow;
      const my = sy / trendWindow;
      let num = 0;
      let den = 0;
      for (let i = 0; i < trendWindow; i++) {
        num += (this.xs[i] - mx) * (this.ys[i] - my);
        den += (this.xs[i] - mx) * (this.xs[i] - mx);
      }
      if (den !== 0) this.trend = num / den;
    }
    return Math.min(this.count, 60) * this.trend * trendGain;
  }
}

class detector {
  constructor() {
    this.threshold = 0;
    this.lastUpdate = 0;
    this.overTime = 0;
    this.overCount = 0;
    this.prevTrend = 0;
    this.signal = 0;
  }

  detect(trend, sendDeltaMs, count, now) {
    if (count < 2) return SignalNormal;
    if (trend > this.threshold) {
      if (this.overTime < 0) this.overTime = sendDeltaMs / 2;
      else this.overTime += sendDeltaMs;
      this.overCount++;
      if (this.overTime > overuseTimeMs && this.overCount > 1 && trend >= this.prevTrend) {
        this.overTime = 0;
        this.overCount = 0;
        this.signal = SignalOver;
      }
    } else if (trend < -this.threshold) {
      this.overTime = -1;
      this.overCount = 0;
      this.signal = SignalUnder;
    } else {
      this.overTime = -1;
      this.overCount = 0;
      this.signal = SignalNormal;
    }
    this.prevTrend = trend;
    const a = Math.abs(trend);
    if (this.lastUpdate === 0) this.lastUpdate = now;
    if (a <= this.threshold + adaptOffsetMs) {
      let k = thresholdDown;
      if (a > this.threshold) k = thresholdUp;
      const dt = Math.min((now - this.lastUpdate) / 1000, 100);
      this.threshold = Math.min(Math.max(this.threshold + k * (a - this.threshold) * dt, thresholdMin), thresholdMax);
    }
    this.lastUpdate = now;
    return this.signal;
  }
}

class aimd {
  constructor() {
    this.rate = 0;
    this.state = 0;
    this.lastChange = 0;
    this.lastDecrease = 0;
    this.capEst = 0;
    this.capVar = 0;
    this.haveCap = false;
  }

  capUpper() {
    const dev = Math.sqrt((this.capVar * this.capEst) / 1000) * 1000;
    return this.capEst + 3 * dev;
  }

  update(signal, acked, floor, rtt, queueing, now, minBps, maxBps) {
    if (signal === SignalOver) this.state = stateDecrease;
    else if (signal === SignalUnder) this.state = stateHold;
    else if (this.state === stateHold) this.state = stateIncrease;
    let dt = Math.min(now - this.lastChange, 1_000_000) / 1e6;
    if (this.lastChange === 0) dt = 0;
    this.lastChange = now;
    if (this.state === stateIncrease) {
      const prev = this.rate;
      if (this.haveCap && acked > this.capUpper()) this.haveCap = false;
      if (this.haveCap) {
        const response = (rtt + 100_000) / 1e6;
        this.rate += Math.max(4000, (1200 * 8) / response) * dt;
      } else {
        this.rate += Math.max(this.rate * (Math.pow(1.08, dt) - 1), 1000 * dt);
      }
      const lim = Math.max(1.5 * acked + 10_000, acked + appLimitedRoom);
      if (acked > 0 && this.rate > lim) this.rate = Math.max(prev, lim);
    } else if (this.state === stateDecrease) {
      const wait = Math.min(Math.max(rtt, 10_000), 200_000);
      if (now - this.lastDecrease >= wait) {
        let base = Math.max(acked, floor);
        if (base <= 0) base = this.rate;
        const b = Math.min(Math.max(beta - (drainBeta * queueing) / 1e6, betaMin), beta);
        let nr = b * base;
        if (nr > this.rate && this.haveCap) nr = b * this.capEst;
        if (nr < this.rate) this.rate = nr;
        if (acked > 0) {
          const x = acked / 1000;
          if (!this.haveCap) {
            this.capEst = x * 1000;
            this.capVar = 0.4;
            this.haveCap = true;
          } else {
            let e = this.capEst / 1000;
            e = 0.95 * e + 0.05 * x;
            const v = 0.95 * this.capVar + (0.05 * (e - x) * (e - x)) / Math.max(e, 1);
            this.capEst = e * 1000;
            this.capVar = Math.min(Math.max(v, 0.4), 2.5);
          }
        }
        this.lastDecrease = now;
      }
      this.state = stateHold;
    }
    this.rate = Math.min(Math.max(this.rate, minBps), maxBps);
  }
}

class probe {
  constructor(init = {}) {
    this.id = init.id ?? 0;
    this.rate = init.rate ?? 0;
    this.bytes = init.bytes ?? 0;
    this.sentBytes = 0;
    this.sentPkts = 0;
    this.firstSend = 0;
    this.lastSend = 0;
    this.lastSize = 0;
    this.recvBytes = 0;
    this.recvPkts = 0;
    this.firstArr = 0;
    this.lastArr = 0;
    this.firstArrSize = 0;
    this.ended = false;
    this.endAt = 0;
  }
}

export class Controller {
  constructor(init = {}) {
    this.cfg = new Config(init.cfg ?? {});
    this.hist = init.hist ?? [];
    this.haveSent = false;
    this.lastSeq = 0;
    this.finalized = 0;
    this.haveRef = false;
    this.refTicks = 0;
    this.ia = new interArrival();
    this.tl = new trendline();
    this.det = new detector();
    this.rate = new aimd();
    this._signal = 0;
    this.acked = [];
    this.ackedSum = 0;
    this.ackedNewest = 0;
    this._ackedRate = 0;
    this.lossRecv = 0;
    this.lossLost = 0;
    this.lossRuns = 0;
    this.lossPrevLost = false;
    this.lossStart = 0;
    this._loss = 0;
    this._burst = 0;
    this.lossBound = init.lossBound ?? 0;
    this.lossGrow = false;
    this.lastLossCut = 0;
    this.lastLossUpdate = 0;
    this.lastOveruse = 0;
    this.qWinStart = 0;
    this.qWinMin = 0;
    this.qMins = [0, 0, 0];
    this.qMin = 0;
    this.lastStand = 0;
    this.standCuts = 0;
    this.owdBuckets = [0, 0, 0];
    this.owdAt = 0;
    this.owdHave = [false, false, false];
    this._queueDelay = 0;
    this.lastFastCut = 0;
    this.lastSent = 0;
    this.lastSilence = 0;
    this.preSilence = 0;
    this.probeEst = 0;
    this.probeAt = 0;
    this.silent = false;
    this.silentAt = 0;
    this.cutSeq = 0;
    this.recovering = false;
    this.drainedAt = 0;
    this.fastCuts = 0;
    this.silenceCuts = 0;
    this.silenceUndone = 0;
    this.srtt = 0;
    this.minRTT = 0;
    this.minRTTAt = 0;
    this.rttBuckets = [0, 0, 0];
    this.lastReport = 0;
    this.probes = [];
    this.nextProbeID = 0;
    this.probeQueue = [];
    this.nextProbeAt = 0;
    this.lastProbeAt = 0;
    this._target = init.target ?? 0;
    this.reports = 0;
    this.overuses = 0;
    this.lostTotal = 0;
    this.recvTotal = 0;
    this.probesSent = 0;
    this.probesWon = 0;
  }

  target() { return this._target; }
  rtt() { return this.srtt; }
  loss() { return this._loss; }
  burst() { return this._burst; }
  signal() { return this._signal; }
  ackedRate() { return Math.trunc(this._ackedRate); }
  threshold() { return this.det.threshold; }
  queueDelay() { return this._queueDelay; }

  onSent(tseq, size, now, probeID) {
    let s = tseq;
    if (this.haveSent) {
      s = near16(tseq, this.lastSeq);
    } else {
      this.finalized = s;
      this.lastSeq = s;
      this.haveSent = true;
    }
    this.lastSeq = Math.max(this.lastSeq, s);
    this.lastSent = now;
    this.hist[s % historySlots] = new sentPacket({ seq: s, sendTime: now, size, probe: probeID });
    if (probeID > 0) {
      const p = this.probeByID(probeID);
      if (p !== null) {
        if (p.sentPkts === 0) p.firstSend = now;
        p.sentPkts++;
        p.sentBytes += size;
        p.lastSend = now;
        p.lastSize = size;
      }
    }
  }

  probeByID(id) {
    for (const p of this.probes) if (p.id === id) return p;
    return null;
  }

  onReport(r, now) {
    if (!this.haveSent) return;
    this.reports++;
    const base = near16(r.base, this.lastSeq);
    let ref = r.refTime;
    if (this.haveRef) ref = near32(r.refTime, this.refTicks);
    this.haveRef = true;
    this.refTicks = ref;
    let newest = -1;
    let newestOwd = 0;
    let haveOwd = false;
    let newestSend = 0;
    for (let i = 0; i < r.received.length; i++) {
      const ok = r.received[i];
      const s = base + i;
      if (s > this.lastSeq) break;
      const h = this.hist[s % historySlots];
      if (h.seq !== s || !ok) continue;
      newest = s;
      if (h.state !== pktPending) continue;
      h.state = pktReceived;
      h.arrival = (ref + r.arrival[i]) * TickUs;
      this.onArrival(h, now);
      newestOwd = h.arrival - h.sendTime;
      haveOwd = true;
      newestSend = h.sendTime;
      this.trackOwd(newestOwd, now);
    }
    if (base - this.finalized > historySlots) this.finalized = base - historySlots;
    for (; this.finalized < base && this.finalized <= this.lastSeq; this.finalized++) {
      const h = this.hist[this.finalized % historySlots];
      if (h.seq !== this.finalized) continue;
      if (h.state === pktPending) h.state = pktLost;
      if (h.seq > this.cutSeq) this.countOutcome(h.state === pktLost);
    }
    if (newest >= 0) {
      const h = this.hist[newest % historySlots];
      const rtt = now - h.sendTime - r.delay * TickUs;
      if (rtt > 0) {
        if (this.srtt === 0) this.srtt = rtt;
        else this.srtt = div(7 * this.srtt + rtt, 8);
        this.trackMinRTT(rtt, now);
      }
    }
    if (haveOwd) {
      this._queueDelay = Math.max(newestOwd - this.minOwd(), 0);
      if (this.silent && newestSend >= this.silentAt - ReportInterval) {
        this.silent = false;
        if (this._queueDelay < queueEvidenceUs) {
          this.rate.rate = Math.max(this.rate.rate, this.preSilence);
          this.silenceUndone++;
        }
      }
      this.trackQueue(now);
      this.fastCut(now);
      this.standingCut(now);
      if (this._queueDelay > drainedUs) this.drainedAt = 0;
      else if (this.drainedAt === 0) this.drainedAt = now;
    }
    this.updateLoss(now);
    let queueing = 0;
    if (this.srtt > 0 && this.minRTT > 0) queueing = Math.max(this.srtt - this.minRTT, 0);
    let floor = 0.0;
    if (this._queueDelay < fastCutQueueUs) floor = probeFloor * this.probeEstimate(now);
    this.rate.update(this._signal, this._ackedRate, floor, this.srtt, queueing, now, this.cfg.minBps, this.cfg.maxBps);
    if (this._signal === SignalOver) {
      this.lastOveruse = now;
      this.overuses++;
    }
    this.evaluateProbes(now);
    this.updateTarget(now);
    this.lastReport = now;
  }

  onArrival(h, now) {
    this.acked.push({ at: h.arrival, size: h.size });
    this.ackedSum += h.size;
    this.ackedNewest = Math.max(this.ackedNewest, h.arrival);
    const newest = this.ackedNewest;
    let drop = 0;
    while (drop < this.acked.length && this.acked[drop].at < newest - ackedWindowUs) {
      this.ackedSum -= this.acked[drop].size;
      drop++;
    }
    if (drop > 0) this.acked.splice(0, drop);
    if (this.acked.length >= 2) {
      const span = newest - this.acked[0].at;
      if (span >= 100_000) this._ackedRate = (this.ackedSum * 8e6) / Math.max(span, ackedWindowUs / 2);
    }
    if (h.probe > 0) {
      const p = this.probeByID(h.probe);
      if (p !== null) {
        if (p.recvPkts === 0 || h.arrival < p.firstArr) {
          p.firstArr = h.arrival;
          p.firstArrSize = h.size;
        }
        if (h.arrival > p.lastArr || p.recvPkts === 0) p.lastArr = h.arrival;
        p.recvPkts++;
        p.recvBytes += h.size;
      }
    }
    const [sd, ad, ok] = this.ia.add(h.sendTime, h.arrival);
    if (!ok) return;
    const trend = this.tl.update(sd, ad, h.arrival);
    this._signal = this.det.detect(trend, sd / 1000, this.tl.count, now);
  }

  trackOwd(d, now) {
    if (this.owdAt === 0 || now - this.owdAt >= minRTTWindowUs) {
      const b0 = this.owdBuckets[0];
      this.owdBuckets[0] = this.owdBuckets[2];
      this.owdBuckets[1] = b0;
      const h0 = this.owdHave[0];
      this.owdHave[0] = this.owdHave[2];
      this.owdHave[1] = h0;
      this.owdBuckets[2] = d;
      this.owdHave[2] = true;
      this.owdAt = now;
    }
    this.owdBuckets[2] = Math.min(this.owdBuckets[2], d);
  }

  minOwd() {
    let m = this.owdBuckets[2];
    for (let i = 0; i < 2; i++) if (this.owdHave[i]) m = Math.min(m, this.owdBuckets[i]);
    return m;
  }

  trackQueue(now) {
    if (this.qWinStart === 0) {
      this.qWinStart = now;
      this.qWinMin = this._queueDelay;
      return;
    }
    this.qWinMin = Math.min(this.qWinMin, this._queueDelay);
    if (now - this.qWinStart < standWindowUs) return;
    this.qMins = [this.qMins[1], this.qMins[2], this.qWinMin];
    this.qWinStart = now;
    this.qWinMin = this._queueDelay;
    this.qMin = 0;
    if (this.qMins[2] >= standingQueueUs && this.qMins[2] >= this.qMins[1] + standRiseUs && this.qMins[1] >= this.qMins[0] + standRiseUs) {
      this.qMin = this.qMins[2];
    }
  }

  standingCut(now) {
    if (this.qMin < standingQueueUs || now - this.lastFastCut < Math.max(this.srtt, standWindowUs) || now - this.lastStand < Math.max(this.srtt, standWindowUs)) return;
    let base = this.recentRate();
    if (base <= 0) base = this._ackedRate;
    const b = Math.min(Math.max(beta - (drainBeta * this.qMin) / 1e6, betaMin), beta);
    const nr = b * base;
    if (nr < this.rate.rate) {
      this.rate.rate = Math.max(nr, this.cfg.minBps);
      this.rate.state = stateHold;
      this.rate.lastDecrease = now;
    }
    this.lastStand = now;
    this.lastOveruse = now;
    this.standCuts++;
  }

  recentRate() {
    if (this.acked.length < 2) return 0;
    const newest = this.ackedNewest;
    let sum = 0;
    let oldest = newest;
    for (let i = this.acked.length - 1; i >= 0 && this.acked[i].at >= newest - recentWindowUs; i--) {
      sum += this.acked[i].size;
      oldest = this.acked[i].at;
    }
    const span = newest - oldest;
    if (span < recentWindowUs / 2) return 0;
    return (sum * 8e6) / span;
  }

  fastCut(now) {
    if (this._queueDelay < fastCutQueueUs || now - this.lastFastCut < Math.max(this.srtt, 100_000)) return;
    const rr = this.recentRate();
    if (rr <= 0) return;
    const b = Math.min(Math.max(beta - (drainBeta * this._queueDelay) / 1e6, betaMin), beta);
    const nr = b * rr;
    if (nr >= this.rate.rate) return;
    this.rate.rate = Math.max(nr, this.cfg.minBps);
    this.rate.state = stateHold;
    this.rate.lastDecrease = now;
    this.lastFastCut = now;
    this.lastOveruse = now;
    this.fastCuts++;
    this.forgetLoss(now);
  }

  forgetLoss(now) {
    this.cutSeq = this.lastSeq;
    this.lossRecv = 0;
    this.lossLost = 0;
    this.lossRuns = 0;
    this.lossStart = now;
    this.recovering = true;
    this.drainedAt = 0;
  }

  checkFeedback(now) {
    if (this.lastReport === 0 || this.lastSent <= this.lastReport) return;
    const limit = Math.max(silenceReports * ReportInterval, 2 * this.minRTT);
    if (now - this.lastReport < limit || now - this.lastSilence < silenceStepUs) return;
    if (!this.silent) {
      this.silent = true;
      this.silentAt = now;
      this.preSilence = this.rate.rate;
      this.forgetLoss(now);
    }
    this.lastSilence = now;
    this.lastOveruse = now;
    this.rate.rate = Math.max(this.rate.rate * silenceFactor, this.cfg.minBps);
    this.rate.state = stateHold;
    this.silenceCuts++;
    this.updateTarget(now);
  }

  trackMinRTT(rtt, now) {
    if (this.minRTTAt === 0 || now - this.minRTTAt >= minRTTWindowUs) {
      const b0 = this.rttBuckets[0];
      this.rttBuckets[0] = this.rttBuckets[2];
      this.rttBuckets[1] = b0;
      this.rttBuckets[2] = rtt;
      this.minRTTAt = now;
    }
    this.rttBuckets[2] = Math.min(this.rttBuckets[2], rtt);
    this.minRTT = this.rttBuckets[2];
    for (let i = 0; i < 2; i++) {
      const b = this.rttBuckets[i];
      if (b > 0) this.minRTT = Math.min(this.minRTT, b);
    }
  }

  countOutcome(lost) {
    if (lost) {
      this.lossLost++;
      this.lostTotal++;
      if (!this.lossPrevLost) this.lossRuns++;
    } else {
      this.lossRecv++;
      this.recvTotal++;
    }
    this.lossPrevLost = lost;
  }

  congested(now) { return this.queueEvidence(now); }

  queueEvidence(now) {
    if (this.lastOveruse > 0 && now - this.lastOveruse < overuseMemoryUs) return true;
    return this.srtt > 0 && this.minRTT > 0 && this.srtt - this.minRTT > queueEvidenceUs;
  }

  updateLoss(now) {
    if (this.lossStart === 0) this.lossStart = now;
    const total = this.lossRecv + this.lossLost;
    if (total < lossWindowPkts && (now - this.lossStart < lossWindowUs || total < 20)) {
      if (this.lossGrow && this.lastLossUpdate > 0) {
        const dt = (now - this.lastReport) / 1e6;
        this.lossBound = Math.min(this.lossBound * Math.pow(1.08, dt), this.cfg.maxBps);
      }
      return;
    }
    const frac = this.lossLost / total;
    this._loss = frac;
    this._burst = 1;
    if (this.lossRuns > 0) this._burst = this.lossLost / this.lossRuns;
    this.lossRecv = 0;
    this.lossLost = 0;
    this.lossRuns = 0;
    this.lossStart = now;
    this.lastLossUpdate = now;
    let high = lossHigh;
    if (!this.queueEvidence(now)) high = lossHighRandom;
    this.lossGrow = frac < lossLow || (frac <= high && high === lossHighRandom);
    if (frac > high) {
      if (now - this.lastLossCut >= 300_000 + this.srtt) {
        this.lossBound = Math.min(this.lossBound, this._target) * (1 - 0.5 * frac);
        this.lastLossCut = now;
      }
    } else if (frac < lossLow) {
      this.lossBound = Math.max(this.lossBound, 1.08 * this._target);
    }
    this.lossBound = Math.min(Math.max(this.lossBound, this.cfg.minBps), this.cfg.maxBps);
  }

  updateTarget(now) {
    const t = Math.min(this.rate.rate, this.lossBound);
    this._target = Math.trunc(Math.min(Math.max(t, this.cfg.minBps), this.cfg.maxBps));
  }

  probeEstimate(now) {
    if (this.probeAt === 0 || now - this.probeAt > probeMemoryUs || this.lastFastCut > this.probeAt || this.lastSilence > this.probeAt) return 0;
    return Math.trunc(Math.min(this.probeEst, this.cfg.maxBps));
  }

  requestProbe(now) {
    if (this._target >= this.cfg.maxBps || now - this.lastProbeAt < requestGapUs || now - this.lastOveruse < requestQuietUs || this.queueEvidence(now)) return;
    if (this.nextProbeAt === 0 || this.nextProbeAt > now) this.nextProbeAt = now;
  }

  startProbe(now) {
    for (const p of this.probes) if (!p.ended) return null;
    let r;
    if (this.recovering && this.drainedAt > 0 && now - this.drainedAt >= recoveryProbeUs) {
      r = Math.trunc(probeMaxFactor * this._target);
      this.recovering = false;
    } else if (this.probeQueue.length > 0) {
      r = this.probeQueue[0];
      this.probeQueue = this.probeQueue.slice(1);
    } else if (this.nextProbeAt > 0 && now >= this.nextProbeAt) {
      r = Math.trunc(probeMaxFactor * this._target);
      this.nextProbeAt = 0;
    } else if (this.lastProbeAt > 0 && now - this.lastProbeAt >= probeInterval && now - this.lastOveruse >= probeQuiet && (this._loss < lossLow || (this.lossGrow && !this.queueEvidence(now)))) {
      r = Math.trunc(probeMaxFactor * this._target);
    } else {
      return null;
    }
    if (this._target >= this.cfg.maxBps) {
      this.lastProbeAt = now;
      return null;
    }
    r = Math.min(r, Math.trunc(probeMaxFactor * this.cfg.maxBps));
    this.nextProbeID++;
    const p = new probe({ id: this.nextProbeID, rate: r, bytes: Math.max(div(r * probeDurationUs, 8_000_000), probeMinPackets * probePacket(r)) });
    this.probes.push(p);
    this.lastProbeAt = now;
    this.probesSent++;
    return [p.id, p.rate, p.bytes];
  }

  endProbe(id, now) {
    const p = this.probeByID(id);
    if (p !== null && !p.ended) {
      p.ended = true;
      p.endAt = now;
    }
  }

  evaluateProbes(now) {
    const keep = [];
    for (const p of this.probes) {
      if (!p.ended) {
        keep.push(p);
        continue;
      }
      const wait = Math.max(probeEvalDelayUs, 2 * this.srtt);
      if (p.recvPkts < p.sentPkts && now - p.endAt < wait) {
        keep.push(p);
        continue;
      }
      this.finishProbe(p, now);
    }
    this.probes = keep;
  }

  finishProbe(p, now) {
    if (p.sentPkts < probeMinPackets || p.recvPkts < 0.8 * p.sentPkts || p.lastArr <= p.firstArr) return;
    const recv = ((p.recvBytes - p.firstArrSize) * 8e6) / (p.lastArr - p.firstArr);
    let send = p.rate;
    if (p.lastSend > p.firstSend) send = ((p.sentBytes - p.lastSize) * 8e6) / (p.lastSend - p.firstSend);
    let est = Math.min(recv, send);
    if (recv < 0.9 * send) est = 0.95 * recv;
    if (est > this._target) {
      this.probeEst = est;
      this.probeAt = now;
      this.probesWon++;
      est = Math.min(est, this.cfg.maxBps);
      if (est > this.rate.rate) {
        this.rate.rate = est;
        this.rate.haveCap = false;
      }
      this.lossBound = Math.max(this.lossBound, est);
      this.updateTarget(now);
      if (est >= 0.7 * p.rate && this._target < this.cfg.maxBps) this.nextProbeAt = now + probeFollowUp;
    }
  }
}

export function newController(cfg) {
  const c = new Config(cfg ?? {}).withDefaults();
  const ctrl = new Controller({
    cfg: c,
    hist: Array.from({ length: historySlots }, () => new sentPacket()),
    lossBound: c.maxBps,
    target: c.startBps,
  });
  ctrl.det.threshold = thresholdInit;
  ctrl.det.overTime = -1;
  ctrl.rate.rate = c.startBps;
  ctrl.probeQueue = [3 * c.startBps, 6 * c.startBps];
  return ctrl;
}

export function probePacket(rate) {
  return Math.min(Math.max(div(div(rate * lowProbeUs, 8_000_000), probeMinPackets), lowProbePacket), minProbePacket);
}

class fifo {
  constructor() {
    this.items = [];
    this.head = 0;
  }

  push(q) { this.items.push(q); }

  empty() { return this.head >= this.items.length; }

  peek() { return this.items[this.head]; }

  pop() {
    const q = this.items[this.head];
    this.items[this.head] = null;
    this.head++;
    if (this.head > 64 && this.head * 2 > this.items.length) {
      this.items = this.items.slice(this.head);
      this.head = 0;
    }
    return q;
  }
}

export class Pacer {
  constructor(init = {}) {
    this.q = Array.from({ length: classes }, () => new fifo());
    this.rate = init.rate ?? 0;
    this.budget = init.budget ?? 0;
    this.last = init.last ?? 0;
    this._queuedBytes = 0;
    this.probeRate = 0;
    this.probeLeft = 0;
    this.probeSize = 0;
    this.probeSent = 0;
    this.drainCap = 0;
    this.dropped = init.dropped ?? 0;
  }

  setDrainCap(bps) { this.drainCap = bps; }

  dropOlder(ageUs, now) {
    for (const c of [ClassRTX, ClassVideo]) {
      while (!this.q[c].empty() && now - this.q[c].peek().at > ageUs) {
        const d = this.q[c].pop();
        this._queuedBytes -= d.data.length;
        this.dropped++;
      }
    }
  }

  setRate(bps) { this.rate = bps; }

  setProbe(bps, bytes) {
    this.probeRate = bps;
    this.probeLeft = bytes;
    this.probeSize = div(bytes, probeMinPackets);
    this.probeSent = 0;
  }

  probing() { return this.probeLeft > 0; }

  queuedBytes() { return this._queuedBytes; }

  oldestUs(now) {
    let o = 0;
    for (let i = 0; i < classes; i++) if (!this.q[i].empty()) o = Math.max(o, now - this.q[i].peek().at);
    return o;
  }

  push(cls, b, now) {
    cls = Math.min(Math.max(cls, 0), classes - 1);
    this.q[cls].push({ data: b, at: now });
    this._queuedBytes += b.length;
    while (this._queuedBytes > maxQueueBytes && !this.q[ClassVideo].empty()) {
      const d = this.q[ClassVideo].pop();
      this._queuedBytes -= d.data.length;
      this.dropped++;
    }
  }

  tick(now, send, pad) {
    if (this.last === 0) this.last = now;
    const dt = now - this.last;
    this.last = now;
    let rate = this.rate;
    if (this.probeLeft > 0) rate = Math.max(rate, this.probeRate);
    for (const c of [ClassRTX, ClassVideo]) {
      while (!this.q[c].empty() && now - this.q[c].peek().at > MaxVideoQueueUs) {
        const d = this.q[c].pop();
        this._queuedBytes -= d.data.length;
        this.dropped++;
      }
    }
    let drain = (this._queuedBytes * 8e6) / drainTargetUs;
    if (this.drainCap > 0) drain = Math.min(drain, Math.max(this.drainCap, rate));
    rate = Math.max(rate, drain);
    this.budget += (rate * dt) / 8e6;
    this.budget = Math.min(this.budget, (rate * maxBurstUs) / 8e6 + PaddingSize);
    while (this.budget > 0) {
      let item = null;
      for (let c = 0; c < classes; c++) {
        if (!this.q[c].empty()) {
          item = this.q[c].pop();
          break;
        }
      }
      if (item === null || item.data == null) break;
      this._queuedBytes -= item.data.length;
      this.budget -= item.data.length;
      const probing = this.probeLeft > 0;
      if (probing) {
        this.probeSent++;
        this.probeLeft = Math.max(this.probeLeft - item.data.length, (probeMinPackets - this.probeSent) * lowProbePacket);
      }
      send(item.data, probing);
    }
    for (let n = 0; this.probeLeft > 0 && this.budget > 0 && pad != null && n < maxProbeBudget; n++) {
      const b = pad(Math.min(Math.max(Math.min(this.probeSize, this.probeLeft), lowProbePacket), PaddingSize));
      this.budget -= b.length;
      this.probeSent++;
      this.probeLeft = Math.max(this.probeLeft - b.length, (probeMinPackets - this.probeSent) * lowProbePacket);
      send(b, true);
    }
    if (this.probeLeft <= 0) this.probeLeft = 0;
    while (!this.q[ClassAudio].empty() && this.budget > -PaddingSize) {
      const item = this.q[ClassAudio].pop();
      this._queuedBytes -= item.data.length;
      this.budget -= item.data.length;
      send(item.data, false);
    }
  }
}
