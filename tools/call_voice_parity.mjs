import { writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const W = await import(pathToFileURL(resolve(root, "app/public/chat-call-voice-worklet.js")).href);
const V = await import(pathToFileURL(resolve(root, "app/public/chat-call-voice.js")).href);

const SAMPLE_RATE = 48000;
const LENGTH = 48000;
const BLOCK = 480;
const STRIDE = 149;
const DIGITS = 1e8;

export function parityInput() {
  const x = new Float32Array(LENGTH);
  let seed = 0x1234567;
  let phase = 0;
  let lp = 0;
  const ramp = 480;
  const envelope = (i, a, b) => (i < a || i >= b ? 0 : Math.min(1, (i - a) / ramp, (b - i) / ramp));
  for (let i = 0; i < LENGTH; i += 1) {
    seed ^= seed << 13;
    seed >>>= 0;
    seed ^= seed >>> 17;
    seed ^= seed << 5;
    seed >>>= 0;
    const noise = seed / 4294967296 - 0.5;
    const glide = i < 1920 ? 0 : i >= 24960 ? 1 : (i - 1920) / 23040;
    const f0 = i < 28000 ? 220 - 40 * glide : 125;
    phase += f0 / SAMPLE_RATE;
    if (phase >= 1) phase -= 1;
    const saw = 2 * phase - 1;
    lp += 0.3 * (saw - lp);
    const voiced = 0.4 * lp * (envelope(i, 1920, 24960) + envelope(i, 31680, 46080));
    x[i] = voiced + noise * (0.3 * envelope(i, 24960, 30720) + 0.006);
  }
  return x;
}

const CASES = [
  { name: "off", voice: "off" },
  ...V.CALL_VOICES.map((voice) => ({ name: voice.id, voice: voice.id })),
  { name: "maya-seeded-120", voice: "maya", userPitchHz: 120 },
  { name: "maya-to-kofi", voice: "maya", switchAt: 24000, switchTo: "kofi" },
  { name: "off-to-teen", voice: "off", switchAt: 12000, switchTo: "teen" },
  { name: "robot-to-off", voice: "robot", switchAt: 36000, switchTo: "off" },
];

const round = (v) => Math.round(v * DIGITS) / DIGITS;

export function runCase(input, spec) {
  const engine = new W.VoiceEngine(SAMPLE_RATE, spec.userPitchHz || 0);
  engine.setVoice(V.callVoiceById(spec.voice));
  const output = new Float32Array(LENGTH);
  const at = spec.switchAt || LENGTH;
  engine.process(input.subarray(0, at), output.subarray(0, at), at);
  if (at < LENGTH) {
    engine.setVoice(V.callVoiceById(spec.switchTo));
    engine.process(input.subarray(at), output.subarray(at), LENGTH - at);
  }
  return { engine, output };
}

export function fixture() {
  const input = parityInput();
  const cases = CASES.map((spec) => {
    const { engine, output } = runCase(input, spec);
    const rms = [];
    for (let b = 0; b < LENGTH; b += BLOCK) {
      let sum = 0;
      for (let i = b; i < b + BLOCK; i += 1) sum += output[i] * output[i];
      rms.push(round(Math.sqrt(sum / BLOCK)));
    }
    const samples = [];
    for (let i = 0; i < LENGTH; i += STRIDE) samples.push(round(output[i]));
    let hash = 0x811c9dc5;
    for (const word of new Uint32Array(output.buffer)) hash = Math.imul(hash ^ word, 0x01000193) >>> 0;
    return {
      name: spec.name,
      voice: spec.voice,
      userPitchHz: spec.userPitchHz || 0,
      switchAt: spec.switchAt || 0,
      switchTo: spec.switchTo || "",
      finalUserPitchHz: round(engine.userPitchHz()),
      hash,
      rms,
      samples,
    };
  });
  return {
    sampleRate: SAMPLE_RATE,
    length: LENGTH,
    block: BLOCK,
    stride: STRIDE,
    latency: new W.VoiceEngine(SAMPLE_RATE).latencySamples(),
    voices: V.CALL_VOICES.map((voice) => ({
      id: voice.id,
      label: voice.label,
      pitchHz: voice.pitchHz,
      tract: voice.tract,
      brightness: voice.brightness,
      breath: voice.breath,
      range: Number.isFinite(voice.range) ? voice.range : 1,
      jitter: voice.jitter || 0,
      tremor: voice.tremor || 0,
    })),
    inputRms: round(Math.sqrt(input.reduce((sum, v) => sum + v * v, 0) / LENGTH)),
    inputSamples: Array.from({ length: Math.ceil(LENGTH / STRIDE) }, (_, k) => round(input[k * STRIDE])),
    cases,
  };
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const text = JSON.stringify(fixture()) + "\n";
  for (const path of process.argv.slice(2)) writeFileSync(path, text);
  if (process.argv.length < 3) process.stdout.write(text);
}
