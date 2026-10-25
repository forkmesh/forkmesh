import { createFaceMesh } from "./chat-call-face-mesh.js";
import { createGestureDetector, createHandTracker, fingerCurls, loadHandModels } from "./chat-call-hand-tracker.js";
import { floatTargets, sharedSegmentModels } from "./chat-call-segment.js";
import { clampValue, createLandmarkSmoother, faceFrame, mouthGap, projectPose, solvePose } from "./chat-call-face-geometry.js";
import { createExpressionProbe } from "./chat-call-face-expression.js";
import { FACE_TEMPLATE_3D } from "./chat-call-face-template.js";
import { RIG_POINTS } from "./call-avatar-rig-wire.js";

const NEUTRAL_RATE = 0.006;
const OPEN_DECAY = 0.0005;
const HAND_EVERY = 2;
const HAND_SLOW_EVERY = 4;
const HAND_SLOW_MS = 30;
const LEFT_EYE = [36, 37, 38, 39, 40, 41];
const RIGHT_EYE = [42, 43, 44, 45, 46, 47];
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1]);

function eyeAspect(points, eye) {
  const p = eye.map((i) => points[i]);
  return (dist(p[1], p[5]) + dist(p[2], p[4])) / (2 * Math.max(1e-6, dist(p[0], p[3])));
}

const LIP_PAIRS = [[61, 67], [62, 66], [63, 65]];

export function closedMouth(residual) {
  const out = Float32Array.from(residual);
  for (const [top, bottom] of LIP_PAIRS) {
    const middle = (residual[top * 2 + 1] + residual[bottom * 2 + 1]) / 2;
    out[top * 2 + 1] = middle;
    out[bottom * 2 + 1] = middle;
  }
  return out;
}

export function createFaceRigSolver() {
  let neutral = null;
  let neutralWidth = 0;
  const open = [0.25, 0.25];
  return {
    reset() {
      neutral = null;
      neutralWidth = 0;
      open[0] = 0.25;
      open[1] = 0.25;
    },
    solve(points, reading = null) {
      if (!points || points.length < RIG_POINTS) return null;
      const pose = solvePose(FACE_TEMPLATE_3D, points);
      if (!pose || !Number.isFinite(pose.scale) || pose.scale <= 0) return null;
      const c = Math.cos(pose.roll);
      const s = Math.sin(pose.roll);
      const residual = new Float32Array(RIG_POINTS * 2);
      for (let i = 0; i < RIG_POINTS; i += 1) {
        const [px, py] = projectPose(pose, FACE_TEMPLATE_3D[i]);
        const rx = (points[i][0] - px) / pose.scale;
        const ry = (points[i][1] - py) / pose.scale;
        residual[i * 2] = rx * c + ry * s;
        residual[i * 2 + 1] = -rx * s + ry * c;
      }
      const frame = faceFrame(points);
      const ears = [eyeAspect(points, LEFT_EYE), eyeAspect(points, RIGHT_EYE)];
      ears.forEach((ear, k) => {
        open[k] = Math.max(0.12, Math.max(open[k] * (1 - OPEN_DECAY), ear));
      });
      const lids = ears.map((ear, k) => clampValue((1 - ear / open[k] - 0.3) / 0.45, 0, 1));
      const gap = mouthGap(points);
      const width = dist(points[48], points[54]) / frame.span;
      const calm = gap < 0.08 && lids[0] < 0.3 && lids[1] < 0.3 && Math.abs(pose.yaw) < 0.35 && Math.abs(pose.pitch) < 0.35;
      if (calm && !neutral) {
        neutral = closedMouth(residual);
        neutralWidth = width;
      } else if (calm) {
        const target = closedMouth(residual);
        for (let i = 0; i < target.length; i += 1) neutral[i] += (target[i] - neutral[i]) * NEUTRAL_RATE;
        neutralWidth += (width - neutralWidth) * NEUTRAL_RATE;
      }
      const reference = neutral || closedMouth(residual);
      const deltas = new Float32Array(RIG_POINTS * 2);
      for (let i = 0; i < deltas.length; i += 1) deltas[i] = residual[i] - reference[i];
      const brow = (from, to) => {
        let sum = 0;
        for (let i = from; i <= to; i += 1) sum += deltas[i * 2 + 1];
        return clampValue(-(sum / (to - from + 1)) / 0.12, -1, 1);
      };
      const ratio = neutralWidth ? width / Math.max(0.2, neutralWidth) : 1;
      const look = reading?.look || [[0, 0], [0, 0]];
      const probeClose = reading?.close || [0, 0];
      return {
        yaw: pose.yaw,
        pitch: pose.pitch,
        roll: pose.roll,
        deltas,
        lidLeft: Math.max(lids[0], probeClose[0] || 0),
        lidRight: Math.max(lids[1], probeClose[1] || 0),
        gazeX: clampValue(((look[0]?.[0] || 0) + (look[1]?.[0] || 0)) / 2, -1, 1),
        gazeY: clampValue(((look[0]?.[1] || 0) + (look[1]?.[1] || 0)) / 2, -1, 1),
        mouth: clampValue(gap / 0.5, 0, 1),
        smile: clampValue((ratio - 1.03) / 0.16, 0, 1),
        browLeft: brow(17, 21),
        browRight: brow(22, 26),
        round: clampValue((0.94 - ratio) / 0.16, 0, 1) * clampValue(gap / 0.06, 0.3, 1),
        tongue: clampValue(reading?.tongue || 0, 0, 1),
        frame,
      };
    },
  };
}

export function handRig(hand, frame) {
  const center = hand.points[9];
  const x = center[0] - frame.mid[0];
  const y = center[1] - frame.mid[1];
  const along = (x * frame.ux[0] + y * frame.ux[1]) / frame.span;
  const down = (x * frame.uy[0] + y * frame.uy[1]) / frame.span;
  const wrist = hand.points[0];
  const roll = Math.atan2(center[0] - wrist[0], wrist[1] - center[1]) - frame.roll;
  return {
    side: along < 0 ? "right" : "left",
    x: along,
    y: down,
    curl: fingerCurls(hand.points),
    roll,
    gesture: hand.gesture || null,
  };
}

function uploadTexture(gl, texture, source) {
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
}

function makeTexture(gl) {
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  for (const [key, value] of [[gl.TEXTURE_MIN_FILTER, gl.LINEAR], [gl.TEXTURE_MAG_FILTER, gl.LINEAR], [gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE], [gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE]]) {
    gl.texParameteri(gl.TEXTURE_2D, key, value);
  }
  return texture;
}

async function trackingContext(documentRef, { hands = true } = {}) {
  const canvas = documentRef.createElement("canvas");
  canvas.width = 4;
  canvas.height = 4;
  const gl = canvas.getContext("webgl", { premultipliedAlpha: false, preserveDrawingBuffer: false, antialias: false });
  if (!gl) return null;
  const format = floatTargets(gl);
  if (!format) return null;
  const [faceModels, handModels] = await Promise.all([
    sharedSegmentModels("face"),
    hands ? loadHandModels().catch(() => ({})) : Promise.resolve({}),
  ]);
  const mesh = createFaceMesh(gl, faceModels, format);
  if (!mesh) return null;
  return { canvas, gl, format, mesh, handTracker: hands ? createHandTracker(gl, handModels, format) : null, texture: makeTexture(gl) };
}

export async function detectFaceAnchors(image, { documentRef = globalThis.document } = {}) {
  const context = await trackingContext(documentRef, { hands: false });
  if (!context) return null;
  try {
    const width = image.naturalWidth || image.videoWidth || image.width;
    const height = image.naturalHeight || image.videoHeight || image.height;
    if (!width || !height) return null;
    uploadTexture(context.gl, context.texture, image);
    let found = null;
    for (let i = 0; i < 2 && !found?.points; i += 1) found = context.mesh.track(context.texture, width, height);
    if (!found?.points || found.points.length < RIG_POINTS) return null;
    return found.points.slice(0, RIG_POINTS).map(([x, y]) => [clampValue(x / width, 0, 1), clampValue(y / height, 0, 1)]);
  } catch (_) {
    return null;
  } finally {
    context.mesh.dispose();
    context.handTracker?.dispose();
    context.gl.getExtension("WEBGL_lose_context")?.loseContext();
  }
}

export function createAvatarCapture({
  stream,
  documentRef = globalThis.document,
  scope = globalThis,
  hands = true,
  onFrame = () => {},
  onGesture = () => {},
} = {}) {
  const video = documentRef.createElement("video");
  video.muted = true;
  video.playsInline = true;
  video.autoplay = true;
  video.srcObject = stream;
  const solver = createFaceRigSolver();
  const smoother = createLandmarkSmoother();
  const probe = createExpressionProbe({ documentRef });
  const gestures = createGestureDetector();
  const stats = { frames: 0, frameMs: 0, faceMs: 0, handMs: 0, fps: 0, tracked: false, hands: 0, startedAt: scope.performance.now() };
  let context = null;
  let stopped = false;
  let handsOn = hands;
  let frameIndex = 0;
  let lastHands = [];
  let lastFace = null;
  let pending = 0;
  let fpsWindow = [];
  let timer = 0;

  function step(now) {
    if (stopped || !context || video.readyState < 2) return;
    const width = video.videoWidth;
    const height = video.videoHeight;
    if (!width || !height) return;
    frameIndex += 1;
    fpsWindow.push(now);
    while (fpsWindow.length && now - fpsWindow[0] > 1000) fpsWindow.shift();
    stats.fps = fpsWindow.length;
    const t0 = scope.performance.now();
    uploadTexture(context.gl, context.texture, video);
    const found = context.mesh.track(context.texture, width, height);
    const t1 = scope.performance.now();
    stats.faceMs = stats.faceMs * 0.9 + (t1 - t0) * 0.1;
    let face = null;
    if (found?.points?.length >= RIG_POINTS) {
      const points = smoother.smooth(found.points, now);
      let reading = null;
      try {
        reading = probe.measure(video, points, now, { lids: true });
      } catch (_) {
        reading = null;
      }
      face = solver.solve(points, reading);
    } else {
      smoother.reset();
    }
    stats.tracked = Boolean(face);
    if (face) lastFace = face;
    const handEvery = stats.handMs > HAND_SLOW_MS ? HAND_SLOW_EVERY : HAND_EVERY;
    if (handsOn && context.handTracker && frameIndex % handEvery === 0) {
      const h0 = scope.performance.now();
      const result = context.handTracker.track(context.texture, width, height, now);
      stats.handMs = stats.handMs * 0.9 + (scope.performance.now() - h0) * 0.1;
      const frame = face?.frame || lastFace?.frame || null;
      lastHands = frame ? result.hands.map((hand) => handRig(hand, frame)) : [];
      stats.hands = result.hands.length;
      const palm = result.hands[0]?.center;
      const fired = gestures.update(result.gesture, now, palm ? palm[0] / width : null);
      if (fired) onGesture(fired);
    } else if (!handsOn) {
      lastHands = [];
    }
    stats.frames += 1;
    stats.frameMs += scope.performance.now() - t0;
    if (face) {
      const { frame, ...rest } = face;
      onFrame({ ...rest, hands: lastHands, tracked: true, time: now });
    } else {
      onFrame({ tracked: false, hands: lastHands, time: now });
    }
  }

  function schedule() {
    if (stopped) return;
    if (typeof video.requestVideoFrameCallback === "function") {
      pending = video.requestVideoFrameCallback((now) => {
        try {
          step(now);
        } finally {
          schedule();
        }
      });
    } else {
      timer = scope.setTimeout(() => {
        try {
          step(scope.performance.now());
        } finally {
          schedule();
        }
      }, 1000 / 24);
    }
  }

  const ready = (async () => {
    context = await trackingContext(documentRef, { hands });
    if (!context || stopped) return false;
    try {
      await video.play();
    } catch (_) {}
    schedule();
    return true;
  })();

  return {
    ready,
    video,
    stats: () => ({ ...stats, handsOn }),
    setHands(on) {
      handsOn = Boolean(on);
      if (!handsOn) gestures.reset();
    },
    stop() {
      stopped = true;
      if (pending && typeof video.cancelVideoFrameCallback === "function") video.cancelVideoFrameCallback(pending);
      if (timer) scope.clearTimeout(timer);
      video.srcObject = null;
      if (context) {
        context.mesh.dispose();
        context.handTracker?.dispose();
        context.gl.getExtension("WEBGL_lose_context")?.loseContext();
        context = null;
      }
    },
  };
}
