import { delaunay, faceFrame } from "./chat-call-face-geometry.js";
import { RIG_POINTS } from "./call-avatar-rig-wire.js";

const INNER_LIPS = [60, 61, 62, 63, 64, 65, 66, 67];
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);

function affine(s0, s1, s2, d0, d1, d2) {
  const den = (s1[0] - s0[0]) * (s2[1] - s0[1]) - (s2[0] - s0[0]) * (s1[1] - s0[1]);
  if (Math.abs(den) < 1e-9) return null;
  const a = ((d1[0] - d0[0]) * (s2[1] - s0[1]) - (d2[0] - d0[0]) * (s1[1] - s0[1])) / den;
  const c = ((d2[0] - d0[0]) * (s1[0] - s0[0]) - (d1[0] - d0[0]) * (s2[0] - s0[0])) / den;
  const b = ((d1[1] - d0[1]) * (s2[1] - s0[1]) - (d2[1] - d0[1]) * (s1[1] - s0[1])) / den;
  const d = ((d2[1] - d0[1]) * (s1[0] - s0[0]) - (d1[1] - d0[1]) * (s2[0] - s0[0])) / den;
  return [a, b, c, d, d0[0] - a * s0[0] - c * s0[1], d0[1] - b * s0[0] - d * s0[1]];
}

function grow(tri, amount) {
  const cx = (tri[0][0] + tri[1][0] + tri[2][0]) / 3;
  const cy = (tri[0][1] + tri[1][1] + tri[2][1]) / 3;
  return tri.map(([x, y]) => {
    const dx = x - cx;
    const dy = y - cy;
    const length = Math.hypot(dx, dy) || 1;
    return [x + (dx / length) * amount, y + (dy / length) * amount];
  });
}

export function puppetTargets(anchors, deltas) {
  const frame = faceFrame(anchors);
  const c = Math.cos(frame.roll);
  const s = Math.sin(frame.roll);
  return anchors.map(([x, y], i) => {
    if (!deltas) return [x, y];
    const dx = deltas[i * 2] * frame.span;
    const dy = deltas[i * 2 + 1] * frame.span;
    return [x + dx * c - dy * s, y + dx * s + dy * c];
  });
}

export function createFacePuppet() {
  let cachedKey = "";
  let triangles = [];
  let border = [];

  function layout(anchors, width, height) {
    const key = `${width}x${height}:${anchors.map(([x, y]) => `${x.toFixed(1)},${y.toFixed(1)}`).join(";")}`;
    if (key === cachedKey) return;
    cachedKey = key;
    border = [];
    const steps = 4;
    for (let i = 0; i < steps; i += 1) {
      const t = i / steps;
      border.push([t * width, 0], [width, t * height], [width - t * width, height], [0, height - t * height]);
    }
    const all = [...anchors, ...border];
    triangles = delaunay(all).filter((tri) => !tri.every((v) => INNER_LIPS.includes(v)));
  }

  return {
    render(context, { image, anchors, deltas = null, width, height, mouth = "#3b1016", teeth = "#efe7da" }) {
      if (!image || !anchors || anchors.length < RIG_POINTS) return false;
      const sourceWidth = image.naturalWidth || image.videoWidth || image.width;
      const sourceHeight = image.naturalHeight || image.videoHeight || image.height;
      if (!sourceWidth || !sourceHeight) return false;
      const scaleX = width / sourceWidth;
      const scaleY = height / sourceHeight;
      const rest = anchors.map(([x, y]) => [x * width, y * height]);
      layout(rest, width, height);
      const moved = [...puppetTargets(rest, deltas), ...border];
      const source = [...rest, ...border];
      context.save();
      context.setTransform(1, 0, 0, 1, 0, 0);
      const lips = INNER_LIPS.map((i) => moved[i]);
      context.fillStyle = mouth;
      context.beginPath();
      lips.forEach(([x, y], i) => (i ? context.lineTo(x, y) : context.moveTo(x, y)));
      context.closePath();
      context.fill();
      context.save();
      context.clip();
      const top = Math.min(...lips.map(([, y]) => y));
      const span = dist(moved[60], moved[64]);
      context.fillStyle = teeth;
      context.fillRect(moved[60][0], top, span, Math.max(1, span * 0.16));
      context.restore();
      for (const tri of triangles) {
        const s = tri.map((v) => source[v]);
        const d = tri.map((v) => moved[v]);
        const m = affine(s[0], s[1], s[2], d[0], d[1], d[2]);
        if (!m) continue;
        const clip = grow(d, 0.6);
        context.save();
        context.beginPath();
        context.moveTo(clip[0][0], clip[0][1]);
        context.lineTo(clip[1][0], clip[1][1]);
        context.lineTo(clip[2][0], clip[2][1]);
        context.closePath();
        context.clip();
        context.setTransform(m[0], m[1], m[2], m[3], m[4], m[5]);
        context.drawImage(image, 0, 0, sourceWidth, sourceHeight, 0, 0, sourceWidth * scaleX, sourceHeight * scaleY);
        context.restore();
      }
      context.restore();
      return true;
    },
  };
}
