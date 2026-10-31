export const VOICE_CHART_SAMPLE_MS = 100;
export const VOICE_CHART_WINDOW = 300;
export const VOICE_CHART_WIDTH = 248;
export const VOICE_CHART_HEIGHT = 112;
export const VOICE_CHART_COLORS = Object.freeze({
  line: "#2563eb",
  fill: "rgba(37, 99, 235, 0.14)",
  grid: "#e5e7eb",
  text: "#374151",
  muted: "#6b7280",
  surface: "#ffffff",
});

const LABEL_WIDTH = 62;
const VALUE_WIDTH = 58;
const PAD = 6;

const round = (value) => Math.round(Number(value) || 0);

export const VOICE_CHART_SERIES = Object.freeze([
  { key: "fps", label: "Frames", floor: 30, fill: false, format: (value) => `${round(value)} fps` },
  { key: "level", label: "Mic level", floor: 0.25, fill: true, format: (value) => `${round(value * 100)}%` },
  { key: "trackingMs", label: "Tracking", floor: 20, fill: false, format: (value) => `${round(value)} ms` },
  { key: "rttMs", label: "Round trip", floor: 80, fill: false, format: (value) => `${round(value)} ms` },
]);

function finite(value) {
  return value === null || value === undefined || !Number.isFinite(Number(value)) ? null : Number(value);
}

export function voiceChartSample(input = {}) {
  return {
    at: Number(input.at) || 0,
    fps: finite(input.fps),
    level: finite(input.level) === null ? null : Math.max(0, Math.min(1, Number(input.level))),
    trackingMs: finite(input.trackingMs),
    rttMs: finite(input.rttMs),
  };
}

export function pushVoiceSample(samples, sample, limit = VOICE_CHART_WINDOW) {
  samples.push(voiceChartSample(sample));
  if (samples.length > limit) samples.splice(0, samples.length - limit);
  return samples;
}

export function seriesCeiling(samples, key, floor) {
  let top = 0;
  for (const sample of samples) {
    const value = sample[key];
    if (value !== null && value > top) top = value;
  }
  return Math.max(floor, top * 1.15);
}

export function latestValue(samples, key) {
  for (let index = samples.length - 1; index >= 0; index -= 1) {
    if (samples[index][key] !== null) return samples[index][key];
  }
  return null;
}

export function voiceChartSummary(samples, series = VOICE_CHART_SERIES) {
  return series
    .map((entry) => {
      const value = latestValue(samples, entry.key);
      return `${entry.label} ${value === null ? "not measured" : entry.format(value)}`;
    })
    .join(", ");
}

export function drawVoiceChart(context, samples, {
  width = VOICE_CHART_WIDTH,
  height = VOICE_CHART_HEIGHT,
  ratio = 1,
  series = VOICE_CHART_SERIES,
  colors = VOICE_CHART_COLORS,
  window = VOICE_CHART_WINDOW,
} = {}) {
  if (!context) return false;
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  context.fillStyle = colors.surface;
  context.fillRect(0, 0, width, height);
  const rowHeight = (height - PAD * 2) / series.length;
  const plotLeft = LABEL_WIDTH;
  const plotWidth = width - LABEL_WIDTH - VALUE_WIDTH;
  const step = plotWidth / Math.max(1, window - 1);
  const offset = window - samples.length;
  context.font = "600 10px system-ui, sans-serif";
  context.textBaseline = "middle";
  series.forEach((entry, row) => {
    const top = PAD + row * rowHeight;
    const bottom = top + rowHeight - 4;
    const middle = top + rowHeight / 2;
    const ceiling = seriesCeiling(samples, entry.key, entry.floor);
    const y = (value) => bottom - (value / ceiling) * (bottom - top - 2);
    context.textAlign = "left";
    context.fillStyle = colors.muted;
    context.fillText(entry.label, PAD, middle);
    context.strokeStyle = colors.grid;
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(plotLeft, bottom + 0.5);
    context.lineTo(plotLeft + plotWidth, bottom + 0.5);
    context.stroke();
    let open = false;
    let last = null;
    if (entry.fill) {
      context.fillStyle = colors.fill;
      context.beginPath();
      let started = -1;
      samples.forEach((sample, index) => {
        const value = sample[entry.key];
        const x = plotLeft + (offset + index) * step;
        if (value === null) return;
        if (started < 0) {
          started = x;
          context.moveTo(x, bottom);
        }
        context.lineTo(x, y(value));
        last = x;
      });
      if (started >= 0) {
        context.lineTo(last, bottom);
        context.closePath();
        context.fill();
      }
      last = null;
    }
    context.strokeStyle = colors.line;
    context.lineWidth = 2;
    context.lineJoin = "round";
    context.lineCap = "round";
    context.beginPath();
    samples.forEach((sample, index) => {
      const value = sample[entry.key];
      const x = plotLeft + (offset + index) * step;
      if (value === null) {
        open = false;
        return;
      }
      if (open) context.lineTo(x, y(value));
      else context.moveTo(x, y(value));
      open = true;
      last = [x, y(value)];
    });
    context.stroke();
    if (last) {
      context.fillStyle = colors.surface;
      context.beginPath();
      context.arc(last[0], last[1], 4, 0, Math.PI * 2);
      context.fill();
      context.fillStyle = colors.line;
      context.beginPath();
      context.arc(last[0], last[1], 2.6, 0, Math.PI * 2);
      context.fill();
    }
    const value = latestValue(samples, entry.key);
    context.textAlign = "right";
    context.fillStyle = value === null ? colors.muted : colors.text;
    context.fillText(value === null ? "—" : entry.format(value), width - PAD, middle);
  });
  return true;
}
