import { createNetwork, floatTargets, parseSegmentModel } from "./chat-call-segment.js";
import { HAND_GESTURES } from "./call-avatar-rig-wire.js";

export const HAND_MODEL_BASE = "/assets/hand-tracking/";
export const HAND_MODELS = Object.freeze({ palm: "palm-detection-lite.fmnn", hand: "hand-landmark-lite.fmnn" });

const PALM_SIZE = 192;
const HAND_SIZE = 224;
const PALM_SCORE = 0.55;
const HAND_SCORE = 0.5;
const PALM_SCALE = 2.6;
const PALM_SHIFT = 0.5;
const TRACK_SCALE = 2.0;
const TRACK_SHIFT = 0.1;
const MAX_HANDS = 2;
const REDETECT_MS = 900;

export const HAND_POINTS = Object.freeze({
  wrist: 0,
  thumb: [1, 2, 3, 4],
  index: [5, 6, 7, 8],
  middle: [9, 10, 11, 12],
  ring: [13, 14, 15, 16],
  pinky: [17, 18, 19, 20],
});

export { HAND_GESTURES };

const CROP_VERTEX = `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const CROP_FRAGMENT = `
precision highp float;
uniform sampler2D uCam;
uniform vec2 uCenter;
uniform vec2 uAxisX;
uniform vec2 uAxisY;
uniform vec2 uFrame;
uniform vec2 uRange;
varying vec2 vUv;
void main() {
  vec2 p = uCenter + (vUv.x - 0.5) * uAxisX + (vUv.y - 0.5) * uAxisY;
  vec2 uv = p / uFrame;
  vec3 c = vec3(0.0);
  if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) c = texture2D(uCam, uv).rgb;
  gl_FragColor = vec4(c * uRange.x + uRange.y, 0.0);
}`;

export function palmAnchors() {
  const anchors = [];
  for (const [grid, count] of [[24, 2], [12, 6]]) {
    for (let y = 0; y < grid; y += 1) {
      for (let x = 0; x < grid; x += 1) {
        for (let k = 0; k < count; k += 1) anchors.push([(x + 0.5) / grid, (y + 0.5) / grid]);
      }
    }
  }
  return anchors;
}

function overlap(a, b) {
  const x0 = Math.max(a.cx - a.w / 2, b.cx - b.w / 2);
  const x1 = Math.min(a.cx + a.w / 2, b.cx + b.w / 2);
  const y0 = Math.max(a.cy - a.h / 2, b.cy - b.h / 2);
  const y1 = Math.min(a.cy + a.h / 2, b.cy + b.h / 2);
  const inter = Math.max(0, x1 - x0) * Math.max(0, y1 - y0);
  return inter / (a.w * a.h + b.w * b.h - inter || 1);
}

export function decodePalms(scores, boxes, anchors, { threshold = PALM_SCORE, limit = MAX_HANDS } = {}) {
  const found = [];
  for (let i = 0; i < anchors.length; i += 1) {
    const logit = Math.max(-100, Math.min(100, scores[i]));
    const score = 1 / (1 + Math.exp(-logit));
    if (score < threshold) continue;
    const r = boxes.subarray(i * 18, i * 18 + 18);
    const [ax, ay] = anchors[i];
    found.push({
      score,
      cx: r[0] / PALM_SIZE + ax,
      cy: r[1] / PALM_SIZE + ay,
      w: r[2] / PALM_SIZE,
      h: r[3] / PALM_SIZE,
      keypoints: Array.from({ length: 7 }, (_, k) => [r[4 + k * 2] / PALM_SIZE + ax, r[5 + k * 2] / PALM_SIZE + ay]),
    });
  }
  found.sort((a, b) => b.score - a.score);
  const palms = [];
  const used = new Set();
  for (let i = 0; i < found.length && palms.length < limit; i += 1) {
    if (used.has(i)) continue;
    const group = [];
    for (let j = i; j < found.length; j += 1) {
      if (!used.has(j) && overlap(found[i], found[j]) > 0.3) {
        used.add(j);
        group.push(found[j]);
      }
    }
    const total = group.reduce((sum, box) => sum + box.score, 0);
    const mean = (pick) => group.reduce((sum, box) => sum + pick(box) * box.score, 0) / total;
    palms.push({
      score: found[i].score,
      cx: mean((b) => b.cx),
      cy: mean((b) => b.cy),
      w: mean((b) => b.w),
      h: mean((b) => b.h),
      keypoints: found[i].keypoints.map((_, k) => [mean((b) => b.keypoints[k][0]), mean((b) => b.keypoints[k][1])]),
    });
  }
  return palms;
}

function rotated(cx, cy, size, angle, shift) {
  return { cx: cx + Math.sin(angle) * shift * size, cy: cy - Math.cos(angle) * shift * size, size, angle };
}

export function roiFromPalm(palm, letterbox) {
  const toFrame = ([x, y]) => [(x - letterbox.x) * letterbox.scale, (y - letterbox.y) * letterbox.scale];
  const [cx, cy] = toFrame([palm.cx, palm.cy]);
  const wrist = toFrame(palm.keypoints[0]);
  const middle = toFrame(palm.keypoints[2]);
  const angle = Math.atan2(middle[0] - wrist[0], wrist[1] - middle[1]);
  const box = Math.max(palm.w, palm.h) * letterbox.scale;
  return rotated(cx, cy, box * PALM_SCALE, angle, PALM_SHIFT / PALM_SCALE);
}

export function roiFromHand(points) {
  const wrist = points[0];
  const knuckle = [
    0.25 * (points[5][0] + points[13][0]) + 0.5 * points[9][0],
    0.25 * (points[5][1] + points[13][1]) + 0.5 * points[9][1],
  ];
  const angle = Math.atan2(knuckle[0] - wrist[0], wrist[1] - knuckle[1]);
  const c = Math.cos(-angle);
  const s = Math.sin(-angle);
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  for (const i of [0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18]) {
    const [x, y] = points[i];
    const rx = (x - wrist[0]) * c - (y - wrist[1]) * s;
    const ry = (x - wrist[0]) * s + (y - wrist[1]) * c;
    minX = Math.min(minX, rx);
    maxX = Math.max(maxX, rx);
    minY = Math.min(minY, ry);
    maxY = Math.max(maxY, ry);
  }
  const mx = (minX + maxX) / 2;
  const my = (minY + maxY) / 2;
  const ci = Math.cos(angle);
  const si = Math.sin(angle);
  const size = Math.max(maxX - minX, maxY - minY) * TRACK_SCALE;
  return rotated(wrist[0] + mx * ci - my * si, wrist[1] + mx * si + my * ci, size, angle, TRACK_SHIFT / TRACK_SCALE);
}

const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);

export function fingerStates(points) {
  const palm = Math.max(1e-3, dist(points[0], points[9]));
  const extended = {};
  for (const name of ["index", "middle", "ring", "pinky"]) {
    const [mcp, pip, , tip] = HAND_POINTS[name];
    const reach = dist(points[tip], points[0]) / palm;
    const bend = dist(points[tip], points[mcp]) / Math.max(1e-3, dist(points[pip], points[mcp]) + dist(points[tip], points[pip]));
    extended[name] = reach > 1.45 && bend > 0.82;
  }
  const [, thumbMcp, thumbIp, thumbTip] = HAND_POINTS.thumb;
  const thumbOut = dist(points[thumbTip], points[17]) / palm;
  const thumbStraight = dist(points[thumbTip], points[thumbMcp]) / Math.max(1e-3, dist(points[thumbIp], points[thumbMcp]) + dist(points[thumbTip], points[thumbIp]));
  extended.thumb = thumbOut > 1.15 && thumbStraight > 0.8;
  return { extended, palm };
}

export function fingerCurls(points) {
  return ["thumb", "index", "middle", "ring", "pinky"].map((name) => {
    const [a, b, c, d] = HAND_POINTS[name];
    const chain = dist(points[a], points[b]) + dist(points[b], points[c]) + dist(points[c], points[d]);
    const straight = dist(points[a], points[d]) / Math.max(1e-3, chain);
    return Math.max(0, Math.min(1, (1 - straight) / 0.6));
  });
}

export function classifyHand(points) {
  if (!points || points.length < 21) return null;
  const { extended, palm } = fingerStates(points);
  const fingers = ["index", "middle", "ring", "pinky"].filter((name) => extended[name]);
  const thumbTip = points[4];
  const thumbBase = points[2];
  const knuckles = [points[5], points[9], points[13], points[17]];
  const knuckleY = knuckles.reduce((sum, p) => sum + p[1], 0) / 4;
  const thumbRise = (knuckleY - thumbTip[1]) / palm;
  const thumbAxis = (thumbBase[1] - thumbTip[1]) / Math.max(1e-3, dist(thumbTip, thumbBase));
  if (!fingers.length && extended.thumb) {
    if (thumbRise > 0.55 && thumbAxis > 0.6) return "thumbs_up";
    if (thumbRise < -0.55 && thumbAxis < -0.6) return "thumbs_down";
  }
  const pinch = dist(points[4], points[8]) / palm;
  if (pinch < 0.32 && extended.middle && extended.ring && extended.pinky) return "ok";
  if (fingers.length === 4 && extended.thumb) return "open_palm";
  if (fingers.length === 4) return "open_palm";
  if (fingers.length === 2 && extended.index && extended.middle) return "victory";
  if (fingers.length === 2 && extended.index && extended.pinky) return "rock";
  if (fingers.length === 1 && extended.index && points[8][1] < points[5][1]) return "point_up";
  if (!fingers.length && !extended.thumb) return "fist";
  return null;
}

export function classifyHands(hands) {
  if (hands.length === 2) {
    const [a, b] = hands;
    const span = Math.max(a.palm || 1, b.palm || 1);
    if (dist(a.points[4], b.points[4]) / span < 0.6 && dist(a.points[8], b.points[8]) / span < 0.6
      && a.points[8][1] < a.points[4][1] && b.points[8][1] < b.points[4][1]) return "heart";
    if (dist(a.points[9], b.points[9]) / span < 0.9 && a.gesture === "open_palm" && b.gesture === "open_palm") return "clap";
  }
  return null;
}

export function createGestureDetector({ holdMs = 450, cooldownMs = 2500, waveSwings = 3, waveWindowMs = 1400 } = {}) {
  let current = null;
  let since = 0;
  let fired = null;
  let lastFire = -Infinity;
  const swings = [];
  let lastX = null;
  let direction = 0;
  return {
    update(gesture, time, palmX = null) {
      if (gesture === "open_palm" && Number.isFinite(palmX)) {
        if (lastX !== null) {
          const step = palmX - lastX;
          const dir = Math.abs(step) > 0.012 ? Math.sign(step) : 0;
          if (dir && dir !== direction) {
            if (direction) swings.push(time);
            direction = dir;
          }
        }
        lastX = palmX;
        while (swings.length && time - swings[0] > waveWindowMs) swings.shift();
        if (swings.length >= waveSwings) {
          swings.length = 0;
          gesture = "wave";
          current = "wave";
          since = time - holdMs;
        }
      } else {
        lastX = null;
        direction = 0;
        swings.length = 0;
      }
      if (gesture !== current) {
        current = gesture;
        since = time;
        if (gesture !== fired) fired = null;
        return null;
      }
      if (!gesture || gesture === "fist" || gesture === fired) return null;
      if (time - since < holdMs || time - lastFire < cooldownMs) return null;
      fired = gesture;
      lastFire = time;
      return gesture;
    },
    reset() {
      current = null;
      fired = null;
      swings.length = 0;
      lastX = null;
    },
  };
}

export async function loadHandModels({ fetchImpl = globalThis.fetch?.bind(globalThis), base = HAND_MODEL_BASE } = {}) {
  const out = {};
  await Promise.all(Object.entries(HAND_MODELS).map(async ([name, file]) => {
    try {
      const response = await fetchImpl(base + file);
      if (response.ok) out[name] = parseSegmentModel(await response.arrayBuffer());
    } catch (_) {}
  }));
  return out;
}

export function createHandTracker(gl, models, format = floatTargets(gl)) {
  if (!format || !models?.palm || !models?.hand) return null;
  const detector = createNetwork(gl, models.palm, format);
  const landmarks = createNetwork(gl, models.hand, format);
  const anchors = palmAnchors();
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const program = gl.createProgram();
  for (const [type, source] of [[gl.VERTEX_SHADER, CROP_VERTEX], [gl.FRAGMENT_SHADER, CROP_FRAGMENT]]) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    gl.attachShader(program, shader);
  }
  gl.bindAttribLocation(program, 0, "aPos");
  gl.linkProgram(program);
  const u = Object.fromEntries(["uCam", "uCenter", "uAxisX", "uAxisY", "uFrame", "uRange"].map((name) => [name, gl.getUniformLocation(program, name)]));
  let rois = [];
  let lastDetect = -Infinity;

  function crop(camera, net, size, region, width, height, range) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, net.input.framebuffer);
    gl.viewport(0, 0, size, size);
    gl.useProgram(program);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, camera);
    gl.uniform1i(u.uCam, 0);
    const c = Math.cos(region.angle);
    const s = Math.sin(region.angle);
    gl.uniform2f(u.uCenter, region.cx, region.cy);
    gl.uniform2f(u.uAxisX, c * region.size, s * region.size);
    gl.uniform2f(u.uAxisY, -s * region.size, c * region.size);
    gl.uniform2f(u.uFrame, width, height);
    gl.uniform2f(u.uRange, range[0], range[1]);
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.disable(gl.BLEND);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.disableVertexAttribArray(0);
  }

  function readChannels(net, group) {
    const parts = group.map((index) => {
      const r = net.read(index);
      const [h, w, channels] = r.shape;
      const values = new Float32Array(h * w * channels);
      let at = 0;
      for (let y = 0; y < h; y += 1) {
        for (let x = 0; x < w; x += 1) {
          for (let c = 0; c < channels; c += 1) values[at++] = r.pixels[((y * r.width) + (c >> 2) * w + x) * 4 + (c & 3)];
        }
      }
      return values;
    });
    const out = new Float32Array(parts.reduce((sum, part) => sum + part.length, 0));
    let at = 0;
    for (const part of parts) {
      out.set(part, at);
      at += part.length;
    }
    return out;
  }

  function detect(camera, width, height) {
    const side = Math.max(width, height);
    const letterbox = { scale: side, x: (side - width) / 2 / side, y: (side - height) / 2 / side };
    crop(camera, detector, PALM_SIZE, { cx: width / 2, cy: height / 2, size: side, angle: 0 }, width, height, [1, 0]);
    detector.run();
    const [boxGroup, scoreGroup] = models.palm.outputs;
    const palms = decodePalms(readChannels(detector, scoreGroup), readChannels(detector, boxGroup), anchors);
    return palms.map((palm) => roiFromPalm(palm, letterbox));
  }

  function landmark(camera, roi, width, height) {
    crop(camera, landmarks, HAND_SIZE, roi, width, height, [1, 0]);
    landmarks.run();
    const [pointGroup, presenceGroup, sideGroup] = models.hand.outputs;
    const presence = readChannels(landmarks, presenceGroup)[0];
    const score = presence > 1 || presence < 0 ? 1 / (1 + Math.exp(-presence)) : presence;
    if (score < HAND_SCORE) return null;
    const raw = readChannels(landmarks, pointGroup);
    const side = readChannels(landmarks, sideGroup)[0];
    const c = Math.cos(roi.angle);
    const s = Math.sin(roi.angle);
    const points = [];
    for (let i = 0; i < raw.length; i += 3) {
      const x = (raw[i] / HAND_SIZE - 0.5) * roi.size;
      const y = (raw[i + 1] / HAND_SIZE - 0.5) * roi.size;
      points.push([roi.cx + x * c - y * s, roi.cy + x * s + y * c, (raw[i + 2] / HAND_SIZE) * roi.size]);
    }
    return { points, score, right: side > 0.5 };
  }

  return {
    track(camera, width, height, time = performance.now()) {
      if (rois.length < MAX_HANDS && time - lastDetect > REDETECT_MS) {
        lastDetect = time;
        for (const found of detect(camera, width, height)) {
          if (rois.length >= MAX_HANDS) break;
          if (!rois.some((roi) => Math.hypot(roi.cx - found.cx, roi.cy - found.cy) < roi.size * 0.35)) rois.push(found);
        }
      }
      const hands = [];
      const next = [];
      for (const roi of rois) {
        const hand = landmark(camera, roi, width, height);
        if (!hand) continue;
        const follow = roiFromHand(hand.points);
        if (next.some((other) => Math.hypot(other.cx - follow.cx, other.cy - follow.cy) < other.size * 0.35)) continue;
        next.push(follow);
        const { palm } = fingerStates(hand.points);
        hands.push({ ...hand, palm, gesture: classifyHand(hand.points), center: hand.points[9] });
      }
      rois = next;
      return { hands, gesture: classifyHands(hands) || hands.find((hand) => hand.gesture && hand.gesture !== "fist")?.gesture || null };
    },
    reset() {
      rois = [];
      lastDetect = -Infinity;
    },
    dispose() {
      detector.dispose();
      landmarks.dispose();
      gl.deleteProgram(program);
      gl.deleteBuffer(quad);
    },
  };
}
