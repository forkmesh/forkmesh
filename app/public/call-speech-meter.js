export const SPEECH_RECENT_MS = 30000;
export const SYLLABLES_PER_WORD = 1.25;

const FRAME_MS = 20;
const HANGOVER_MS = 320;
const MIN_PEAK_GAP_MS = 110;
const PEAK_RISE_DB = 4;
const FLOOR_RISE = 0.002;
const FLOOR_FALL = 0.2;
const SPEECH_OVER_FLOOR_DB = 9;
const SPEECH_MIN_DB = -58;

export function createSyllableCounter() {
  let floor = -70;
  let talkingUntil = 0;
  let talking = false;
  let valley = 0;
  let rising = false;
  let peak = -120;
  let lastPeakAt = -Infinity;
  let previous = -120;
  let syllables = 0;
  return {
    get syllables() {
      return syllables;
    },
    get talking() {
      return talking;
    },
    get floor() {
      return floor;
    },
    push(db, time) {
      if (!Number.isFinite(db)) db = -120;
      floor = db < floor ? floor + (db - floor) * FLOOR_FALL : floor + (db - floor) * FLOOR_RISE;
      const loud = db > SPEECH_MIN_DB && db > floor + SPEECH_OVER_FLOOR_DB;
      if (loud) talkingUntil = time + HANGOVER_MS;
      talking = time < talkingUntil;
      if (db > previous) {
        if (!rising) {
          valley = previous;
          rising = true;
        }
        peak = db;
      } else if (rising && db < previous) {
        rising = false;
        if (talking && peak - valley >= PEAK_RISE_DB && peak > floor + SPEECH_OVER_FLOOR_DB && time - lastPeakAt >= MIN_PEAK_GAP_MS) {
          syllables += 1;
          lastPeakAt = time;
        }
      }
      previous = db;
      return talking;
    },
    reset() {
      syllables = 0;
    },
  };
}

function bandLevel(analyser, data, sampleRate) {
  analyser.getFloatFrequencyData(data);
  const binHz = sampleRate / 2 / data.length;
  const low = Math.max(1, Math.floor(300 / binHz));
  const high = Math.min(data.length - 1, Math.ceil(3400 / binHz));
  let power = 0;
  for (let i = low; i <= high; i += 1) power += 10 ** (data[i] / 10);
  return 10 * Math.log10(power / (high - low + 1) + 1e-12);
}

export function createSpeechMeter({
  track,
  scope = globalThis,
  context = null,
  onChange = () => {},
} = {}) {
  const AudioContextClass = scope.AudioContext || scope.webkitAudioContext;
  if (!track || !AudioContextClass) return null;
  const audio = context || new AudioContextClass();
  const source = audio.createMediaStreamSource(new scope.MediaStream([track]));
  const analyser = audio.createAnalyser();
  analyser.fftSize = 512;
  analyser.smoothingTimeConstant = 0.2;
  source.connect(analyser);
  const data = new Float32Array(analyser.frequencyBinCount);
  const counter = createSyllableCounter();
  const state = { talking: false, level: 0, words: 0, lastSpokeAt: 0 };
  let stopped = false;

  const timer = scope.setInterval(() => {
    if (stopped) return;
    const now = scope.performance.now();
    const db = bandLevel(analyser, data, audio.sampleRate);
    const talking = counter.push(db, now);
    state.level = Math.max(0, Math.min(1, (db - counter.floor) / 40));
    if (talking) state.lastSpokeAt = Date.now();
    const words = Math.round(counter.syllables / SYLLABLES_PER_WORD);
    const changed = talking !== state.talking || words !== state.words;
    state.talking = talking;
    state.words = Math.max(state.words, words);
    if (changed) onChange({ ...state });
  }, FRAME_MS);

  return {
    get state() {
      return { ...state };
    },
    resetWords() {
      counter.reset();
      state.words = 0;
    },
    stop() {
      stopped = true;
      scope.clearInterval(timer);
      try {
        source.disconnect();
      } catch (_) {}
      if (!context) void audio.close?.().catch(() => {});
    },
  };
}

export function speakerState({ talking = false, lastSpokeAt = 0 } = {}, now = Date.now()) {
  if (talking) return "talking";
  if (lastSpokeAt && now - lastSpokeAt < SPEECH_RECENT_MS) return "recent";
  return "quiet";
}
