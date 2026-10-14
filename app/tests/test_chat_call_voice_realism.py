import json
import math
import shutil
import subprocess
from pathlib import Path

import pytest

PUBLIC = Path(__file__).resolve().parents[1] / "public"

COMMON = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[2].replace(/chat-call-voice-worklet\.js$/, "");
const W = await import(pathToFileURL(base + "chat-call-voice-worklet.js").href);
const V = await import(pathToFileURL(base + "chat-call-voice.js").href);
const sr = 48000;
const TWO_PI = 2 * Math.PI;
function rng(seed) {
  let s = seed >>> 0 || 1;
  return () => {
    s ^= s << 13; s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5; s >>>= 0;
    return s / 4294967296;
  };
}

const VOWELS = [
  [730, 1090, 2440], [270, 2290, 3010], [300, 870, 2240], [530, 1840, 2480], [570, 840, 2410], [660, 1720, 2410],
];
const BANDS = [80, 90, 120, 180, 220];

function plan(seconds, seed) {
  const r = rng(seed);
  const segs = [];
  let t = 0.25;
  while (t < seconds - 0.6) {
    if (r() < 0.45) {
      const len = 0.07 + 0.06 * r();
      segs.push({ kind: "f", start: t, end: t + len, hz: r() < 0.5 ? 5600 : 3000 });
      t += len;
    }
    const len = 0.16 + 0.16 * r();
    segs.push({ kind: "v", start: t, end: t + len, vowel: (r() * VOWELS.length) | 0, accent: r() < 0.4 ? 2.5 : 0 });
    t += len + (r() < 0.3 ? 0.06 + 0.08 * r() : 0.0);
  }
  return segs;
}

function contour(t, segs) {
  const phrase = (t % 2.2) / 2.2;
  let st = 2.5 - 5 * phrase;
  for (const s of segs) {
    if (s.kind === "v" && s.accent) {
      const mid = (s.start + s.end) / 2;
      st += s.accent * Math.exp(-(((t - mid) / 0.07) ** 2));
    }
  }
  return st;
}

function synth(segs, seconds, f0At, scale, seed) {
  const n = Math.floor(seconds * sr);
  const x = new Float32Array(n);
  const voicedAmp = new Float32Array(n);
  const fricAmp = new Float32Array(n);
  const vowelAt = new Int8Array(n).fill(-1);
  const f0 = new Float32Array(n);
  for (const s of segs) {
    const a = Math.floor(s.start * sr), b = Math.min(n, Math.floor(s.end * sr));
    const ramp = 0.02 * sr;
    for (let i = a; i < b; i++) {
      const env = Math.min(1, (i - a) / ramp, (b - i) / ramp);
      if (s.kind === "v") { voicedAmp[i] = env; vowelAt[i] = s.vowel; } else fricAmp[i] = env * env;
    }
  }
  const noise = rng(seed);
  const jit = rng(seed + 7);
  let phase = 0, period = 1, jitter = 1, prevG = 0;
  const state = BANDS.map(() => [0, 0]);
  let cur = VOWELS[0].concat([3300, 3750]);
  const fstate = [0, 0];
  let last = 0;
  for (let i = 0; i < n; i++) {
    const t = i / sr;
    const hz = f0At(t);
    f0[i] = voicedAmp[i] > 0 ? hz : 0;
    phase += (hz * jitter) / sr;
    if (phase >= 1) { phase -= 1; jitter = 1 + 0.01 * (jit() - 0.5); }
    const p = phase;
    const g = p < 0.4 ? 0.5 * (1 - Math.cos((Math.PI * p) / 0.4)) : p < 0.56 ? Math.cos((Math.PI * (p - 0.4)) / 0.32) : 0;
    const dg = (g - prevG) * 40;
    prevG = g;
    const asp = (noise() - 0.5) * 0.04 * g;
    let src = voicedAmp[i] * (dg + asp);
    const want = vowelAt[i] >= 0 ? VOWELS[vowelAt[i]].concat([3300, 3750]) : cur;
    for (let k = 0; k < 5; k++) cur[k] += 0.004 * (want[k] - cur[k]);
    let v = src;
    for (let k = 0; k < 5; k++) {
      const fhz = Math.min(cur[k] * scale, 0.45 * sr);
      const rr = Math.exp((-Math.PI * BANDS[k] * scale) / sr);
      const c = 2 * rr * Math.cos((TWO_PI * fhz) / sr);
      const g0 = 1 - c + rr * rr;
      const y = g0 * v + c * state[k][0] - rr * rr * state[k][1];
      state[k][1] = state[k][0];
      state[k][0] = y;
      v = y;
    }
    let fr = 0;
    if (fricAmp[i] > 0) {
      const seg = segs.find((s) => s.kind === "f" && t >= s.start && t < s.end);
      const fhz = Math.min((seg ? seg.hz : 5000) * scale, 0.42 * sr);
      const rr = Math.exp((-Math.PI * 1800) / sr);
      const c = 2 * rr * Math.cos((TWO_PI * fhz) / sr);
      const y = (noise() - 0.5) + c * fstate[0] - rr * rr * fstate[1];
      fstate[1] = fstate[0];
      fstate[0] = y;
      fr = 0.005 * fricAmp[i] * y;
    } else {
      fstate[0] *= 0.9; fstate[1] *= 0.9;
    }
    last = 0.15 * v + fr + 1e-4 * (noise() - 0.5);
    x[i] = last;
  }
  return { x, f0, voicedAmp, fricAmp };
}

function decimate3(y, from = 0, len = y.length) {
  const out = new Float64Array(Math.floor(len / 3));
  for (let i = 0; i < out.length; i++) {
    const j = from + 3 * i;
    out[i] = ((y[j - 1] || 0) + 2 * (y[j] || 0) + (y[j + 1] || 0)) / 4;
  }
  return out;
}

function yin(frame, rate, lo = 55, hi = 600) {
  const W = frame.length >> 1;
  const maxLag = Math.min(W, Math.ceil(rate / lo));
  const minLag = Math.floor(rate / hi);
  const d = new Float64Array(maxLag + 1);
  for (let tau = 1; tau <= maxLag; tau++) {
    let s = 0;
    for (let i = 0; i < W; i++) { const v = frame[i] - frame[i + tau]; s += v * v; }
    d[tau] = s;
  }
  let run = 0;
  const cm = new Float64Array(maxLag + 1);
  cm[0] = 1;
  for (let tau = 1; tau <= maxLag; tau++) { run += d[tau]; cm[tau] = run > 0 ? (d[tau] * tau) / run : 1; }
  let tau = minLag;
  for (; tau < maxLag; tau++) {
    if (cm[tau] < 0.15) { while (tau + 1 < maxLag && cm[tau + 1] < cm[tau]) tau++; break; }
  }
  if (tau >= maxLag) {
    let best = minLag;
    for (let t = minLag; t < maxLag; t++) if (cm[t] < cm[best]) best = t;
    if (cm[best] > 0.35) return { hz: 0, ap: cm[best] };
    tau = best;
  }
  const a = cm[tau - 1], b = cm[tau], c = cm[tau + 1];
  const den = a - 2 * b + c;
  const sh = Math.abs(den) > 1e-12 ? Math.max(-1, Math.min(1, (0.5 * (a - c)) / den)) : 0;
  return { hz: rate / (tau + sh), ap: b };
}

function lpcEnvelope(frame, order, rate, freqs) {
  const n = frame.length;
  const x = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const pre = frame[i] - 0.97 * (i ? frame[i - 1] : 0);
    x[i] = pre * (0.54 - 0.46 * Math.cos((TWO_PI * i) / (n - 1)));
  }
  const r = new Float64Array(order + 1);
  for (let k = 0; k <= order; k++) { let s = 0; for (let i = k; i < n; i++) s += x[i] * x[i - k]; r[k] = s; }
  r[0] *= 1.0001;
  if (r[0] <= 1e-14) return null;
  const a = new Float64Array(order + 1); a[0] = 1;
  let err = r[0];
  const tmp = new Float64Array(order + 1);
  for (let i = 1; i <= order; i++) {
    let acc = r[i];
    for (let j = 1; j < i; j++) acc += a[j] * r[i - j];
    const k = -acc / err;
    tmp.set(a);
    for (let j = 1; j < i; j++) a[j] = tmp[j] + k * tmp[i - j];
    a[i] = k;
    err *= 1 - k * k;
  }
  return freqs.map((f) => {
    const w = (TWO_PI * f) / rate;
    let re = 0, im = 0;
    for (let k = 0; k <= order; k++) { re += a[k] * Math.cos(w * k); im -= a[k] * Math.sin(w * k); }
    return 10 * Math.log10(err / (re * re + im * im + 1e-20));
  });
}

function periodicity(frame, rate, hz) {
  const lag0 = rate / hz;
  const W = frame.length - Math.ceil(lag0 * 1.1) - 1;
  if (W < 64) return 0;
  let best = 0;
  for (let lag = Math.floor(lag0 * 0.9); lag <= Math.ceil(lag0 * 1.1); lag++) {
    let xy = 0, xx = 0, yy = 0;
    for (let i = 0; i < W; i++) { const a = frame[i], b = frame[i + lag]; xy += a * b; xx += a * a; yy += b * b; }
    best = Math.max(best, xy / Math.sqrt(xx * yy + 1e-20));
  }
  return best;
}

const FREQS = Array.from({ length: 60 }, (_, i) => 150 * Math.pow(5000 / 150, i / 59));

function lsdBetween(a, b) {
  const d = a.map((v, k) => v - b[k]);
  const m = d.reduce((x, y) => x + y, 0) / d.length;
  return Math.sqrt(d.reduce((x, y) => x + (y - m) ** 2, 0) / d.length);
}
'''

HARNESS = COMMON + r'''
function evaluate(caller, preset) {
  const seconds = 5;
  const segs = plan(seconds, 99);
  const callerAt = (t) => caller.hz * Math.pow(2, contour(t, segs) / 12);
  const input = synth(segs, seconds, callerAt, caller.scale, 5);
  const voicedHz = [];
  for (let i = 0; i < input.f0.length; i += 480) if (input.f0[i] > 0) voicedHz.push(input.f0[i]);
  voicedHz.sort((a, b) => a - b);
  const median = voicedHz[voicedHz.length >> 1];
  const p = W.voicePlan(preset, median);
  const intendedAt = (hz) => median * p.ratio * Math.pow(hz / median, p.range ?? 1);
  const ideal = synth(segs, seconds, (t) => intendedAt(callerAt(t)), caller.scale * p.formant, 5);
  const engine = new W.VoiceEngine(sr, median);
  engine.setVoice(preset);
  const y = new Float32Array(input.x.length);
  const times = [];
  let gross = 0, trackN = 0, missed = 0;
  for (let i = 0; i < y.length; i += 128) {
    const t0 = process.hrtime.bigint();
    engine.process(input.x.subarray(i, i + 128), y.subarray(i, i + 128), 128);
    times.push(Number(process.hrtime.bigint() - t0) / 1e3);
    const at = i + 128 - 768;
    if (i > sr * 0.5 && at > 2400 && input.voicedAmp[at] >= 1 && input.voicedAmp[at - 2400] >= 1 && input.voicedAmp[at + 2400] >= 1) {
      trackN++;
      const got = engine.shiftPitchHz;
      if (!(got > 0)) missed++;
      else if (Math.abs(12 * Math.log2(got / input.f0[at])) > 3) gross++;
    }
  }
  let nan = 0, peak = 0;
  for (const v of y) { if (!Number.isFinite(v)) nan++; peak = Math.max(peak, Math.abs(v)); }
  const L = engine.latencySamples();
  const out = new Float32Array(y.length);
  for (let i = 0; i + L < y.length; i++) out[i] = y[i + L];
  const rate = sr / 3;
  const errs = [], lsd = [], lsdNone = [], per = [], perIdeal = [], buzz = [], buzzIn = [];
  for (let c = Math.floor(0.6 * sr); c + 2048 < out.length; c += 480) {
    const vA = input.voicedAmp;
    if (vA[c] >= 1 && vA[c - 1600] >= 1 && vA[c + 1600] >= 1) {
      const want = intendedAt(input.f0[c]);
      const fo = decimate3(out, c - 1023, 2046);
      const fi = decimate3(ideal.x, c - 1023, 2046);
      const r = yin(fo, rate);
      errs.push(r.hz > 0 ? 12 * Math.log2(r.hz / want) : 99);
      const eo = lpcEnvelope(fo.subarray(0, 512), 18, rate, FREQS);
      const ei = lpcEnvelope(fi.subarray(0, 512), 18, rate, FREQS);
      const en = lpcEnvelope(decimate3(input.x, c - 1023, 2046).subarray(0, 512), 18, rate, FREQS);
      if (eo && ei) lsd.push(lsdBetween(eo, ei));
      if (en && ei) lsdNone.push(lsdBetween(en, ei));
      per.push(periodicity(fo, rate, want));
      perIdeal.push(periodicity(fi, rate, want));
    }
    if (input.fricAmp[c] > 0.6 && input.fricAmp[c - 960] > 0.3 && input.fricAmp[c + 960] > 0.3) {
      const want = intendedAt(caller.hz);
      buzz.push(periodicity(decimate3(out, c - 1023, 2046), rate, want));
      buzzIn.push(periodicity(decimate3(input.x, c - 1023, 2046), rate, want));
    }
  }
  const share = (f) => errs.filter(f).length / errs.length;
  const mean = (a) => a.reduce((s, v) => s + v, 0) / Math.max(1, a.length);
  const quanta = times.slice(200);
  return {
    frames: errs.length,
    within1: share((e) => Math.abs(e) <= 1),
    within05: share((e) => Math.abs(e) <= 0.5),
    octave: share((e) => Math.abs(Math.abs(e) - 12) < 2),
    lsd: mean(lsd), lsdUnchanged: mean(lsdNone),
    periodicity: mean(per), idealPeriodicity: mean(perIdeal),
    fricativeBuzz: mean(buzz) - mean(buzzIn),
    trackGross: gross / trackN, trackMissed: missed / trackN,
    nan, peak, latencyMs: (L / sr) * 1000, meanUs: mean(quanta),
  };
}

const callers = { male: { hz: 115, scale: 1.0 }, female: { hz: 205, scale: 1.17 } };
const result = {};
for (const [cn, c] of Object.entries(callers)) {
  for (const id of ["maya", "kofi", "ingrid", "theo", "deep", "teen", "robot"]) {
    const preset = { ...V.callVoiceById(id), brightness: 0, breath: 0 };
    result[cn + ">" + id] = evaluate(c, preset);
  }
}
console.log(JSON.stringify(result));
'''

DETAIL_HARNESS = COMMON + r'''
function fftInPlace(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -TWO_PI / len;
    for (let k = 0; k < len / 2; k++) {
      const wr = Math.cos(ang * k), wi = Math.sin(ang * k);
      for (let i = k; i < n; i += len) {
        const b = i + len / 2;
        const xr = re[b] * wr - im[b] * wi, xi = re[b] * wi + im[b] * wr;
        re[b] = re[i] - xr; im[b] = im[i] - xi; re[i] += xr; im[i] += xi;
      }
    }
  }
}

function power(frame, size = 16384) {
  const re = new Float64Array(size), im = new Float64Array(size);
  const n = frame.length;
  for (let i = 0; i < n; i++) re[i] = frame[i] * (0.5 - 0.5 * Math.cos((TWO_PI * i) / (n - 1)));
  fftInPlace(re, im);
  const P = new Float64Array(size / 2 + 1);
  for (let k = 0; k <= size / 2; k++) P[k] = re[k] * re[k] + im[k] * im[k];
  return { P, hz: sr / size };
}

function refineF0(P, hz, guess) {
  let best = guess, bestSum = -1;
  for (let s = -60; s <= 60; s++) {
    const f = guess * (1 + s * 0.0005);
    let sum = 0;
    for (let h = 1; h * f < 3000; h++) sum += P[Math.round((h * f) / hz)];
    if (sum > bestSum) { bestSum = sum; best = f; }
  }
  return best;
}

function interHarmonic(P, hz, f0, lo, hi) {
  let total = 0, between = 0;
  for (let k = 0; k < P.length; k++) total += P[k];
  for (let k = Math.ceil(lo / hz); k <= hi / hz; k++) {
    const q = (k * hz) / f0;
    if (Math.abs(q - Math.round(q)) > 0.3) between += P[k];
  }
  return 10 * Math.log10((2.5 * between + 1e-30) / total);
}

function formantPeak(P, hz, f0, lo, hi) {
  const amps = [];
  for (let h = 1; h * f0 < hi + 2 * f0; h++) {
    const c = Math.round((h * f0) / hz);
    let a = 0;
    for (let k = c - 2; k <= c + 2; k++) a += P[k];
    amps.push([h * f0, 10 * Math.log10(a + 1e-30)]);
  }
  let best = -1;
  for (let i = 1; i < amps.length - 1; i++) {
    if (amps[i][0] < lo || amps[i][0] > hi || amps[i][1] < amps[i - 1][1] || amps[i][1] < amps[i + 1][1]) continue;
    if (best < 0 || amps[i][1] > amps[best][1]) best = i;
  }
  if (best < 0) return 0;
  const [a, b, c] = [amps[best - 1][1], amps[best][1], amps[best + 1][1]];
  const d = a - 2 * b + c;
  const shift = d < 0 ? Math.max(-0.5, Math.min(0.5, (0.5 * (a - c)) / d)) : 0;
  return amps[best][0] + shift * f0;
}

const STEADY = [[500, 80], [1500, 90], [2500, 120], [3300, 180], [3750, 220]];

function steadyVowel(seconds, f0, scale) {
  const n = Math.floor(seconds * sr);
  const x = new Float32Array(n);
  let phase = 0, prev = 0;
  const st = STEADY.map(() => [0, 0]);
  for (let i = 0; i < n; i++) {
    phase += f0 / sr;
    if (phase >= 1) phase -= 1;
    const g = phase < 0.4 ? 0.5 * (1 - Math.cos((Math.PI * phase) / 0.4)) : phase < 0.56 ? Math.cos((Math.PI * (phase - 0.4)) / 0.32) : 0;
    let v = (g - prev) * 40;
    prev = g;
    for (let k = 0; k < STEADY.length; k++) {
      const r = Math.exp((-Math.PI * STEADY[k][1] * scale) / sr);
      const c = 2 * r * Math.cos((TWO_PI * STEADY[k][0] * scale) / sr);
      const y = (1 - c + r * r) * v + c * st[k][0] - r * r * st[k][1];
      st[k][1] = st[k][0];
      st[k][0] = y;
      v = y;
    }
    x[i] = 0.1 * v;
  }
  return x;
}

function fricative(seconds, centre, seed) {
  const n = Math.floor(seconds * sr);
  const x = new Float32Array(n);
  const noise = rng(seed);
  const r = Math.exp((-Math.PI * 1500) / sr);
  const c = 2 * r * Math.cos((TWO_PI * centre) / sr);
  let y1 = 0, y2 = 0;
  for (let i = 0; i < n; i++) {
    const y = noise() - 0.5 + c * y1 - r * r * y2;
    y2 = y1;
    y1 = y;
    x[i] = 0.02 * y * Math.min(1, i / 2400, (n - i) / 2400);
  }
  return x;
}

function centroid(y, from, to) {
  let num = 0, den = 0;
  for (let c = from + 2048; c + 2048 < to; c += 2048) {
    const { P, hz } = power(y.subarray(c - 2048, c + 2048), 4096);
    for (let k = Math.ceil(1500 / hz); k <= 11000 / hz; k++) { num += k * hz * P[k]; den += P[k]; }
  }
  return num / den;
}

function run(x, preset, seed, quiet = false) {
  const engine = new W.VoiceEngine(sr, seed);
  if (quiet) engine.setNoiseReduction(0);
  engine.setVoice(preset);
  const y = new Float32Array(x.length);
  for (let i = 0; i < y.length; i += 128) engine.process(x.subarray(i, i + 128), y.subarray(i, i + 128), 128);
  const L = engine.latencySamples();
  const out = new Float32Array(y.length);
  for (let i = 0; i + L < y.length; i++) out[i] = y[i + L];
  return out;
}

function clicksNear(y, edges) {
  const e = new Float64Array(y.length);
  for (let i = 2; i < y.length; i++) e[i] = Math.abs(y[i] - 2 * y[i - 1] + y[i - 2]);
  const sums = new Float64Array(y.length + 1);
  for (let i = 0; i < y.length; i++) sums[i + 1] = sums[i] + e[i] * e[i];
  let worst = 0;
  for (const edge of edges) {
    const at = Math.floor(edge * sr);
    for (let i = Math.max(960, at - 1440); i < Math.min(y.length - 960, at + 1440); i++) {
      const rms = Math.sqrt((sums[i + 480] - sums[i - 480]) / 960) + 1e-6;
      worst = Math.max(worst, e[i] / rms);
    }
  }
  return worst;
}

const detail = { steady: {}, fricative: {}, breath: {}, edges: {} };
const plain = (id) => ({ ...V.callVoiceById(id), brightness: 0, breath: 0, jitter: 0, tremor: 0 });
for (const [cn, hz, scale] of [["male", 48000 / 417.37, 1], ["female", 48000 / 234.15, 1.17]]) {
  const x = steadyVowel(2.5, hz, scale);
  for (const id of ["maya", "ingrid", "teen", "theo", "kofi", "deep"]) {
    const preset = plain(id);
    const p = W.voicePlan(preset, hz);
    const y = run(x, preset, hz, true);
    const noise = [], ratio = [], f2 = [];
    for (let c = 0.8 * sr; c + 4096 < y.length - 0.1 * sr; c += 4800) {
      const { P, hz: bin } = power(y.subarray(c - 4096, c + 4096));
      const f0 = refineF0(P, bin, hz * p.ratio);
      noise.push(interHarmonic(P, bin, f0, 100, 2000));
      ratio.push(f0 / hz);
      f2.push(formantPeak(P, bin, f0, 1000, 2000));
    }
    const mid = (a) => a.slice().sort((u, v) => u - v)[a.length >> 1];
    detail.steady[cn + ">" + id] = { noise: mid(noise), ratio: mid(ratio), planRatio: p.ratio, formant: mid(f2) / (1500 * scale), planFormant: p.formant };
  }
  const ideal = steadyVowel(2.5, hz, scale);
  const { P, hz: bin } = power(ideal.subarray(sr - 4096, sr + 4096));
  detail.steady[cn + ">dry"] = { noise: interHarmonic(P, bin, refineF0(P, bin, hz), 100, 2000) };
  for (const id of ["ingrid", "older-woman"]) {
    const levels = [];
    for (const breath of [0, 0.1]) {
      const y = run(x, { ...plain(id), breath }, hz, true);
      const p = W.voicePlan(plain(id), hz);
      const high = [], low = [];
      for (let c = 0.8 * sr; c + 4096 < y.length - 0.1 * sr; c += 4800) {
        const { P, hz: bin } = power(y.subarray(c - 4096, c + 4096));
        const f0 = refineF0(P, bin, hz * p.ratio);
        high.push(interHarmonic(P, bin, f0, 2000, 5000));
        low.push(interHarmonic(P, bin, f0, 100, 1000));
      }
      levels.push({ high: high.reduce((a, b) => a + b) / high.length, low: low.reduce((a, b) => a + b) / low.length });
    }
    detail.breath[cn + ">" + id] = { plainHigh: levels[0].high, breathHigh: levels[1].high, breathLow: levels[1].low };
  }
}
for (const centre of [3000, 5600]) {
  const x = fricative(1.5, centre, 11);
  const base = centroid(x, 0.4 * sr, 1.3 * sr);
  for (const [id, caller] of [["ingrid", 115], ["teen", 115], ["kofi", 205], ["deep", 205]]) {
    const p = W.voicePlan(plain(id), caller);
    const y = run(x, plain(id), caller);
    detail.fricative[centre + ">" + id] = { moved: centroid(y, 0.4 * sr, 1.3 * sr) / base, planFormant: p.formant };
  }
}
for (const [cn, caller] of [["male", 115, 1], ["female", 205, 1.17]].map(([n, hz, scale]) => [n, { hz, scale }])) {
  const segs = plan(5, 99);
  const callerAt = (t) => caller.hz * Math.pow(2, contour(t, segs) / 12);
  const input = synth(segs, 5, callerAt, caller.scale, 5);
  const edges = [];
  for (const s of segs) edges.push(s.start, s.end);
  for (const id of ["maya", "kofi", "ingrid", "deep", "older-woman"]) {
    const preset = { ...V.callVoiceById(id), breath: 0 };
    const p = W.voicePlan(preset, caller.hz);
    const ideal = synth(segs, 5, (t) => caller.hz * p.ratio * Math.pow(callerAt(t) / caller.hz, p.range), caller.scale * p.formant, 5);
    detail.edges[cn + ">" + id] = { worst: clicksNear(run(input.x, preset, caller.hz), edges), ideal: clicksNear(ideal.x, edges) };
  }
}
console.log(JSON.stringify(detail));
'''

VOICES = ("maya", "kofi", "ingrid", "theo", "deep", "teen", "robot")


def _run(harness):
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(PUBLIC / "chat-call-voice-worklet.js")],
        input=harness, capture_output=True, text=True, timeout=240, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


@pytest.fixture(scope="module")
def realism():
    return _run(HARNESS)


@pytest.fixture(scope="module")
def detail():
    return _run(DETAIL_HARNESS)


def test_every_case_is_measured(realism):
    assert sorted(realism) == sorted(f"{caller}>{voice}" for caller in ("male", "female") for voice in VOICES)
    for case in realism.values():
        assert case["frames"] > 150
        assert case["nan"] == 0
        assert case["peak"] <= 0.951


def test_output_follows_the_intended_pitch_contour(realism):
    for name, case in realism.items():
        assert case["within1"] >= 0.975, (name, case)
        assert case["octave"] <= 0.01, (name, case)


def test_the_tracker_finds_every_voiced_stretch_without_gross_errors(realism):
    for name, case in realism.items():
        assert case["trackMissed"] == 0, (name, case)
        assert case["trackGross"] <= 0.02, (name, case)


def test_formants_land_close_to_a_natural_voice_of_the_target_size(realism):
    for name, case in realism.items():
        assert case["lsd"] <= 3.4, (name, case)
        if case["lsdUnchanged"] > 3:
            assert case["lsd"] <= case["lsdUnchanged"] - 1, (name, case)


def test_shifted_voice_stays_as_periodic_as_a_natural_one(realism):
    for name, case in realism.items():
        assert case["idealPeriodicity"] - case["periodicity"] <= 0.016, (name, case)


def test_fricatives_are_not_given_a_buzz(realism):
    buzz = [case["fricativeBuzz"] for case in realism.values()]
    assert max(buzz) <= 0.08
    assert sum(buzz) / len(buzz) <= 0.04


def test_shifted_harmonics_stay_clean_between_the_partials(detail):
    steady = detail["steady"]
    for name, case in steady.items():
        if name.endswith(">dry"):
            continue
        assert case["noise"] <= -40, (name, case)


def test_harmonics_follow_the_pitch_ratio_and_formants_follow_the_tract(detail):
    for name, case in detail["steady"].items():
        if name.endswith(">dry"):
            continue
        assert abs(case["ratio"] / case["planRatio"] - 1) <= 0.01, (name, case)
        assert abs(math.log(case["formant"] / case["planFormant"])) <= 0.1, (name, case)


def test_frication_is_only_lightly_formant_warped(detail):
    for name, case in detail["fricative"].items():
        assert abs(math.log(case["moved"])) <= 0.5 * abs(math.log(case["planFormant"])), (name, case)


def test_breath_is_aperiodic_noise_that_fills_between_harmonics(detail):
    for name, case in detail["breath"].items():
        assert case["breathHigh"] - case["plainHigh"] >= 12, (name, case)
        assert case["breathLow"] <= -35, (name, case)


def test_voiced_and_unvoiced_boundaries_stay_click_free(detail):
    for name, case in detail["edges"].items():
        assert case["worst"] <= 1.8 * case["ideal"], (name, case)
