const TWO_PI = 2 * Math.PI;
const EPS = 1e-9;
const MIN_WINDOWS = 8;
const LIMIT_CEILING = 0.95;
const MALE_HZ = 115;
const FEMALE_HZ = 205;
const FEMALE_TRACT = 1.17;
const HIST_LOW_HZ = 50;
const HIST_STEPS_PER_SEMITONE = 4;
const HIST_BINS = 160;
const HIST_HALF_LIFE = 3000;
const SEED_WEIGHT = 60;
const VOICING_ENTER = 0.55;
const VOICING_STAY = 0.45;
const ANCHOR_BIAS = 0.3;
const LOCAL_VOICING = 0.6;
const PITCH_WINDOW = 512;
const PITCH_EVERY = 2;
const PITCH_LOW_HZ = 50;
const PITCH_HIGH_HZ = 420;
const PEAK_SPAN = 0.5;
const LIFTER_PERIODS = 0.55;
const ENVELOPE_GLIDE_S = 0.006;
const VOICING_GLIDE = 0.25;
const UNVOICED_WARP = 0.4;
const WARP_KEEP_HZ = 5000;
const WARP_RELEASE_HZ = 9000;
const BREATH_LEVEL = 2;
const UNVOICED_BREATH = 0.3;
const BREATH_LOW_HZ = 800;
const BREATH_RISE_HZ = 3000;
const PULSE_DEPTH = 0.7;
const PULSE_GAIN = 1 / Math.sqrt(1 + (PULSE_DEPTH * PULSE_DEPTH) / 2);
const TREMOR_HZ = 5.5;
const VOICED_HOLD = 10;

export const VOICE_LIMITS = Object.freeze({
  ratio: [0.45, 2.6],
  formant: [0.72, 1.42],
  callerHz: [70, 330],
  tract: [0.94, 1.26],
  range: [0, 1.5],
  jitter: [0, 0.04],
  tremor: [0, 0.05],
});

const clamp = (value, low, high) => (value < low ? low : value > high ? high : value);

export function tractFromPitch(hz) {
  const f = clamp(Number(hz) || MALE_HZ, VOICE_LIMITS.callerHz[0], VOICE_LIMITS.callerHz[1]);
  const t = Math.log(f / MALE_HZ) / Math.log(FEMALE_HZ / MALE_HZ);
  return clamp(1 + t * (FEMALE_TRACT - 1), VOICE_LIMITS.tract[0], VOICE_LIMITS.tract[1]);
}

export function voicePlanInto(out, preset, callerHz) {
  if (!preset || !(preset.pitchHz > 0)) {
    out.ratio = 1;
    out.formant = 1;
    out.brightness = 0;
    out.breath = 0;
    out.range = 1;
    out.jitter = 0;
    out.tremor = 0;
    out.mix = 0;
    return out;
  }
  const caller = clamp(Number(callerHz) || MALE_HZ, VOICE_LIMITS.callerHz[0], VOICE_LIMITS.callerHz[1]);
  out.ratio = clamp(preset.pitchHz / caller, VOICE_LIMITS.ratio[0], VOICE_LIMITS.ratio[1]);
  out.formant = clamp((preset.tract || 1) / tractFromPitch(caller), VOICE_LIMITS.formant[0], VOICE_LIMITS.formant[1]);
  out.brightness = clamp(Number(preset.brightness) || 0, -1, 1);
  out.breath = clamp(Number(preset.breath) || 0, 0, 1);
  out.range = clamp(Number.isFinite(preset.range) ? preset.range : 1, VOICE_LIMITS.range[0], VOICE_LIMITS.range[1]);
  out.jitter = clamp(Number(preset.jitter) || 0, VOICE_LIMITS.jitter[0], VOICE_LIMITS.jitter[1]);
  out.tremor = clamp(Number(preset.tremor) || 0, VOICE_LIMITS.tremor[0], VOICE_LIMITS.tremor[1]);
  out.mix = 1;
  return out;
}

export function voicePlan(preset, callerHz) {
  return voicePlanInto({}, preset, callerHz);
}

export class Fft {
  constructor(size) {
    this.size = size;
    this.cos = new Float64Array(size / 2);
    this.sin = new Float64Array(size / 2);
    this.reverse = new Uint32Array(size);
    for (let i = 0; i < size / 2; i += 1) {
      this.cos[i] = Math.cos((TWO_PI * i) / size);
      this.sin[i] = Math.sin((TWO_PI * i) / size);
    }
    const bits = Math.round(Math.log2(size));
    for (let i = 0; i < size; i += 1) {
      let r = 0;
      for (let b = 0; b < bits; b += 1) r |= ((i >> b) & 1) << (bits - 1 - b);
      this.reverse[i] = r;
    }
  }

  transform(re, im, inverse) {
    const n = this.size;
    const reverse = this.reverse;
    for (let i = 0; i < n; i += 1) {
      const j = reverse[i];
      if (j > i) {
        let t = re[i];
        re[i] = re[j];
        re[j] = t;
        t = im[i];
        im[i] = im[j];
        im[j] = t;
      }
    }
    const sign = inverse ? 1 : -1;
    const cos = this.cos;
    const sin = this.sin;
    for (let len = 2; len <= n; len <<= 1) {
      const half = len >> 1;
      const step = n / len;
      for (let start = 0; start < n; start += len) {
        for (let k = 0; k < half; k += 1) {
          const wr = cos[k * step];
          const wi = sign * sin[k * step];
          const a = start + k;
          const b = a + half;
          const xr = re[b] * wr - im[b] * wi;
          const xi = re[b] * wi + im[b] * wr;
          re[b] = re[a] - xr;
          im[b] = im[a] - xi;
          re[a] += xr;
          im[a] += xi;
        }
      }
    }
    if (inverse) {
      const scale = 1 / n;
      for (let i = 0; i < n; i += 1) {
        re[i] *= scale;
        im[i] *= scale;
      }
    }
  }
}

export class RealFft {
  constructor(size) {
    this.size = size;
    this.half = size / 2;
    this.fft = new Fft(size / 2);
    this.zr = new Float64Array(size / 2);
    this.zi = new Float64Array(size / 2);
    this.wr = new Float64Array(size / 2);
    this.wi = new Float64Array(size / 2);
    for (let k = 0; k < size / 2; k += 1) {
      this.wr[k] = Math.cos((TWO_PI * k) / size);
      this.wi[k] = -Math.sin((TWO_PI * k) / size);
    }
  }

  forward(x, re, im) {
    const h = this.half;
    const zr = this.zr;
    const zi = this.zi;
    for (let i = 0; i < h; i += 1) {
      zr[i] = x[2 * i];
      zi[i] = x[2 * i + 1];
    }
    this.fft.transform(zr, zi, false);
    re[0] = zr[0] + zi[0];
    im[0] = 0;
    re[h] = zr[0] - zi[0];
    im[h] = 0;
    for (let k = 1; k < h; k += 1) {
      const ar = zr[k];
      const ai = zi[k];
      const br = zr[h - k];
      const bi = -zi[h - k];
      const er = 0.5 * (ar + br);
      const ei = 0.5 * (ai + bi);
      const or = 0.5 * (ai - bi);
      const oi = -0.5 * (ar - br);
      const wr = this.wr[k];
      const wi = this.wi[k];
      re[k] = er + wr * or - wi * oi;
      im[k] = ei + wr * oi + wi * or;
    }
  }

  inverse(re, im, x) {
    const h = this.half;
    const zr = this.zr;
    const zi = this.zi;
    for (let k = 0; k < h; k += 1) {
      const ar = re[k];
      const ai = im[k];
      const br = re[h - k];
      const bi = -im[h - k];
      const er = 0.5 * (ar + br);
      const ei = 0.5 * (ai + bi);
      const dr = 0.5 * (ar - br);
      const di = 0.5 * (ai - bi);
      const wr = this.wr[k];
      const wi = -this.wi[k];
      const or = dr * wr - di * wi;
      const oi = dr * wi + di * wr;
      zr[k] = er - oi;
      zi[k] = ei + or;
    }
    this.fft.transform(zr, zi, true);
    for (let i = 0; i < h; i += 1) {
      x[2 * i] = zr[i];
      x[2 * i + 1] = zi[i];
    }
  }
}

export class PitchDetector {
  constructor(sampleRate, size = PITCH_WINDOW) {
    this.sampleRate = sampleRate;
    this.size = size;
    this.fft = new RealFft(size * 2);
    this.buffer = new Float64Array(size * 2);
    this.re = new Float64Array(size + 1);
    this.im = new Float64Array(size + 1);
    this.minLag = Math.max(2, Math.floor(sampleRate / PITCH_HIGH_HZ));
    this.maxLag = Math.min(size - 2, Math.ceil(sampleRate / PITCH_LOW_HZ));
    this.nsdf = new Float64Array(this.maxLag + 2);
    this.peaks = new Int32Array(64);
  }

  detect(samples, previousHz = 0) {
    const n = this.size;
    const buffer = this.buffer;
    const re = this.re;
    const im = this.im;
    let mean = 0;
    for (let i = 0; i < n; i += 1) mean += samples[i];
    mean /= n;
    let energy = 0;
    for (let i = 0; i < n; i += 1) {
      const v = samples[i] - mean;
      buffer[i] = v;
      energy += v * v;
    }
    if (Math.sqrt(energy / n) < 0.004) return 0;
    buffer.fill(0, n);
    this.fft.forward(buffer, re, im);
    for (let k = 0; k <= n; k += 1) {
      re[k] = re[k] * re[k] + im[k] * im[k];
      im[k] = 0;
    }
    this.fft.inverse(re, im, buffer);
    const nsdf = this.nsdf;
    const maxLag = this.maxLag;
    let m = 2 * buffer[0];
    for (let tau = 0; tau <= maxLag + 1; tau += 1) {
      if (tau > 0) {
        const head = samples[tau - 1] - mean;
        const tail = samples[n - tau] - mean;
        m -= head * head + tail * tail;
      }
      nsdf[tau] = m > EPS ? (2 * buffer[tau]) / m : 0;
    }
    const peaks = this.peaks;
    let count = 0;
    let tau = 1;
    while (tau < maxLag && nsdf[tau] > 0) tau += 1;
    while (tau < maxLag && count < 64) {
      while (tau < maxLag && nsdf[tau] <= 0) tau += 1;
      let best = tau;
      while (tau < maxLag && nsdf[tau] > 0) {
        if (nsdf[tau] > nsdf[best]) best = tau;
        tau += 1;
      }
      if (best >= this.minLag && best < maxLag) peaks[count++] = best;
    }
    if (!count) return 0;
    let highest = 0;
    for (let i = 0; i < count; i += 1) highest = Math.max(highest, nsdf[peaks[i]]);
    const tracking = previousHz > 0;
    if (highest < (tracking ? VOICING_STAY : VOICING_ENTER)) return 0;
    let lag = peaks[0];
    for (let i = 0; i < count; i += 1) {
      if (nsdf[peaks[i]] >= 0.9 * highest) {
        lag = peaks[i];
        break;
      }
    }
    if (tracking) {
      const expected = this.sampleRate / previousHz;
      if (Math.abs(Math.log2(lag / expected)) > 0.4) {
        let near = -1;
        for (let i = 0; i < count; i += 1) {
          if (Math.abs(Math.log2(peaks[i] / expected)) < 0.2 && (near < 0 || nsdf[peaks[i]] > nsdf[near])) near = peaks[i];
        }
        if (near > 0 && nsdf[near] >= 0.8 * highest) lag = near;
      }
    }
    const a = nsdf[lag - 1];
    const b = nsdf[lag];
    const c = nsdf[lag + 1];
    const denominator = a - 2 * b + c;
    const shift = Math.abs(denominator) > EPS ? clamp((0.5 * (a - c)) / denominator, -1, 1) : 0;
    return this.sampleRate / (lag + shift);
  }
}

export class PitchShifter {
  constructor(sampleRate) {
    this.mask = 8191;
    this.maxHalf = Math.floor(sampleRate * 0.0107);
    this.unvoiced = Math.max(16, Math.floor(sampleRate * 0.005));
    this.lookahead = this.maxHalf + Math.floor(this.maxHalf / 4);
    this.delay = this.lookahead + this.maxHalf + 1;
    this.input = new Float64Array(this.mask + 1);
    this.output = new Float64Array(this.mask + 1);
    this.pulse = new Float64Array(this.mask + 1).fill(1);
    this.pulseOut = 1;
    this.smooth = new Float64Array(this.mask + 1);
    this.lpCoef = 1 - Math.exp((-TWO_PI * 900) / sampleRate);
    this.lp1 = 0;
    this.lp2 = 0;
    this.markCount = 32;
    this.markAt = new Float64Array(this.markCount);
    this.markPeriod = new Float64Array(this.markCount);
    this.markHalf = new Int32Array(this.markCount);
    this.markVoiced = new Uint8Array(this.markCount);
    this.markWrite = 0;
    this.written = 0;
    this.lastAt = 0;
    this.lastVoiced = false;
    this.next = 0;
    this.shifting = 0;
    this.range = 1;
    this.centrePeriod = sampleRate / MALE_HZ;
    this.jitter = 0;
    this.tremor = 0;
    this.tremorStep = (TWO_PI * TREMOR_HZ) / sampleRate;
    this.seed = 0x6d2b79f5;
  }

  latency() {
    return this.delay;
  }

  shape(range, centrePeriod, jitter, tremor) {
    this.range = range;
    this.centrePeriod = centrePeriod;
    this.jitter = jitter;
    this.tremor = tremor;
  }

  random() {
    let x = this.seed;
    x ^= x << 13;
    x ^= x >>> 17;
    x ^= x << 5;
    this.seed = x >>> 0;
    return this.seed / 4294967296 - 0.5;
  }

  at(index) {
    return this.input[index & this.mask];
  }

  similarity(a, b, reach, stride = 2) {
    const input = this.input;
    const mask = this.mask;
    let dot = 0;
    let ea = 0;
    let eb = 0;
    for (let j = -reach; j <= reach; j += stride) {
      const x = input[(a + j) & mask];
      const y = input[(b + j) & mask];
      dot += x * y;
      ea += x * x;
      eb += y * y;
    }
    return dot / Math.sqrt(ea * eb + 1e-18);
  }

  addMarks(period) {
    const voiced = period >= 16;
    const step = voiced ? Math.floor(period + 0.5) : this.unvoiced;
    const half = voiced ? Math.min(step, this.maxHalf) : this.unvoiced;
    const reach = Math.max(8, Math.floor(half / 2));
    const smooth = this.smooth;
    const mask = this.mask;
    for (;;) {
      const onset = voiced && !this.lastVoiced;
      const search = voiced ? Math.floor(onset ? step / 2 : step / 8) : 0;
      const lastAt = this.lastAt;
      const anchor = Math.round(lastAt);
      const guess = anchor + step;
      if (guess + search + half + 2 >= this.written) return;
      let best = guess;
      let likeness = 1;
      if (voiced) {
        let peak = 1e-12;
        for (let c = guess - search; c <= guess + search; c += 1) {
          const v = Math.abs(smooth[c & mask]);
          if (v > peak) {
            peak = v;
            if (onset) best = c;
          }
        }
        if (onset) {
          likeness = -1;
          const wobble = Math.max(1, Math.floor(step / 8));
          for (let lag = step - wobble; lag <= step + wobble; lag += 1) {
            likeness = Math.max(likeness, this.similarity(best, best - lag, reach));
          }
        } else {
          let bestScore = -Infinity;
          for (let c = guess - search; c <= guess + search; c += 1) {
            const like = this.similarity(anchor, c, reach);
            const score = like + (ANCHOR_BIAS * Math.abs(smooth[c & mask])) / peak;
            if (score > bestScore) {
              bestScore = score;
              best = c;
              likeness = like;
            }
          }
          const a = this.similarity(anchor, best - 1, reach, 1);
          const b = this.similarity(anchor, best, reach, 1);
          const c = this.similarity(anchor, best + 1, reach, 1);
          const denominator = a - 2 * b + c;
          const shift = denominator < -EPS ? clamp((0.5 * (a - c)) / denominator, -0.5, 0.5) : 0;
          best += shift + (lastAt - anchor);
        }
      }
      const periodic = voiced && likeness >= LOCAL_VOICING;
      if (voiced && !periodic) best = Math.round(lastAt) + this.unvoiced;
      const slot = this.markWrite;
      this.markAt[slot] = best;
      this.markPeriod[slot] = periodic ? (onset ? step : best - lastAt) : this.unvoiced;
      this.markHalf[slot] = periodic ? half : this.unvoiced;
      this.markVoiced[slot] = periodic ? 1 : 0;
      this.markWrite = (slot + 1) % this.markCount;
      this.lastAt = best;
      this.lastVoiced = periodic;
    }
  }

  placeGrains(ratio) {
    const input = this.input;
    const output = this.output;
    const mask = this.mask;
    while (this.next + this.lookahead < this.written) {
      const s = this.next;
      let mark = -1;
      let nearest = 0;
      for (let i = 0; i < this.markCount; i += 1) {
        if (this.markPeriod[i] <= 0) continue;
        const distance = Math.abs(this.markAt[i] - s);
        if (mark < 0 || distance < nearest) {
          mark = i;
          nearest = distance;
        }
      }
      if (mark < 0) {
        this.next += this.unvoiced;
        continue;
      }
      const voiced = this.markVoiced[mark] === 1;
      const period = this.markPeriod[mark];
      let hop = period;
      if (voiced) {
        let r = ratio;
        if (this.range !== 1) r *= Math.pow(period / this.centrePeriod, 1 - this.range);
        if (this.tremor > 0) r *= 1 + this.tremor * Math.sin(this.tremorStep * s);
        hop = period / clamp(r, VOICE_LIMITS.ratio[0] * 0.9, VOICE_LIMITS.ratio[1] * 1.1);
        if (this.jitter > 0) hop *= 1 + 2 * this.jitter * this.random();
        hop = Math.max(8, hop);
      }
      const centre = this.markAt[mark];
      let half = this.markHalf[mark];
      if (voiced && hop > 2 * half) {
        const room = Math.floor(Math.min(this.maxHalf, this.written - 3 - centre, s - (this.written - this.delay)));
        half = Math.max(half, Math.min(Math.ceil(hop / 2), room));
      }
      const gain = voiced ? Math.sqrt(hop / half) : 1;
      const offset = centre - s;
      const whole = Math.floor(offset);
      const f = offset - whole;
      const f2 = f * f;
      const f3 = f2 * f;
      const c0 = gain * (-0.5 * f3 + f2 - 0.5 * f);
      const c1 = gain * (1.5 * f3 - 2.5 * f2 + 1);
      const c2 = gain * (-1.5 * f3 + 2 * f2 + 0.5 * f);
      const c3 = gain * (0.5 * f3 - 0.5 * f2);
      const start = Math.floor(s - half) + 1;
      const end = Math.ceil(s + half);
      const w = Math.PI / half;
      const turnCos = Math.cos(w);
      const turnSin = Math.sin(w);
      let wc = Math.cos(w * (start - s));
      let ws = Math.sin(w * (start - s));
      for (let n = start; n < end; n += 1) {
        const j = n + whole;
        const value = c0 * input[(j - 1) & mask] + c1 * input[j & mask] + c2 * input[(j + 1) & mask] + c3 * input[(j + 2) & mask];
        output[n & mask] += (0.5 + 0.5 * wc) * value;
        const nc = wc * turnCos - ws * turnSin;
        ws = ws * turnCos + wc * turnSin;
        wc = nc;
      }
      if (voiced) this.markPulse(s, hop);
      this.next += hop;
    }
  }

  markPulse(s, hop) {
    const pulse = this.pulse;
    const mask = this.mask;
    const from = Math.ceil(s - 0.75 * hop);
    const to = Math.ceil(s + 0.25 * hop);
    const step = TWO_PI / hop;
    for (let n = from; n < to; n += 1) pulse[n & mask] = PULSE_GAIN * (1 + PULSE_DEPTH * Math.cos(step * (n - s + 0.25 * hop)));
  }

  process(sample, period, ratio) {
    this.input[this.written & this.mask] = sample;
    this.lp1 += this.lpCoef * (sample - this.lp1);
    this.lp2 += this.lpCoef * (this.lp1 - this.lp2);
    this.smooth[this.written & this.mask] = this.lp2;
    this.written += 1;
    this.addMarks(period);
    this.placeGrains(ratio);
    const t = this.written - 1 - this.delay;
    if (t < 0) return 0;
    const slot = t & this.mask;
    const shifted = this.output[slot];
    this.output[slot] = 0;
    this.pulseOut = this.pulse[slot];
    this.pulse[slot] = 1;
    const wanted = Math.abs(ratio - 1) > 2e-3 || Math.abs(this.range - 1) > 2e-3 || this.jitter > 0 || this.tremor > 0 ? 1 : 0;
    const step = 1 / (2 * this.unvoiced);
    this.shifting = wanted > this.shifting
      ? Math.min(wanted, this.shifting + step)
      : Math.max(wanted, this.shifting - step);
    return this.shifting * shifted + (1 - this.shifting) * this.input[slot];
  }
}

export class PeakLimiter {
  constructor(sampleRate) {
    this.length = Math.max(8, Math.round(sampleRate * 0.0013));
    this.required = new Float64Array(this.length).fill(1);
    this.held = new Float64Array(this.length).fill(1);
    this.delayLine = new Float64Array(this.length);
    this.pos = 0;
    this.heldSum = this.length;
    this.release = 1 / (0.1 * sampleRate);
    this.smoothed = 1;
    this.lowAge = this.length;
  }

  latency() {
    return this.length - 1;
  }

  process(sample) {
    const n = this.length;
    const pos = this.pos;
    const magnitude = Math.abs(sample);
    const required = this.required;
    let minimum = 1;
    if (magnitude > LIMIT_CEILING) {
      required[pos] = LIMIT_CEILING / magnitude;
      this.lowAge = 0;
    } else {
      required[pos] = 1;
      if (this.lowAge < n) this.lowAge += 1;
    }
    if (this.lowAge < n) {
      for (let i = 0; i < n; i += 1) if (required[i] < minimum) minimum = required[i];
    }
    const released = this.smoothed + this.release * (1 - this.smoothed);
    this.smoothed = minimum < released ? minimum : released;
    this.heldSum += this.smoothed - this.held[pos];
    this.held[pos] = this.smoothed;
    const next = pos + 1 === n ? 0 : pos + 1;
    const delayed = this.delayLine[next];
    this.delayLine[pos] = sample;
    this.pos = next;
    return delayed * Math.min(1, this.heldSum / n);
  }
}

export class VoiceEngine {
  constructor(sampleRate, userPitchHz = 0) {
    const size = sampleRate > 32000 ? 1024 : 512;
    this.sampleRate = sampleRate;
    this.size = size;
    this.hop = size / 8;
    this.half = size / 2;
    this.oversample = 8;
    this.fft = new RealFft(size);
    this.tail = size / 2;
    this.window = new Float64Array(size);
    this.synthesis = new Float64Array(size);
    this.windowPower = 0;
    this.olaScale = (size / 8) / (size / 4);
    this.inFifo = new Float64Array(size);
    this.outFifo = new Float64Array(size);
    this.accum = new Float64Array(size * 2);
    this.fifoPos = size - size / 8;
    this.frameBuffer = new Float64Array(size);
    this.re = new Float64Array(size / 2 + 1);
    this.im = new Float64Array(size / 2 + 1);
    this.cre = new Float64Array(size / 2 + 1);
    this.cim = new Float64Array(size / 2 + 1);
    this.cepstrum = new Float64Array(size);
    this.lifter = Math.max(12, Math.round(sampleRate / 650));
    const bins = size / 2 + 1;
    this.anaMag = new Float64Array(bins);
    this.envelope = new Float64Array(bins);
    this.logEnvelope = new Float64Array(bins);
    this.peakLog = new Float64Array(bins);
    this.envelopeReady = false;
    this.warpSource = new Float64Array(bins);
    this.warpFormant = 0;
    this.voicing = 0;
    this.airRe = new Float64Array(bins);
    this.airIm = new Float64Array(bins);
    this.airBuffer = new Float64Array(size);
    this.airAccum = new Float64Array(size * 2);
    this.airFifo = new Float64Array(size);
    this.airActive = false;
    this.airTurn = false;
    this.envelopeGlide = 1 - Math.exp(-this.hop / (ENVELOPE_GLIDE_S * sampleRate));
    this.gain = new Float64Array(bins).fill(1);
    this.noiseCos = new Float64Array(1024);
    this.noiseSin = new Float64Array(1024);
    for (let i = 0; i < 1024; i += 1) {
      this.noiseCos[i] = Math.cos((TWO_PI * i) / 1024);
      this.noiseSin[i] = Math.sin((TWO_PI * i) / 1024);
    }
    this.tilt = new Float64Array(bins).fill(1);
    this.decimate = Math.max(1, Math.round(sampleRate / 12000));
    this.pitch = new PitchDetector(sampleRate / this.decimate, PITCH_WINDOW);
    this.pitchRing = new Float64Array(PITCH_WINDOW);
    this.pitchScratch = new Float64Array(PITCH_WINDOW);
    this.pitchWrite = 0;
    this.decAcc = 0;
    this.decCount = 0;
    this.framesSincePitch = 0;
    this.userHz = 150;
    this.pitchHist = new Float64Array(HIST_BINS);
    this.histWeight = 1;
    this.histTotal = 0;
    this.histGrowth = Math.pow(2, PITCH_EVERY / (4 * HIST_HALF_LIFE));
    this.setUserPitch(userPitchHz);
    this.voiced = false;
    this.lastPitchHz = 0;
    this.recentPitch = new Float64Array(3);
    this.voicedHold = 0;
    this.shiftPitchHz = 0;
    this.shifter = new PitchShifter(sampleRate);
    this.limiter = new PeakLimiter(sampleRate);
    this.preset = null;
    this.plan = voicePlan(null, 0);
    this.ratio = 1;
    this.formant = 1;
    this.brightness = 0;
    this.tiltBrightness = 0;
    this.breath = 0;
    this.range = 1;
    this.jitter = 0;
    this.tremor = 0;
    this.mix = 0;
    this.mixTarget = 0;
    this.mixStep = 1 / (0.03 * sampleRate);
    this.level = 0;
    this.agc = 1;
    this.inPower = 0;
    this.outPower = 0;
    this.speechEnergy = 0;
    this.noiseEnergy = 0;
    this.denoiseAmount = 0;
    this.hp = { b0: 1, b1: 0, b2: 0, a1: 0, a2: 0, x1: 0, x2: 0, y1: 0, y2: 0 };
    this.noiseReady = false;
    this.denoiseFrames = 0;
    this.minFrames = 0;
    this.minSlot = 0;
    this.noisePower = new Float64Array(bins);
    this.trackPower = new Float64Array(bins);
    this.minPower = new Float64Array(bins);
    this.cleanPower = new Float64Array(bins);
    this.minHistory = new Float64Array(MIN_WINDOWS * bins);
    this.seed = 0x2545f491;
    const rise = size - this.tail / 2;
    const fall = size - this.tail;
    for (let i = 0; i < size; i += 1) {
      const hann = i < rise
        ? 0.5 - 0.5 * Math.cos((Math.PI * i) / rise)
        : 0.5 - 0.5 * Math.cos((TWO_PI * (i - fall)) / this.tail);
      this.window[i] = Math.sqrt(hann);
      this.windowPower += hann / size;
      this.synthesis[i] = i < fall
        ? 0
        : (0.5 - 0.5 * Math.cos((TWO_PI * (i - fall)) / this.tail)) / this.window[i];
    }
    this.airWindow = new Float64Array(this.tail);
    for (let i = 0; i < this.tail; i += 1) {
      this.airWindow[i] = Math.sin((Math.PI * (i + 0.5)) / this.tail) / Math.sqrt(this.windowPower);
    }
    this.dry = new Float64Array(this.tail + this.shifter.latency());
    this.dryPos = 0;
    this.airDelay = new Float64Array(this.shifter.latency());
    this.airPos = 0;
    this.setNoiseReduction(0.6);
    this.setVoice(null);
  }

  latencySamples() {
    return this.dry.length + this.limiter.latency();
  }

  setVoice(preset) {
    this.preset = preset && preset.pitchHz > 0 ? preset : null;
    this.mixTarget = this.preset ? 1 : 0;
  }

  setUserPitch(hz) {
    if (!(hz > 50 && hz < 500)) return;
    this.pitchHist.fill(0);
    this.histWeight = 1;
    this.histTotal = 0;
    this.addPitchReading(hz, SEED_WEIGHT);
  }

  addPitchReading(hz, weight) {
    const position = 12 * HIST_STEPS_PER_SEMITONE * Math.log2(hz / HIST_LOW_HZ);
    const bin = clamp(Math.floor(position), 0, HIST_BINS - 1);
    const amount = weight * this.histWeight;
    this.pitchHist[bin] += amount;
    this.histTotal += amount;
    this.histWeight *= this.histGrowth;
    if (this.histWeight > 1e6) {
      const scale = 1 / this.histWeight;
      for (let i = 0; i < HIST_BINS; i += 1) this.pitchHist[i] *= scale;
      this.histTotal *= scale;
      this.histWeight = 1;
    }
    const half = this.histTotal / 2;
    let sum = 0;
    for (let i = 0; i < HIST_BINS; i += 1) {
      const next = sum + this.pitchHist[i];
      if (next >= half) {
        const fraction = this.pitchHist[i] > 0 ? (half - sum) / this.pitchHist[i] : 0.5;
        this.userHz = HIST_LOW_HZ * Math.pow(2, (i + fraction) / (12 * HIST_STEPS_PER_SEMITONE));
        return;
      }
      sum = next;
    }
  }

  userPitchHz() {
    return this.userHz;
  }

  setNoiseReduction(amount) {
    const wanted = Number.isFinite(amount) ? clamp(amount, 0, 1) : 0;
    if (wanted > 0 && this.denoiseAmount <= 0) {
      this.noiseReady = false;
      this.denoiseFrames = 0;
      this.minFrames = 0;
    }
    this.denoiseAmount = wanted;
    const w0 = (TWO_PI * 70) / this.sampleRate;
    const alpha = Math.sin(w0) / Math.SQRT2;
    const a0 = 1 + alpha;
    const hp = this.hp;
    hp.b0 = (1 + Math.cos(w0)) / 2 / a0;
    hp.b1 = -(1 + Math.cos(w0)) / a0;
    hp.b2 = hp.b0;
    hp.a1 = (-2 * Math.cos(w0)) / a0;
    hp.a2 = (1 - alpha) / a0;
  }

  stats() {
    return {
      userPitchHz: Math.round(this.userPitchHz()),
      pitchHz: Math.round(this.lastPitchHz),
      voiced: this.voiced,
      ratio: this.ratio,
      formant: this.formant,
      mix: this.mix,
      latencyMs: Math.round((this.latencySamples() / this.sampleRate) * 1000),
    };
  }

  random() {
    let x = this.seed;
    x ^= x << 13;
    x ^= x >>> 17;
    x ^= x << 5;
    this.seed = x >>> 0;
    return this.seed / 4294967296;
  }

  updateTilt() {
    const tiltDb = this.brightness * 6;
    const binHz = this.sampleRate / this.size;
    for (let k = 0; k <= this.half; k += 1) {
      const hz = Math.max(k * binHz, 60);
      this.tilt[k] = Math.pow(10, (tiltDb * Math.log2(hz / 1000)) / 20);
    }
    this.tiltBrightness = this.brightness;
  }

  trackPitch(sample) {
    this.decAcc += sample;
    this.decCount += 1;
    if (this.decCount < this.decimate) return;
    this.pitchRing[this.pitchWrite] = this.decAcc / this.decCount;
    this.pitchWrite = (this.pitchWrite + 1) % this.pitchRing.length;
    this.decAcc = 0;
    this.decCount = 0;
  }

  analysePitch() {
    const n = this.pitchRing.length;
    const write = this.pitchWrite;
    for (let i = 0; i < n; i += 1) {
      const j = write + i;
      this.pitchScratch[i] = this.pitchRing[j < n ? j : j - n];
    }
    const hz = this.pitch.detect(this.pitchScratch, this.voicedHold > 0 ? this.shiftPitchHz : 0);
    this.voiced = hz >= PITCH_LOW_HZ && hz <= PITCH_HIGH_HZ;
    if (!this.voiced) {
      if (this.voicedHold > 0) {
        this.voicedHold -= 1;
        if (this.voicedHold === 0) this.shiftPitchHz = 0;
      }
      return;
    }
    const recent = this.recentPitch;
    if (this.voicedHold === 0) recent[0] = recent[1] = recent[2] = hz;
    recent[0] = recent[1];
    recent[1] = recent[2];
    recent[2] = hz;
    const a = recent[0];
    const b = recent[1];
    const c = recent[2];
    this.shiftPitchHz = Math.max(Math.min(a, b), Math.min(Math.max(a, b), c));
    this.voicedHold = VOICED_HOLD;
    this.lastPitchHz = hz;
    this.addPitchReading(this.shiftPitchHz, PITCH_EVERY / 4);
  }

  steer() {
    const plan = voicePlanInto(this.plan, this.preset, this.userPitchHz());
    this.ratio += 0.25 * (plan.ratio - this.ratio);
    this.formant += 0.15 * (plan.formant - this.formant);
    this.brightness += 0.15 * (plan.brightness - this.brightness);
    this.breath += 0.15 * (plan.breath - this.breath);
    this.range += 0.15 * (plan.range - this.range);
    this.jitter += 0.15 * (plan.jitter - this.jitter);
    this.tremor += 0.15 * (plan.tremor - this.tremor);
    if (Math.abs(this.range - 1) < 1e-4) this.range = 1;
    if (this.jitter < 1e-5) this.jitter = 0;
    if (this.tremor < 1e-5) this.tremor = 0;
    this.shifter.shape(this.range, this.sampleRate / this.userPitchHz(), this.jitter, this.tremor);
    if (Math.abs(this.brightness - this.tiltBrightness) > 2e-3) this.updateTilt();
  }

  process(input, output, count) {
    const n = this.size;
    const hop = this.hop;
    const latency = this.dry.length;
    const hp = this.hp;
    const filtering = this.denoiseAmount > 0;
    for (let i = 0; i < count; i += 1) {
      const sample = input ? input[i] : 0;
      let changed = sample;
      if (filtering) {
        changed = hp.b0 * sample + hp.b1 * hp.x1 + hp.b2 * hp.x2 - hp.a1 * hp.y1 - hp.a2 * hp.y2;
        hp.x2 = hp.x1;
        hp.x1 = sample;
        hp.y2 = hp.y1;
        hp.y1 = changed;
      }
      this.trackPitch(changed);
      this.inFifo[this.fifoPos] = changed;
      const delayed = this.dry[this.dryPos];
      this.dry[this.dryPos] = sample;
      this.dryPos = this.dryPos + 1 === latency ? 0 : this.dryPos + 1;
      if (this.mix !== this.mixTarget) {
        this.mix = this.mixTarget > this.mix
          ? Math.min(this.mixTarget, this.mix + this.mixStep)
          : Math.max(this.mixTarget, this.mix - this.mixStep);
      }
      const filtered = this.agc * this.outFifo[this.fifoPos - (n - hop)];
      const period = this.shiftPitchHz > 0 ? this.sampleRate / this.shiftPitchHz : 0;
      const airDelay = this.airDelay;
      const air = airDelay[this.airPos];
      airDelay[this.airPos] = this.agc * this.airFifo[this.fifoPos - (n - hop)];
      this.airPos = this.airPos + 1 === airDelay.length ? 0 : this.airPos + 1;
      const shifted = this.shifter.process(filtered, period, this.ratio);
      const wet = shifted + air * this.shifter.pulseOut;
      const mixed = this.mix * wet + (1 - this.mix) * delayed;
      output[i] = this.limiter.process(mixed);
      this.fifoPos += 1;
      if (this.fifoPos >= n) {
        this.fifoPos = n - hop;
        this.frame();
        this.outFifo.set(this.accum.subarray(0, hop), 0);
        this.accum.copyWithin(0, hop, hop + n);
        this.accum.fill(0, n, n + hop);
        this.airFifo.set(this.airAccum.subarray(0, hop), 0);
        this.airAccum.copyWithin(0, hop, hop + n);
        this.airAccum.fill(0, n, n + hop);
        this.inFifo.copyWithin(0, hop, n);
        this.levelMatch();
      }
    }
  }

  levelMatch() {
    if (this.inPower < 1e-6 || this.outPower < 1e-9) return;
    const wanted = clamp(Math.sqrt(this.inPower / this.outPower), 0.5, 4);
    this.agc += 0.05 * (wanted - this.agc);
  }

  denoise() {
    if (!this.noiseReady) {
      this.denoiseFrames += 1;
      if (this.denoiseFrames < this.oversample) return;
    }
    const framesPerSecond = this.sampleRate / this.hop;
    const track = 1 - Math.exp(-1 / (0.02 * framesPerSecond));
    const windowFrames = Math.max(1, Math.floor(0.2 * framesPerSecond));
    const bias = 4;
    const floorGain = Math.pow(10, (-18 * this.denoiseAmount) / 20);
    const bins = this.half + 1;
    let newWindow = false;
    if (this.noiseReady) {
      this.minFrames += 1;
      newWindow = this.minFrames >= windowFrames;
    }
    const finished = this.minSlot;
    if (newWindow) {
      this.minFrames = 0;
      this.minSlot = (this.minSlot + 1) % MIN_WINDOWS;
    }
    const history = this.minHistory;
    let speech = 0;
    let noise = 0;
    for (let k = 0; k <= this.half; k += 1) {
      const power = this.anaMag[k] * this.anaMag[k];
      if (!this.noiseReady) {
        this.trackPower[k] = power;
        for (let w = 0; w < MIN_WINDOWS; w += 1) history[w * bins + k] = power;
      } else {
        this.trackPower[k] += track * (power - this.trackPower[k]);
      }
      const tracked = this.trackPower[k];
      if (newWindow || !this.noiseReady) {
        if (this.noiseReady) history[finished * bins + k] = this.minPower[k];
        this.minPower[k] = tracked;
      } else if (tracked < this.minPower[k]) {
        this.minPower[k] = tracked;
      }
      let minimum = this.minPower[k];
      for (let w = 0; w < MIN_WINDOWS; w += 1) {
        const value = history[w * bins + k];
        if (value < minimum) minimum = value;
      }
      const estimate = bias * minimum;
      this.noisePower[k] = estimate;
      const expected = estimate + 1e-12;
      const posterior = power / expected;
      const prior = Math.max((0.96 * this.cleanPower[k]) / expected + 0.04 * Math.max(posterior - 1, 0), 1e-3);
      const gain = Math.max(prior / (1 + prior), floorGain);
      this.cleanPower[k] = gain * gain * power;
      this.anaMag[k] *= gain;
      this.gain[k] = gain;
      speech += power;
      noise += estimate;
    }
    this.noiseReady = true;
    this.speechEnergy = speech;
    this.noiseEnergy = noise;
  }

  frame() {
    const n = this.size;
    const half = this.half;
    const hop = this.hop;
    const re = this.re;
    const im = this.im;
    const buffer = this.frameBuffer;
    this.framesSincePitch += 1;
    if (this.framesSincePitch >= PITCH_EVERY) {
      this.framesSincePitch = 0;
      this.analysePitch();
    }
    this.steer();
    let energyIn = 0;
    for (let i = 0; i < n; i += 1) {
      const v = this.inFifo[i] * this.window[i];
      const j = i + half;
      buffer[j < n ? j : j - n] = v;
      energyIn += v * v;
    }
    this.level = Math.sqrt(((energyIn / n) * 0.375) / this.windowPower);
    this.fft.forward(buffer, re, im);
    const anaMag = this.anaMag;
    for (let k = 0; k <= half; k += 1) anaMag[k] = Math.sqrt(re[k] * re[k] + im[k] * im[k]);
    let speech = true;
    if (this.denoiseAmount > 0) {
      this.denoise();
      speech = this.speechEnergy > 3 * this.noiseEnergy;
    } else {
      this.gain.fill(1);
    }
    let spectrumIn = 0;
    for (let k = 0; k <= half; k += 1) spectrumIn += anaMag[k] * anaMag[k];
    this.updateEnvelope();
    const envelope = this.envelope;
    this.voicing += VOICING_GLIDE * ((this.voiced ? 1 : 0) - this.voicing);
    const depth = UNVOICED_WARP + (1 - UNVOICED_WARP) * this.voicing;
    const formant = 1 + (this.formant - 1) * depth;
    if (Math.abs(formant - this.warpFormant) > 1e-4) this.updateWarp(formant);
    const source = this.warpSource;
    const tilt = this.tilt;
    const gain = this.gain;
    let spectrumOut = 0;
    for (let k = 0; k <= half; k += 1) {
      const at = source[k];
      const low = Math.floor(at);
      const env = low >= half ? envelope[half] : envelope[low] + (envelope[low + 1] - envelope[low]) * (at - low);
      const factor = (gain[k] * env * tilt[k]) / (envelope[k] + EPS);
      const outRe = re[k] * factor;
      const outIm = im[k] * factor;
      re[k] = outRe;
      im[k] = outIm;
      spectrumOut += outRe * outRe + outIm * outIm;
    }
    im[0] = 0;
    im[half] = 0;
    if (speech) {
      const scale = 2 / (n * n * this.windowPower);
      const smoothing = 1 - Math.exp(-hop / (0.3 * this.sampleRate));
      this.inPower += smoothing * (spectrumIn * scale - this.inPower);
      this.outPower += smoothing * (spectrumOut * scale - this.outPower);
    }
    this.fft.inverse(re, im, buffer);
    const scale = this.olaScale;
    const fall = n - this.tail;
    for (let i = fall; i < n; i += 1) {
      const j = i + half;
      this.accum[i - fall] += buffer[j < n ? j : j - n] * this.synthesis[i] * scale;
    }
    const breath = this.breath * (UNVOICED_BREATH + (1 - UNVOICED_BREATH) * this.voicing);
    this.airTurn = !this.airTurn;
    if (this.airTurn && (breath > 1e-3 || this.airActive)) this.breathe(breath);
  }

  updateEnvelope() {
    const n = this.size;
    const half = this.half;
    const anaMag = this.anaMag;
    const peaks = this.peakLog;
    const cre = this.cre;
    const cim = this.cim;
    for (let k = 0; k <= half; k += 1) {
      peaks[k] = Math.log(anaMag[k] + EPS);
      cim[k] = 0;
    }
    const localHz = this.shiftPitchHz > 0 ? this.shiftPitchHz : this.userHz;
    const reach = Math.max(1, Math.round((PEAK_SPAN * localHz * n) / this.sampleRate));
    for (let k = 0; k <= half; k += 1) {
      let top = peaks[k];
      const from = k > reach ? k - reach : 0;
      const to = k + reach < half ? k + reach : half;
      for (let j = from; j <= to; j += 1) if (peaks[j] > top) top = peaks[j];
      cre[k] = top;
    }
    const cepstrum = this.cepstrum;
    this.fft.inverse(cre, cim, cepstrum);
    const lifter = Math.round(clamp((LIFTER_PERIODS * this.sampleRate) / this.userHz, this.lifter, n / 4));
    cepstrum.fill(0, lifter, n - lifter + 1);
    this.fft.forward(cepstrum, cre, cim);
    const envelope = this.envelope;
    const logEnvelope = this.logEnvelope;
    const settle = this.envelopeReady ? this.envelopeGlide : 1;
    for (let k = 0; k <= half; k += 1) {
      logEnvelope[k] += settle * (cre[k] - logEnvelope[k]);
      envelope[k] = Math.exp(logEnvelope[k]);
    }
    this.envelopeReady = true;
  }

  updateWarp(formant) {
    const binHz = this.sampleRate / this.size;
    const top = (this.half - 1) * binHz;
    const keep = Math.min(WARP_KEEP_HZ, 0.5 * top);
    const release = Math.min(WARP_RELEASE_HZ, top);
    const from = keep / formant;
    const slope = (release - from) / (release - keep);
    const source = this.warpSource;
    for (let k = 0; k <= this.half; k += 1) {
      const hz = k * binHz;
      const at = hz <= keep ? hz / formant : hz < release ? from + (hz - keep) * slope : hz;
      source[k] = at / binHz;
    }
    this.warpFormant = formant;
  }

  breathe(breath) {
    const n = this.size;
    const half = this.half;
    const envelope = this.envelope;
    const source = this.warpSource;
    const air = this.airRe;
    const airIm = this.airIm;
    const binHz = this.sampleRate / n;
    const level = breath * BREATH_LEVEL;
    for (let k = 0; k <= half; k += 1) {
      const at = source[k];
      const low = Math.floor(at);
      const env = low >= half ? envelope[half] : envelope[low] + (envelope[low + 1] - envelope[low]) * (at - low);
      const amount = level * env * this.tilt[k] * clamp((k * binHz - BREATH_LOW_HZ) / BREATH_RISE_HZ, 0, 1);
      const slot = (this.random() * 1024) | 0;
      air[k] = amount * this.noiseCos[slot];
      airIm[k] = amount * this.noiseSin[slot];
    }
    airIm[0] = 0;
    airIm[half] = 0;
    const buffer = this.airBuffer;
    this.fft.inverse(air, airIm, buffer);
    const window = this.airWindow;
    const accum = this.airAccum;
    for (let i = 0; i < this.tail; i += 1) accum[i] += buffer[i] * window[i];
    this.airActive = breath > 1e-3;
  }

}

if (typeof registerProcessor === "function" && typeof AudioWorkletProcessor === "function") {
  registerProcessor("fm-call-voice", class extends AudioWorkletProcessor {
    constructor(options) {
      super();
      const seed = Number(options?.processorOptions?.userPitchHz) || 0;
      this.engine = new VoiceEngine(sampleRate, seed);
      this.engine.setVoice(options?.processorOptions?.preset || null);
      this.scratch = new Float32Array(128);
      this.sinceReport = 0;
      this.running = true;
      this.port.onmessage = (event) => {
        const data = event.data || {};
        if (data.type === "voice") this.engine.setVoice(data.preset || null);
        else if (data.type === "pitch") this.engine.setUserPitch(Number(data.hz) || 0);
        else if (data.type === "stop") this.running = false;
      };
    }

    process(inputs, outputs) {
      const output = outputs[0];
      if (!output || !output.length) return this.running;
      const channel = output[0];
      const count = channel.length;
      const input = inputs[0] && inputs[0].length ? inputs[0][0] : null;
      if (!input && this.scratch.length !== count) this.scratch = new Float32Array(count);
      this.engine.process(input || this.scratch, channel, count);
      for (let c = 1; c < output.length; c += 1) output[c].set(channel);
      this.sinceReport += count;
      if (this.sinceReport >= sampleRate) {
        this.sinceReport = 0;
        this.port.postMessage({ type: "stats", ...this.engine.stats() });
      }
      return this.running;
    }
  });
}
