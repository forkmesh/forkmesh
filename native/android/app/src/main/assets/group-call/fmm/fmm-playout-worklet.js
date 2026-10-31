const START = 1440;
const LIMIT = 9600;
const STATS_EVERY = 48000;

class FmmPlayout extends AudioWorkletProcessor {
  constructor() {
    super();
    this.engine = null;
    this.streams = new Map();
    this.counted = 0;
    this.underruns = 0;
    this.trimmed = 0;
    this.port.onmessage = (event) => {
      if (event.data?.port) {
        this.engine = event.data.port;
        this.engine.onmessage = (message) => this.receive(message.data);
      }
    };
  }

  receive(message) {
    if (!message) return;
    if (message.t === "pcm") {
      let s = this.streams.get(message.id);
      if (!s) {
        s = { chunks: [], offset: 0, queued: 0, playing: false };
        this.streams.set(message.id, s);
      }
      s.chunks.push(message.pcm);
      s.queued += message.pcm.length;
      while (s.queued > LIMIT && s.chunks.length > 1) {
        const old = s.chunks.shift();
        s.queued -= old.length - s.offset;
        s.offset = 0;
        this.trimmed += 1;
      }
    } else if (message.t === "drop") {
      this.streams.delete(message.id);
    } else if (message.t === "reset") {
      this.streams.clear();
    } else if (message.t === "stats" && this.engine) {
      this.engine.postMessage({ t: "stats", underruns: this.underruns, trimmed: this.trimmed });
    }
  }

  process(inputs, outputs) {
    const out = outputs[0][0];
    out.fill(0);
    for (const s of this.streams.values()) {
      if (!s.playing) {
        if (s.queued < START) continue;
        s.playing = true;
      }
      let i = 0;
      while (i < out.length && s.chunks.length) {
        const chunk = s.chunks[0];
        const n = Math.min(out.length - i, chunk.length - s.offset);
        for (let k = 0; k < n; k += 1) out[i + k] += chunk[s.offset + k];
        i += n;
        s.offset += n;
        s.queued -= n;
        if (s.offset >= chunk.length) {
          s.chunks.shift();
          s.offset = 0;
        }
      }
      if (i < out.length) {
        s.playing = false;
        this.underruns += 1;
      }
    }
    for (let c = 1; c < outputs[0].length; c += 1) outputs[0][c].set(out);
    this.counted += out.length;
    if (this.counted >= STATS_EVERY) {
      this.counted = 0;
      if (this.engine) this.engine.postMessage({ t: "stats", underruns: this.underruns, trimmed: this.trimmed });
    }
    return true;
  }
}

registerProcessor("fmm-playout", FmmPlayout);
