export const StatusWaiting = 0;
export const StatusPlay = 1;
export const StatusConceal = 2;
export const StatusInsert = 3;
export const StatusDRED = 4;

export const FrameUs = 20_000;

const minWindowUs = 10_000_000;
const maxBuffered = 150;
const resetAheadUs = 3_000_000;
const flushOverUs = 200_000;
const decayPerSec = 100_000;
const runWindowUs = 15_000_000;
const maxRuns = 64;
const minLossRuns = 2;
const settleUs = 200_000;
const lookAheadGoal = 0.005;
const lookAheadStepUs = 10_000;

const maxPending = 256;
const maxBroken = 1024;
const maxHoles = 64;

const div = (a, b) => Math.trunc(a / b);
const byNumber = (a, b) => a - b;

export class AudioConfig {
  constructor(init = {}) {
    this.minDelayUs = init.minDelayUs ?? 0;
    this.maxDelayUs = init.maxDelayUs ?? 0;
    this.startDelayUs = init.startDelayUs ?? 0;
    this.percentile = init.percentile ?? 0;
    this.windowUs = init.windowUs ?? 0;
    this.marginUs = init.marginUs ?? 0;
    this.hysteresisUs = init.hysteresisUs ?? 0;
    this.adjustGapUs = init.adjustGapUs ?? 0;
    this.maxExtraUs = init.maxExtraUs ?? 0;
    this.noLookAhead = init.noLookAhead ?? false;
  }

  withDefaults() {
    const c = new AudioConfig(this);
    if (c.minDelayUs === 0) c.minDelayUs = 20_000;
    if (c.maxDelayUs === 0) c.maxDelayUs = 400_000;
    if (c.startDelayUs === 0) c.startDelayUs = 60_000;
    if (c.percentile === 0) c.percentile = 0.98;
    if (c.windowUs === 0) c.windowUs = 3_000_000;
    if (c.marginUs === 0) c.marginUs = 5_000;
    if (c.hysteresisUs === 0) c.hysteresisUs = 15_000;
    if (c.adjustGapUs === 0) c.adjustGapUs = 100_000;
    if (c.maxExtraUs === 0) c.maxExtraUs = 100_000;
    return c;
  }
}

export class AudioOut {
  constructor(init = {}) {
    this.status = init.status ?? 0;
    this.data = init.data ?? null;
    this.offset = init.offset ?? 0;
  }
}

export class AudioStats {
  constructor(init = {}) {
    this.played = init.played ?? 0;
    this.concealed = init.concealed ?? 0;
    this.fec = init.fec ?? 0;
    this.dred = init.dred ?? 0;
    this.inserted = init.inserted ?? 0;
    this.dropped = init.dropped ?? 0;
    this.late = init.late ?? 0;
    this.early = init.early ?? 0;
  }
}

function pushMinQ(q, at, delay) {
  while (q.length > 0 && q[q.length - 1].delay >= delay) q.pop();
  q.push({ at, delay });
}

function cutSamples(samples, before) {
  let cut = 0;
  while (cut < samples.length && samples[cut].at < before) cut++;
  if (cut > 0) samples.splice(0, cut);
}

function percentileOf(samples, percentile) {
  const d = samples.map((s) => s.delay).sort(byNumber);
  return d[Math.min(d.length - 1, Math.trunc(percentile * d.length))];
}

export class Audio {
  constructor(init = {}) {
    this.cfg = init.cfg ?? new AudioConfig().withDefaults();
    this.started = init.started ?? false;
    this.playing = init.playing ?? false;
    this.firstID = init.firstID ?? 0;
    this.firstTS = init.firstTS ?? 0;
    this.next = init.next ?? 0;
    this.frames = init.frames ?? new Map();
    this.newest = init.newest ?? 0;
    this.arrived = init.arrived ?? 0;
    this.runs = init.runs ?? [];
    this.depth = init.depth ?? 0;
    this.extra = init.extra ?? 0;
    this.base = init.base ?? 0;
    this.samples = init.samples ?? [];
    this.minQ = init.minQ ?? [];
    this.target = init.target ?? 0;
    this.lastTarget = init.lastTarget ?? 0;
    this.lastAdjust = init.lastAdjust ?? 0;
    this.stats = init.stats ?? new AudioStats();
  }

  targetUs() {
    return this.target;
  }

  tsOf(id) {
    return this.firstTS + (id - this.firstID) * FrameUs;
  }

  extraUs() {
    return this.extra;
  }

  push(id, ts, data, arrival) {
    this.pushDepth(id, ts, data, 0, arrival);
  }

  pushDepth(id, ts, data, depthUs, arrival) {
    if (!this.started) {
      this.started = true;
      this.firstID = id;
      this.firstTS = ts;
      this.next = id;
      this.newest = id;
      this.arrived = id;
    }
    if ((id - this.next) * FrameUs > resetAheadUs) {
      this.stats.early++;
      this.frames.clear();
      this.started = true;
      this.playing = false;
      this.firstID = id;
      this.firstTS = ts;
      this.next = id;
      this.newest = id;
      this.arrived = id;
      this.samples.length = 0;
      this.minQ.length = 0;
      this.runs.length = 0;
    }
    if (this.frames.has(id)) return;
    this.sampleDelay(arrival - this.tsOf(id), arrival);
    this.depth = depthUs;
    this.sampleLoss(id, arrival);
    if (id < this.next) {
      this.stats.late++;
      return;
    }
    this.frames.set(id, { data, depth: depthUs });
    this.newest = Math.max(this.newest, id);
    while (this.frames.size > maxBuffered) {
      this.frames.delete(this.next);
      this.next++;
      this.stats.dropped++;
    }
  }

  sampleDelay(d, arrival) {
    this.samples.push({ at: arrival, delay: d });
    pushMinQ(this.minQ, arrival, d);
    cutSamples(this.samples, arrival - this.cfg.windowUs);
    while (this.minQ.length > 1 && this.minQ[0].at < arrival - minWindowUs) this.minQ.shift();
  }

  sampleLoss(id, arrival) {
    if (id > this.arrived + 1) {
      const r = { at: arrival, start: this.arrived + 1, n: id - this.arrived - 1, need: -1 };
      if (this.depth >= r.n * FrameUs || r.n === 1) {
        r.need = arrival - (this.tsOf(r.start) + this.dmin() + this.base);
      }
      this.runs.push(r);
    }
    if (id < this.arrived) {
      for (let i = this.runs.length - 1; i >= 0; i--) {
        const r = this.runs[i];
        if (id < r.start || id >= r.start + r.n) continue;
        if (r.n === 1) {
          this.runs.splice(i, 1);
        } else if (id === r.start) {
          r.start++;
          r.n--;
        } else if (id === r.start + r.n - 1) {
          r.n--;
        } else {
          const tail = { at: r.at, start: id + 1, n: r.start + r.n - id - 1, need: 0 };
          r.n = id - r.start;
          this.runs.splice(i + 1, 0, tail);
        }
        break;
      }
    }
    this.arrived = Math.max(this.arrived, id);
    let cut = 0;
    while (cut < this.runs.length && (this.runs[cut].at < arrival - runWindowUs || this.runs.length - cut > maxRuns)) cut++;
    if (cut > 0) this.runs.splice(0, cut);
  }

  lookAhead(now) {
    if (this.cfg.noLookAhead || this.runs.length < minLossRuns) return 0;
    const settled = [];
    for (const r of this.runs) {
      if (r.at <= now - settleUs) settled.push(r);
    }
    if (settled.length < minLossRuns) return 0;
    const span = this.arrived - this.runs[0].start + 1;
    if (span <= 0) return 0;
    const goal = lookAheadGoal * span;
    const unrecovered = (e) => {
      let n = 0;
      for (const r of settled) {
        if (r.need < 0) continue;
        n += Math.min(Math.max(div(r.need - e + FrameUs - 1, FrameUs), 0), r.n);
      }
      return n;
    };
    let limit = this.cfg.maxExtraUs;
    if (this.depth > 0) limit = Math.min(limit, this.depth);
    else limit = Math.min(limit, FrameUs);
    for (let e = 0; e <= limit; e += lookAheadStepUs) {
      if (unrecovered(e) <= goal) return e;
    }
    return limit;
  }

  dmin() {
    return this.minQ[0].delay;
  }

  updateTarget(now) {
    if (this.samples.length === 0) return;
    const p = percentileOf(this.samples, this.cfg.percentile);
    this.extra = this.lookAhead(now);
    this.base = Math.min(Math.max(p - this.dmin() + this.cfg.marginUs, this.cfg.minDelayUs), this.cfg.maxDelayUs);
    const want = Math.min(Math.max(p - this.dmin() + this.cfg.marginUs + this.extra, this.cfg.minDelayUs), this.cfg.maxDelayUs);
    if (want >= this.target) {
      this.target = want;
    } else if (this.lastTarget > 0) {
      const step = div(decayPerSec * (now - this.lastTarget), 1_000_000);
      this.target = Math.max(want, this.target - step);
    }
    this.lastTarget = now;
  }

  pull(now) {
    const o = this.pullOut(now);
    return [o.data, o.status];
  }

  pullOut(now) {
    if (!this.started) return new AudioOut({ status: StatusWaiting });
    this.updateTarget(now);
    let lag = now - (this.tsOf(this.next) + this.dmin());
    if (!this.playing) {
      if (lag < this.target) return new AudioOut({ status: StatusWaiting });
      this.playing = true;
      this.lastAdjust = now;
    }
    if (lag > this.target + flushOverUs) {
      while (lag > this.target && this.next < this.newest) {
        this.frames.delete(this.next);
        this.next++;
        this.stats.dropped++;
        lag -= FrameUs;
      }
    } else if (lag > this.target + this.cfg.hysteresisUs && now - this.lastAdjust >= this.cfg.adjustGapUs && this.next < this.newest) {
      this.frames.delete(this.next);
      this.next++;
      this.stats.dropped++;
      this.lastAdjust = now;
    }
    const f = this.frames.get(this.next);
    const ok = f !== undefined;
    if (!ok && lag < this.target - this.cfg.hysteresisUs && now - this.lastAdjust >= div(this.cfg.adjustGapUs, 2) && this.recoverFrom(this.next) < 0) {
      this.lastAdjust = now;
      this.stats.inserted++;
      return new AudioOut({ status: StatusInsert });
    }
    if (ok && lag < this.target - this.cfg.hysteresisUs && now - this.lastAdjust >= this.cfg.adjustGapUs) {
      this.lastAdjust = now;
      this.stats.inserted++;
      return new AudioOut({ status: StatusInsert });
    }
    const id = this.next;
    this.frames.delete(id);
    this.next++;
    if (ok) {
      this.stats.played++;
      return new AudioOut({ status: StatusPlay, data: f.data });
    }
    const m = this.recoverFrom(id);
    if (m >= 0) {
      this.stats.dred++;
      return new AudioOut({ status: StatusDRED, data: this.frames.get(m).data, offset: m - id });
    }
    const n = this.frames.get(id + 1);
    if (n !== undefined) {
      this.stats.fec++;
      this.stats.concealed++;
      return new AudioOut({ status: StatusConceal, data: n.data });
    }
    this.stats.concealed++;
    return new AudioOut({ status: StatusConceal });
  }

  recoverFrom(id) {
    for (let m = id + 1; m <= this.newest && (m - id) * FrameUs <= this.cfg.maxDelayUs * 4; m++) {
      const f = this.frames.get(m);
      if (f !== undefined && f.depth >= (m - id) * FrameUs) return m;
    }
    return -1;
  }
}

export function newAudio(cfg = new AudioConfig()) {
  const c = new AudioConfig(cfg).withDefaults();
  return new Audio({ cfg: c, target: c.startDelayUs });
}

export class VideoConfig {
  constructor(init = {}) {
    this.minWaitUs = init.minWaitUs ?? 0;
    this.maxWaitUs = init.maxWaitUs ?? 0;
    this.keyframeIntervalUs = init.keyframeIntervalUs ?? 0;
    this.refWaitUs = init.refWaitUs ?? 0;
    this.percentile = init.percentile ?? 0;
    this.windowUs = init.windowUs ?? 0;
    this.recoveryIntervalUs = init.recoveryIntervalUs ?? 0;
    this.escalateUs = init.escalateUs ?? 0;
    this.noRecovery = init.noRecovery ?? false;
  }

  withDefaults() {
    const c = new VideoConfig(this);
    if (c.minWaitUs === 0) c.minWaitUs = 30_000;
    if (c.maxWaitUs === 0) c.maxWaitUs = 300_000;
    if (c.keyframeIntervalUs === 0) c.keyframeIntervalUs = 500_000;
    if (c.refWaitUs === 0) c.refWaitUs = 1_000_000;
    if (c.percentile === 0) c.percentile = 0.95;
    if (c.windowUs === 0) c.windowUs = 3_000_000;
    if (c.recoveryIntervalUs === 0) c.recoveryIntervalUs = 250_000;
    if (c.escalateUs === 0) c.escalateUs = 600_000;
    return c;
  }
}

export class VideoStats {
  constructor(init = {}) {
    this.released = init.released ?? 0;
    this.undecodable = init.undecodable ?? 0;
    this.late = init.late ?? 0;
    this.gapsSkipped = init.gapsSkipped ?? 0;
    this.keyframeRequests = init.keyframeRequests ?? 0;
    this.recoveryRequests = init.recoveryRequests ?? 0;
    this.recovered = init.recovered ?? 0;
    this.escalated = init.escalated ?? 0;
  }
}

export class Video {
  constructor(init = {}) {
    this.cfg = init.cfg ?? new VideoConfig().withDefaults();
    this.pending = init.pending ?? [];
    this.started = init.started ?? false;
    this._lastID = init._lastID ?? 0;
    this.lastSeq = init.lastSeq ?? 0;
    this.holes = init.holes ?? [];
    this.decoded = init.decoded ?? new Set();
    this.decodedList = init.decodedList ?? [];
    this.broken = init.broken ?? new Set();
    this.needKey = init.needKey ?? false;
    this.lastKeyReq = init.lastKeyReq ?? 0;
    this.needRec = init.needRec ?? false;
    this.needRecAt = init.needRecAt ?? 0;
    this.lastRecReq = init.lastRecReq ?? 0;
    this.acks = init.acks ?? [];
    this.samples = init.samples ?? [];
    this.minQ = init.minQ ?? [];
    this.wait = init.wait ?? 0;
    this.stats = init.stats ?? new VideoStats();
  }

  waitUs() {
    return this.wait;
  }

  push(f, now) {
    if (this.started && f.id <= this._lastID) {
      this.stats.late++;
      return;
    }
    for (const p of this.pending) {
      if (p.id === f.id) return;
    }
    let lo = 0;
    let hi = this.pending.length;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (this.pending[mid].id < f.id) lo = mid + 1;
      else hi = mid;
    }
    this.pending.splice(lo, 0, f);
    if (this.pending.length > maxPending) this.pending.shift();
    this.sample(f);
  }

  sample(f) {
    const d = f.complete - f.timestamp;
    this.samples.push({ at: f.complete, delay: d });
    pushMinQ(this.minQ, f.complete, d);
    cutSamples(this.samples, f.complete - this.cfg.windowUs);
    while (this.minQ.length > 1 && this.minQ[0].at < f.complete - minWindowUs) this.minQ.shift();
    if (this.samples.length >= 10) {
      const p = percentileOf(this.samples, this.cfg.percentile);
      this.wait = Math.min(Math.max(p - this.minQ[0].delay + 10_000, this.cfg.minWaitUs), this.cfg.maxWaitUs);
    }
  }

  lost(l, now) {
    if (l.keyframe || l.layer === 0) this.broken.add(l.id);
  }

  markDecoded(id) {
    this.decoded.add(id);
    this.decodedList.push(id);
    if (this.decodedList.length > 512) {
      this.decoded.delete(this.decodedList[0]);
      this.decodedList.shift();
    }
  }

  advance(last) {
    this.lastSeq = Math.max(this.lastSeq, last);
    for (let changed = true; changed;) {
      changed = false;
      const keep = [];
      for (const h of this.holes) {
        if (h[0] <= this.lastSeq + 1) {
          this.lastSeq = Math.max(this.lastSeq, h[1]);
          changed = true;
        } else {
          keep.push(h);
        }
      }
      this.holes = keep;
    }
  }

  discard(f) {
    this.pending.shift();
    if (f.firstSeq <= this.lastSeq + 1) this.advance(f.lastSeq);
    else if (this.holes.length < maxHoles) this.holes.push([f.firstSeq, f.lastSeq]);
    this.stats.undecodable++;
    if (this.broken.size > maxBroken) this.broken.clear();
    this.broken.add(f.id);
  }

  release(f) {
    this.pending.shift();
    this.started = true;
    this._lastID = f.id;
    this.advance(f.lastSeq);
    this.markDecoded(f.id);
    this.stats.released++;
    if (f.layer === 0) {
      this.needKey = false;
      this.needRec = false;
    }
    if (f.longTerm && this.acks.length < 64) this.acks.push(f.id);
    return f;
  }

  chainBroken(now) {
    if (this.cfg.noRecovery || !this.started) {
      this.needKey = true;
      return;
    }
    if (!this.needRec) {
      this.needRec = true;
      this.needRecAt = now;
    }
  }

  recoveryAt() {
    for (let i = 0; i < this.pending.length; i++) {
      const p = this.pending[i];
      if (p.recovery && this.decoded.has(p.ref)) return i;
    }
    return -1;
  }

  independent(f) {
    return !f.keyframe && f.ref < 0;
  }

  pop(now) {
    while (this.pending.length > 0) {
      const f = this.pending[0];
      if (f.keyframe) {
        this.decoded.clear();
        this.decodedList.length = 0;
        this.broken.clear();
        this.holes.length = 0;
        this.lastSeq = f.firstSeq - 1;
        return this.release(f);
      }
      if (this.independent(f)) {
        if (f.firstSeq > this.lastSeq + 1) this.stats.gapsSkipped++;
        this.lastSeq = Math.max(this.lastSeq, f.firstSeq - 1);
        return this.release(f);
      }
      if (!this.started) {
        this.discard(f);
        this.needKey = true;
        continue;
      }
      const waited = now - f.complete;
      if (!this.decoded.has(f.ref)) {
        const i = this.recoveryAt();
        if (i > 0) {
          for (let k = 0; k < i; k++) this.discard(this.pending[0]);
          continue;
        }
        if (f.ref > this._lastID && !this.broken.has(f.ref)) {
          let limit = this.wait;
          if (f.layer === 0) {
            if (waited >= this.wait) this.chainBroken(now);
            limit = Math.max(this.wait, this.cfg.refWaitUs);
          }
          if (waited < limit) return null;
        }
        this.discard(f);
        if (f.layer === 0) this.chainBroken(now);
        continue;
      }
      if (f.firstSeq > this.lastSeq + 1 && !f.recovery) {
        if (waited < this.wait) return null;
        this.stats.gapsSkipped++;
      }
      if (f.recovery) {
        this.stats.recovered++;
        this.lastSeq = Math.max(this.lastSeq, f.firstSeq - 1);
      }
      return this.release(f);
    }
    return null;
  }

  takeAcks() {
    const a = this.acks;
    this.acks = [];
    return a;
  }

  lastID() {
    return this.started ? this._lastID : -1;
  }

  setTiming(rttUs) {
    this.cfg.recoveryIntervalUs = Math.min(Math.max(2 * rttUs + 100_000, 200_000), 1_000_000);
    this.cfg.escalateUs = Math.min(Math.max(3 * rttUs + 400_000, 500_000), 2_000_000);
  }

  recoveryNeeded(now) {
    if (!this.needRec || this.needKey) return [false, 0];
    if (now - this.needRecAt >= this.cfg.escalateUs) {
      this.needRec = false;
      this.needKey = true;
      this.stats.escalated++;
      return [false, 0];
    }
    if (this.lastRecReq !== 0 && now - this.lastRecReq < this.cfg.recoveryIntervalUs) return [false, 0];
    this.lastRecReq = now;
    this.stats.recoveryRequests++;
    let stuck = this._lastID + 1;
    if (this.pending.length > 0) stuck = this.pending[0].id;
    return [true, stuck];
  }

  keyframeNeeded(now) {
    if (!this.needKey) return false;
    if (this.lastKeyReq !== 0 && now - this.lastKeyReq < this.cfg.keyframeIntervalUs) return false;
    this.lastKeyReq = now;
    this.stats.keyframeRequests++;
    return true;
  }
}

export function newVideo(cfg = new VideoConfig()) {
  const c = new VideoConfig(cfg).withDefaults();
  return new Video({ cfg: c, wait: c.minWaitUs * 2 });
}
