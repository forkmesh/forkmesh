import json
import shutil
import subprocess
from pathlib import Path

import pytest

PUBLIC = Path(__file__).resolve().parents[1] / "public"

HARNESS = r'''
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

VOICES = ("maya", "kofi", "ingrid", "theo", "deep", "teen", "robot")


@pytest.fixture(scope="module")
def realism():
    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [node, "--input-type=module", "-", str(PUBLIC / "chat-call-voice-worklet.js")],
        input=HARNESS, capture_output=True, text=True, timeout=240, check=False,
    )
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout.strip().splitlines()[-1])


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
