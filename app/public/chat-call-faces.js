import {
  buildFaceMesh,
  buildTemplate,
  canonicalShape,
  clampValue,
  createPointSmoother,
  faceFrame,
  fitTemplate,
  landmarksFromRegions,
  OneEuroFilter,
  pointsFromFlat,
} from "./chat-call-face-geometry.js";
import { createFaceTracker } from "./chat-call-face-tracker.js";
import { createFaceRenderer, faceWebGL } from "./chat-call-face-render.js";

const FRAME_RATE = 24;
const FRAME_MS = Math.round(1000 / FRAME_RATE);
const MAX_SIDE = 960;
const HOST_SIDE = 480;
const HOST_IN_FLIGHT = 2;
const HOST_FAILURES = 3;
const HOLD_MS = 500;
const FADE_IN = 0.3;
const FADE_OUT = 0.2;
const PHOTO_SIZE = 512;

export const CALL_FACE_STORAGE_KEY = "forkmesh.chat.callFace";
export const CALL_FACE_PHOTO_BASE = "/assets/call-faces/";

export const CALL_FACES = Object.freeze([
  Object.freeze({
    id: "maya",
    label: "Maya",
    points: Object.freeze([
      80, 167, 79, 218, 80, 271, 84, 319, 95, 365, 113, 405, 145, 439, 196, 465, 256, 477, 320, 467,
      378, 442, 419, 407, 444, 365, 458, 319, 465, 271, 470, 218, 471, 167, 111, 108, 131, 90, 157, 84,
      186, 86, 212, 92, 303, 93, 329, 84, 361, 81, 396, 90, 419, 108, 259, 151, 260, 186, 262, 221,
      259, 253, 218, 272, 236, 282, 259, 285, 282, 282, 303, 272, 119, 154, 143, 143, 177, 143, 207, 160,
      180, 166, 145, 164, 305, 160, 332, 140, 364, 140, 396, 151, 367, 164, 332, 166, 160, 332, 192, 317,
      227, 313, 256, 316, 288, 313, 329, 317, 372, 329, 343, 367, 303, 393, 256, 399, 212, 393, 180, 367,
      169, 335, 209, 329, 256, 330, 305, 329, 364, 332, 305, 380, 256, 383, 209, 380,
    ]),
  }),
  Object.freeze({
    id: "theo",
    label: "Theo",
    points: Object.freeze([
      41, 167, 41, 218, 45, 271, 51, 319, 61, 365, 79, 405, 113, 436, 175, 455, 255, 461, 335, 455,
      387, 436, 407, 405, 428, 365, 441, 319, 445, 271, 448, 218, 449, 167, 87, 118, 122, 103, 157, 97,
      192, 97, 224, 105, 300, 103, 332, 97, 367, 97, 401, 103, 425, 118, 259, 151, 260, 188, 262, 224,
      263, 259, 215, 275, 237, 282, 262, 285, 288, 282, 311, 275, 121, 153, 145, 141, 179, 143, 211, 159,
      180, 163, 145, 162, 307, 159, 332, 143, 365, 143, 396, 153, 367, 162, 332, 163, 169, 337, 196, 331,
      230, 325, 256, 327, 282, 325, 316, 329, 346, 336, 322, 349, 299, 361, 256, 367, 213, 361, 192, 349,
      175, 338, 211, 342, 256, 345, 302, 342, 340, 337, 302, 343, 256, 346, 211, 343,
    ]),
  }),
  Object.freeze({
    id: "kofi",
    label: "Kofi",
    points: Object.freeze([
      26, 167, 26, 218, 29, 271, 36, 319, 49, 365, 70, 405, 102, 436, 169, 458, 256, 465, 340, 458,
      401, 436, 423, 405, 438, 365, 447, 319, 451, 271, 452, 218, 454, 167, 96, 108, 125, 95, 157, 87,
      189, 89, 221, 96, 300, 96, 332, 87, 367, 87, 401, 96, 431, 111, 259, 151, 260, 186, 262, 221,
      260, 256, 221, 276, 239, 284, 262, 287, 288, 282, 308, 273, 119, 150, 145, 138, 177, 140, 212, 159,
      180, 161, 145, 161, 305, 159, 332, 140, 364, 138, 396, 150, 367, 161, 332, 163, 166, 337, 204, 326,
      230, 321, 256, 324, 285, 321, 317, 326, 349, 335, 326, 352, 300, 367, 256, 372, 215, 367, 186, 355,
      171, 339, 211, 343, 256, 346, 302, 343, 343, 336, 302, 345, 256, 348, 211, 345,
    ]),
  }),
  Object.freeze({
    id: "ingrid",
    label: "Ingrid",
    look: Object.freeze({ forehead: 0.2, brows: 0 }),
    points: Object.freeze([
      74, 167, 76, 218, 80, 271, 89, 319, 105, 362, 128, 397, 160, 425, 204, 445, 257, 454, 313, 445,
      356, 425, 390, 397, 415, 362, 433, 319, 444, 271, 448, 218, 449, 167, 99, 118, 125, 108, 154, 103,
      186, 103, 215, 108, 303, 108, 332, 103, 364, 103, 396, 108, 419, 118, 259, 151, 260, 186, 262, 221,
      262, 257, 218, 276, 239, 284, 262, 287, 288, 284, 308, 276, 116, 153, 143, 140, 177, 141, 209, 157,
      177, 166, 145, 166, 305, 157, 335, 140, 367, 140, 396, 151, 367, 166, 335, 167, 169, 337, 198, 329,
      230, 321, 256, 324, 285, 321, 320, 327, 346, 333, 329, 353, 300, 372, 256, 380, 215, 372, 189, 356,
      175, 337, 209, 342, 256, 345, 303, 342, 340, 335, 303, 343, 256, 346, 209, 343,
    ]),
  }),
  Object.freeze({
    id: "ren",
    label: "Ren",
    points: Object.freeze([
      74, 167, 70, 218, 70, 271, 74, 319, 84, 365, 102, 405, 134, 442, 182, 474, 247, 502, 313, 483,
      356, 454, 385, 417, 404, 377, 417, 330, 423, 282, 426, 231, 428, 175, 102, 111, 122, 97, 154, 90,
      189, 90, 218, 99, 303, 96, 329, 87, 361, 86, 390, 90, 404, 102, 262, 151, 265, 186, 268, 221,
      273, 259, 230, 276, 247, 284, 271, 288, 294, 284, 314, 273, 113, 157, 140, 143, 175, 143, 209, 160,
      175, 167, 143, 167, 305, 157, 329, 140, 361, 138, 390, 148, 364, 163, 332, 166, 157, 355, 186, 337,
      221, 332, 256, 333, 288, 330, 320, 335, 337, 340, 326, 387, 297, 416, 256, 425, 215, 416, 180, 393,
      166, 356, 204, 351, 256, 352, 303, 349, 332, 343, 300, 393, 256, 399, 209, 393,
    ]),
  }),
]);

let template = null;
let mesh = null;

export function faceTemplate() {
  if (!template) template = buildTemplate(CALL_FACES.map(facePoints));
  return template;
}

export function faceMesh() {
  if (!mesh) mesh = buildFaceMesh(faceTemplate());
  return mesh;
}

export function facePoints(face) {
  return canonicalShape(pointsFromFlat(face.points));
}

export function callFaceById(id) {
  return CALL_FACES.find((face) => face.id === id) || null;
}

export function callFacePhotoUrl(face) {
  return globalThis.FmCallFacePhotos?.[face.id] || `${CALL_FACE_PHOTO_BASE}${face.id}.jpg`;
}

const supportCache = new WeakMap();

export function callFacesSupported(documentRef = globalThis.document) {
  if (!documentRef) return false;
  if (supportCache.has(documentRef)) return supportCache.get(documentRef);
  const canvas = documentRef.createElement?.("canvas");
  let ok = Boolean(canvas && typeof canvas.captureStream === "function");
  if (ok) {
    const gl = faceWebGL(canvas);
    ok = Boolean(gl);
    gl?.getExtension?.("WEBGL_lose_context")?.loseContext();
  }
  supportCache.set(documentRef, ok);
  return ok;
}

export function outputSize(width, height, limit = MAX_SIDE) {
  const w = width || 640;
  const h = height || 480;
  const scale = Math.min(1, limit / Math.max(w, h));
  return [Math.max(2, Math.round(w * scale / 2) * 2), Math.max(2, Math.round(h * scale / 2) * 2)];
}

export function createAnchorSmoother() {
  const filters = {
    eye: Array.from({ length: 4 }, () => new OneEuroFilter(1.5, 1.5)),
    mouth: Array.from({ length: 2 }, () => new OneEuroFilter(1.5, 1.5)),
    open: new OneEuroFilter(3, 0.5),
    yaw: new OneEuroFilter(0.6, 0.3),
  };
  let yaw = 0;
  return {
    reset() {
      for (const filter of [...filters.eye, ...filters.mouth, filters.open, filters.yaw]) filter.reset();
      yaw = 0;
    },
    smooth(anchors, time) {
      const span = Math.hypot(anchors.eyeR[0] - anchors.eyeL[0], anchors.eyeR[1] - anchors.eyeL[1]) || 1;
      const [a, b, c, d] = filters.eye;
      if (anchors.yaw !== null && anchors.yaw !== undefined) yaw = filters.yaw.filter(anchors.yaw, time);
      return {
        eyeL: [a.filter(anchors.eyeL[0], time, span), b.filter(anchors.eyeL[1], time, span)],
        eyeR: [c.filter(anchors.eyeR[0], time, span), d.filter(anchors.eyeR[1], time, span)],
        mouth: [
          filters.mouth[0].filter(anchors.mouth[0], time, span),
          filters.mouth[1].filter(anchors.mouth[1], time, span),
        ],
        open: clampValue(filters.open.filter(anchors.open || 0, time), 0, 1),
        yaw,
      };
    },
  };
}

export function canvasGrabber(documentRef) {
  const canvases = new Map();
  return (source, rect, width, height) => {
    const key = `${width}x${height}`;
    let entry = canvases.get(key);
    if (!entry) {
      const canvas = documentRef.createElement("canvas");
      canvas.width = width;
      canvas.height = height;
      entry = { canvas, ctx: canvas.getContext("2d", { willReadFrequently: true }) };
      canvases.set(key, entry);
    }
    const { ctx } = entry;
    const k = rect.side / width;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, width, height);
    ctx.translate(width / 2, height / 2);
    ctx.scale(1 / k, 1 / k);
    ctx.rotate(-rect.angle);
    ctx.translate(-rect.cx, -rect.cy);
    ctx.drawImage(source, 0, 0);
    return ctx.getImageData(0, 0, width, height);
  };
}

export function createHostLocator(host, documentRef) {
  const canvas = documentRef.createElement("canvas");
  const ctx = canvas.getContext("2d");
  return async (frame, width, height) => {
    const scale = Math.min(1, HOST_SIDE / Math.max(width, height));
    canvas.width = Math.round(width * scale);
    canvas.height = Math.round(height * scale);
    ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
    const regions = await host.landmarks(canvas.toDataURL("image/jpeg", 0.8), canvas.width, canvas.height);
    const points = landmarksFromRegions(regions);
    return points ? points.map(([x, y]) => [x / scale, y / scale]) : null;
  };
}

export function createFaceStage({
  cameraStream,
  faceId = "",
  documentRef = globalThis.document,
  MediaStreamImpl = globalThis.MediaStream,
  ImageImpl = globalThis.Image,
  landmarkHost = globalThis.FmCallFaceHost,
  setIntervalImpl = globalThis.setInterval.bind(globalThis),
  clearIntervalImpl = globalThis.clearInterval.bind(globalThis),
  now = () => globalThis.performance.now(),
} = {}) {
  const cameraTrack = cameraStream.getVideoTracks()[0];
  const settings = cameraTrack?.getSettings?.() || {};
  let [width, height] = outputSize(settings.width, settings.height);
  const canvas = documentRef.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const renderer = createFaceRenderer(canvas);
  const track = canvas.captureStream(FRAME_RATE).getVideoTracks()[0];
  const frames = Array.from({ length: HOST_IN_FLIGHT + 1 }, () => {
    const frame = documentRef.createElement("canvas");
    return { canvas: frame, ctx: frame.getContext("2d"), busy: false };
  });

  const video = documentRef.createElement("video");
  video.muted = true;
  video.playsInline = true;
  video.srcObject = new MediaStreamImpl(cameraTrack ? [cameraTrack] : []);
  video.play?.()?.catch?.(() => {});

  const tracker = createFaceTracker({ grab: canvasGrabber(documentRef) });
  const anchorSmoother = createAnchorSmoother();
  const pointSmoother = createPointSmoother(68, 1.5, 1.5);
  let hostLocate = landmarkHost?.landmarks ? createHostLocator(landmarkHost, documentRef) : null;
  let hostFailures = 0;
  let face = null;
  let loading = 0;
  let live = null;
  let seenAt = -Infinity;
  let presence = 0;
  let hide = 1;
  let sequence = 0;
  let shown = 0;
  let inFlight = 0;
  let stopped = false;

  async function locate(frame, w, h, time) {
    if (hostLocate) {
      try {
        const points = await hostLocate(frame, w, h);
        hostFailures = 0;
        if (!points) return null;
        return pointSmoother.smooth(points, time, faceFrame(points).span);
      } catch (_) {
        hostFailures += 1;
        if (hostFailures >= HOST_FAILURES) hostLocate = null;
        return null;
      }
    }
    const anchors = tracker.track(frame, w, h);
    if (!anchors) return null;
    const smooth = anchorSmoother.smooth(anchors, time);
    return fitTemplate(faceTemplate(), smooth);
  }

  function render(frame, points, time) {
    if (points) {
      live = points;
      seenAt = time;
    }
    const held = live && time - seenAt < HOLD_MS;
    if (!held) {
      live = null;
      anchorSmoother.reset();
      pointSmoother.reset();
    }
    const ready = Boolean(face?.ready);
    const seen = Boolean(points) && ready;
    presence += ((held && ready ? 1 : 0) - presence) * (held ? FADE_IN : FADE_OUT);
    hide += ((seen ? 0 : 1) - hide) * (seen ? FADE_IN : FADE_OUT);
    renderer.draw(frame.canvas, width, height, live, { presence, hide: face ? hide : 0 });
  }

  function tick() {
    if (stopped || !video.videoWidth || (video.readyState ?? 4) < 2) return;
    const limit = hostLocate ? HOST_IN_FLIGHT : 1;
    if (inFlight >= limit) return;
    const frame = frames.find((entry) => !entry.busy);
    if (!frame) return;
    const [w, h] = outputSize(video.videoWidth, video.videoHeight);
    if (w !== width || h !== height) {
      width = w;
      height = h;
      tracker.reset();
    }
    if (frame.canvas.width !== w || frame.canvas.height !== h) {
      frame.canvas.width = w;
      frame.canvas.height = h;
    }
    frame.ctx.drawImage(video, 0, 0, w, h);
    frame.busy = true;
    inFlight += 1;
    const id = ++sequence;
    const time = now();
    const work = face ? locate(frame.canvas, w, h, time) : Promise.resolve(null);
    Promise.resolve(work).catch(() => null).then((points) => {
      inFlight -= 1;
      if (!stopped && id > shown) {
        shown = id;
        render(frame, points, time);
      }
      frame.busy = false;
    });
  }

  function loadFace(id) {
    const next = callFaceById(id);
    const token = ++loading;
    if (!next) {
      face = null;
      renderer.clearPhoto();
      return;
    }
    const points = facePoints(next);
    face = { id: next.id, ready: false };
    presence = 0;
    const image = new ImageImpl();
    image.onload = () => {
      if (token !== loading || stopped) return;
      renderer.setPhoto(image, faceMesh(), points, PHOTO_SIZE, next.look);
      face.ready = true;
    };
    image.src = callFacePhotoUrl(next);
  }

  loadFace(faceId);
  const timer = setIntervalImpl(tick, FRAME_MS);

  return {
    track,
    face: () => face?.id || "",
    setFace(id) {
      loadFace(id);
    },
    stop() {
      if (stopped) return;
      stopped = true;
      clearIntervalImpl(timer);
      try {
        track?.stop();
      } catch (_) {}
      video.srcObject = null;
      renderer.dispose();
    },
  };
}
