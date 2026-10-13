const KEY = 'office-sound';
const DEFAULTS = { on: true, vol: 0.6 };
let settings = null;
let ctx = null;
let noise = null;

export function cleanSound(v) {
  const on = v?.on !== false;
  const vol = Number(v?.vol);
  return { on, vol: Number.isFinite(vol) ? Math.min(1, Math.max(0, vol)) : DEFAULTS.vol };
}

export function soundSettings() {
  if (settings) return settings;
  let stored = null;
  try {
    stored = JSON.parse(localStorage.getItem(KEY) || 'null');
  } catch {}
  settings = cleanSound(stored);
  return settings;
}

export function setSound(next) {
  settings = cleanSound({ ...soundSettings(), ...next });
  try {
    localStorage.setItem(KEY, JSON.stringify(settings));
  } catch {}
  return settings;
}

export function applauseHits(seconds, crowd, rng = Math.random) {
  const hits = [];
  const count = Math.round(Math.min(14, Math.max(3, crowd)) * seconds * 5);
  for (let i = 0; i < count; i++) {
    const t = rng() * seconds;
    const rise = Math.min(1, t / 0.35);
    const fall = Math.min(1, (seconds - t) / (seconds * 0.4));
    const level = rise * fall;
    if (rng() > 0.25 + 0.75 * level) continue;
    hits.push({ t, gain: 0.12 + 0.3 * level * rng(), freq: 900 + rng() * 1500, pan: rng() * 1.6 - 0.8, len: 0.05 + rng() * 0.07 });
  }
  return hits.sort((a, b) => a.t - b.t);
}

function audio() {
  const s = soundSettings();
  if (!s.on || s.vol <= 0) return null;
  const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
  if (!AC) return null;
  try {
    if (!ctx) ctx = new AC();
    if (ctx.state === 'suspended') ctx.resume().catch(() => {});
  } catch {
    return null;
  }
  if (!noise) {
    noise = ctx.createBuffer(1, ctx.sampleRate, ctx.sampleRate);
    const data = noise.getChannelData(0);
    for (let i = 0; i < data.length; i++) data[i] = Math.random() * 2 - 1;
  }
  return ctx;
}

function hit(ac, at, h, volume) {
  const src = ac.createBufferSource();
  src.buffer = noise;
  const band = ac.createBiquadFilter();
  band.type = 'bandpass';
  band.frequency.value = h.freq;
  band.Q.value = 1.1;
  const gain = ac.createGain();
  gain.gain.setValueAtTime(0.0001, at);
  gain.gain.exponentialRampToValueAtTime(Math.max(0.0002, h.gain * volume), at + 0.004);
  gain.gain.exponentialRampToValueAtTime(0.0001, at + h.len);
  src.connect(band);
  band.connect(gain);
  if (ac.createStereoPanner) {
    const pan = ac.createStereoPanner();
    pan.pan.value = h.pan;
    gain.connect(pan);
    pan.connect(ac.destination);
  } else gain.connect(ac.destination);
  src.start(at, Math.random() * 0.8, h.len + 0.02);
}

export function playClaps(count = 4, gain = 0.5) {
  const ac = audio();
  if (!ac) return false;
  const vol = soundSettings().vol;
  for (let i = 0; i < count; i++) hit(ac, ac.currentTime + 0.02 + i * 0.28, { gain: 0.3 * gain, freq: 1300 + (i % 2) * 250, pan: 0, len: 0.07 }, vol);
  return true;
}

export function playApplause(seconds = 2.8, crowd = 6) {
  const ac = audio();
  if (!ac) return false;
  const vol = soundSettings().vol;
  for (const h of applauseHits(seconds, crowd)) hit(ac, ac.currentTime + 0.02 + h.t, h, vol);
  return true;
}

export function playChime(notes = [659, 784], gain = 0.22) {
  const ac = audio();
  if (!ac) return false;
  const vol = soundSettings().vol;
  notes.forEach((freq, i) => {
    const at = ac.currentTime + 0.02 + i * 0.17;
    const osc = ac.createOscillator();
    osc.type = 'sine';
    osc.frequency.value = freq;
    const g = ac.createGain();
    g.gain.setValueAtTime(0.0001, at);
    g.gain.exponentialRampToValueAtTime(Math.max(0.0002, gain * vol), at + 0.02);
    g.gain.exponentialRampToValueAtTime(0.0001, at + 0.45);
    osc.connect(g);
    g.connect(ac.destination);
    osc.start(at);
    osc.stop(at + 0.5);
  });
  return true;
}
