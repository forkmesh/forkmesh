export const CALL_TONES = Object.freeze({
  ringback: Object.freeze({ periodMs: 6000, bursts: [[0, 2]], freqs: [440, 480], gain: 0.12 }),
  ring: Object.freeze({ periodMs: 3000, bursts: [[0, 0.4], [0.6, 0.4]], freqs: [400, 450], gain: 0.3 }),
});

export function createCallTones({
  AudioContextImpl = globalThis.AudioContext || globalThis.webkitAudioContext,
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
} = {}) {
  let context = null;
  let timer = null;
  let playing = "";
  const live = new Set();

  function audio() {
    if (!AudioContextImpl) return null;
    if (!context) {
      try {
        context = new AudioContextImpl();
      } catch (_) {
        return null;
      }
    }
    if (context.state === "suspended") Promise.resolve(context.resume?.()).catch(() => {});
    return context;
  }

  function burst(freqs, start, length, level) {
    const gain = context.createGain();
    gain.gain.setValueAtTime(0, start);
    gain.gain.linearRampToValueAtTime(level, start + 0.02);
    gain.gain.setValueAtTime(level, start + length - 0.03);
    gain.gain.linearRampToValueAtTime(0, start + length);
    gain.connect(context.destination);
    live.add(gain);
    let left = freqs.length;
    for (const freq of freqs) {
      const oscillator = context.createOscillator();
      oscillator.type = "sine";
      oscillator.frequency.value = freq;
      oscillator.connect(gain);
      oscillator.onended = () => {
        left -= 1;
        if (left > 0) return;
        live.delete(gain);
        try {
          gain.disconnect();
        } catch (_) {}
      };
      oscillator.start(start);
      oscillator.stop(start + length);
    }
  }

  function stop() {
    if (timer !== null) clearIntervalImpl(timer);
    timer = null;
    playing = "";
    for (const gain of live) {
      try {
        gain.disconnect();
      } catch (_) {}
    }
    live.clear();
  }

  function play(name) {
    const pattern = CALL_TONES[name];
    if (!pattern || playing === name) return playing === name;
    stop();
    const ctx = audio();
    if (!ctx) return false;
    playing = name;
    const cycle = () => {
      const at = ctx.currentTime + 0.05;
      for (const [offset, length] of pattern.bursts) burst(pattern.freqs, at + offset, length, pattern.gain);
    };
    try {
      cycle();
    } catch (_) {
      stop();
      return false;
    }
    timer = setIntervalImpl(cycle, pattern.periodMs);
    return true;
  }

  return {
    ringback: () => play("ringback"),
    ring: () => play("ring"),
    stop,
    playing: () => playing,
    close() {
      stop();
      const closing = context;
      context = null;
      Promise.resolve(closing?.close?.()).catch(() => {});
    },
  };
}
