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
      154.1, 292.1, 153.5, 321.6, 154.1, 352.3, 156.4, 380.1, 162.8, 406.7, 173.2, 429.8, 191.7, 449.5, 221.3, 464.6, 256, 471.5, 293.1, 465.7,
      326.6, 451.3, 350.4, 431, 364.8, 406.7, 372.9, 380.1, 377, 352.3, 379.9, 321.6, 380.5, 292.1, 172.1, 257.9, 183.6, 247.5, 198.7, 244,
      215.5, 245.2, 230.5, 248.6, 283.2, 249.2, 298.3, 244, 316.8, 242.3, 337.1, 247.5, 350.4, 257.9, 257.7, 282.8, 258.3, 303.1, 259.5, 323.3,
      257.7, 341.8, 234, 352.8, 244.4, 358.6, 257.7, 360.4, 271.1, 358.6, 283.2, 352.8, 176.7, 284.5, 190.6, 278.2, 210.3, 278.2, 227.6, 288,
      212, 291.5, 191.7, 290.3, 284.4, 288, 300, 276.4, 318.5, 276.4, 337.1, 282.8, 320.3, 290.3, 300, 291.5, 200.4, 387.6, 218.9, 378.9,
      239.2, 376.6, 256, 378.3, 274.5, 376.6, 298.3, 378.9, 323.2, 385.8, 306.4, 407.8, 283.2, 422.9, 256, 426.4, 230.5, 422.9, 212, 407.8,
      205.6, 389.3, 228.8, 385.8, 256, 386.4, 284.4, 385.8, 318.5, 387.6, 284.4, 415.4, 256, 417.1, 228.8, 415.4,
    ]),
    head: Object.freeze([
      255.8, 67.7, 297.9, 61.9, 337, 78.1, 372.9, 99.7, 405.2, 127.5, 433.4, 161.1, 460.6, 199.9, 485.7, 246.7, 503.3, 303.2, 504, 366.2,
      485.9, 430.2, 451.2, 491.8, 387.9, 512, 322.5, 512, 255.8, 512, 191.1, 512, 133.5, 500.5, 88.3, 456.6, 58.8, 403.8, 47.3, 347.1,
      48.5, 294, 62, 246.7, 77.6, 206, 99.8, 171.6, 124.5, 142, 152.3, 116.9, 182.5, 94.5, 217.2, 77.9,
    ]),
  }),
  Object.freeze({
    id: "theo",
    label: "Theo",
    points: Object.freeze([
      131.5, 292.1, 131.5, 321.6, 133.8, 352.3, 137.3, 380.1, 143.1, 406.7, 153.5, 429.8, 173.2, 447.8, 209.1, 458.8, 255.4, 462.3, 301.7, 458.8,
      331.8, 447.8, 343.4, 429.8, 355.6, 406.7, 363.1, 380.1, 365.4, 352.3, 367.2, 321.6, 367.7, 292.1, 158.2, 263.7, 178.4, 255, 198.7, 251.5,
      218.9, 251.5, 237.5, 256.2, 281.5, 255, 300, 251.5, 320.3, 251.5, 339.9, 255, 353.8, 263.7, 257.7, 282.8, 258.3, 304.2, 259.5, 325.1,
      260.1, 345.3, 232.3, 354.6, 245, 358.6, 259.5, 360.4, 274.5, 358.6, 287.8, 354.6, 177.8, 283.9, 191.7, 277, 211.4, 278.2, 229.9, 287.4,
      212, 289.7, 191.7, 289.2, 285.5, 287.4, 300, 278.2, 319.1, 278.2, 337.1, 283.9, 320.3, 289.2, 300, 289.7, 205.6, 390.5, 221.3, 387,
      240.9, 383.5, 256, 384.7, 271.1, 383.5, 290.7, 385.8, 308.1, 389.9, 294.2, 397.4, 280.9, 404.4, 256, 407.8, 231.1, 404.4, 218.9, 397.4,
      209.1, 391.1, 229.9, 393.4, 256, 395.1, 282.6, 393.4, 304.6, 390.5, 282.6, 393.9, 256, 395.7, 229.9, 393.9,
    ]),
    head: Object.freeze([
      256.4, 43, 300.6, 52.9, 339.7, 73.6, 370.5, 103.5, 393.1, 137.6, 406.9, 174.1, 416.7, 210, 422.2, 246.6, 424.9, 285, 424, 327.3,
      422.6, 379.1, 404.2, 431.9, 368.8, 479.9, 321, 512, 256.4, 512, 190, 512, 124.7, 512, 87, 458.9, 59.8, 403.3, 49.9, 346,
      54.4, 292.7, 56.4, 246.6, 63.3, 202.5, 77.6, 160.5, 99.1, 121.1, 129.3, 87.2, 167.3, 61.7, 210.5, 45.3,
    ]),
  }),
  Object.freeze({
    id: "kofi",
    label: "Kofi",
    points: Object.freeze([
      122.8, 292.1, 122.8, 321.6, 124.6, 352.3, 128.6, 380.1, 136.2, 406.7, 148.3, 429.8, 166.8, 447.8, 205.6, 460.5, 256, 464.6, 304.6, 460.5,
      339.9, 447.8, 352.7, 429.8, 361.4, 406.7, 366.6, 380.1, 368.9, 352.3, 369.5, 321.6, 370.6, 292.1, 163.4, 257.9, 180.2, 250.4, 198.7, 245.7,
      217.2, 246.9, 235.7, 250.9, 281.5, 250.9, 300, 245.7, 320.3, 245.7, 339.9, 250.9, 357.3, 259.6, 257.7, 282.8, 258.3, 303.1, 259.5, 323.3,
      258.3, 343.6, 235.7, 355.2, 246.2, 359.8, 259.5, 361.5, 274.5, 358.6, 286.1, 353.4, 176.7, 282.2, 191.7, 275.3, 210.3, 276.4, 230.5, 287.4,
      212, 288.6, 191.7, 288.6, 284.4, 287.4, 300, 276.4, 318.5, 275.3, 337.1, 282.2, 320.3, 288.6, 300, 289.7, 203.9, 390.5, 225.9, 384.1,
      240.9, 381.2, 256, 382.9, 272.8, 381.2, 291.3, 384.1, 309.8, 389.3, 296.5, 399.2, 281.5, 407.8, 256, 410.7, 232.3, 407.8, 215.5, 400.9,
      206.8, 391.6, 229.9, 393.9, 256, 395.7, 282.6, 393.9, 306.4, 389.9, 282.6, 395.1, 256, 396.8, 229.9, 395.1,
    ]),
    head: Object.freeze([
      256.1, 48.2, 298.6, 59, 336.5, 78.4, 367.1, 106.2, 388.6, 139.8, 403.7, 174.3, 411.9, 209.9, 416.3, 245.4, 419.1, 282.6, 430, 329.2,
      428.7, 383.1, 410.4, 438.9, 378.6, 499.9, 321.5, 512, 256.1, 512, 188.3, 512, 120.1, 512, 82, 463.7, 53.4, 407, 43.5, 347.8,
      48.4, 292.8, 48.9, 245.4, 56, 199.8, 72.1, 156.8, 93.1, 115.5, 126.2, 82.5, 167.1, 60.5, 211, 47.7,
    ]),
  }),
  Object.freeze({
    id: "ingrid",
    label: "Ingrid",
    points: Object.freeze([
      150.6, 292.1, 151.8, 321.6, 154.1, 352.3, 159.3, 380.1, 168.6, 404.9, 181.9, 425.2, 200.4, 441.4, 225.9, 453, 256.6, 458.2, 289, 453,
      313.9, 441.4, 333.6, 425.2, 348.1, 404.9, 358.5, 380.1, 364.8, 352.3, 367.2, 321.6, 367.7, 292.1, 165.1, 263.7, 180.2, 257.9, 196.9, 255,
      215.5, 255, 232.3, 257.9, 283.2, 257.9, 300, 255, 318.5, 255, 337.1, 257.9, 350.4, 263.7, 257.7, 282.8, 258.3, 303.1, 259.5, 323.3,
      259.5, 344.2, 234, 355.2, 246.2, 359.8, 259.5, 361.5, 274.5, 359.8, 286.1, 355.2, 174.9, 283.9, 190.6, 276.4, 210.3, 277, 228.8, 286.3,
      210.3, 291.5, 191.7, 291.5, 284.4, 286.3, 301.7, 276.4, 320.3, 276.4, 337.1, 282.8, 320.3, 291.5, 301.7, 292.1, 205.6, 390.5, 222.4, 385.8,
      240.9, 381.2, 256, 382.9, 272.8, 381.2, 293.1, 384.7, 308.1, 388.2, 298.3, 399.7, 281.5, 410.7, 256, 415.4, 232.3, 410.7, 217.2, 401.5,
      209.1, 390.5, 228.8, 393.4, 256, 395.1, 283.2, 393.4, 304.6, 389.3, 283.2, 393.9, 256, 395.7, 228.8, 393.9,
    ]),
    head: Object.freeze([
      256, 48.1, 300.5, 51.1, 342, 67.4, 378.1, 92.8, 406.4, 126, 427.9, 163.1, 443.3, 203.1, 452.2, 245.9, 461.1, 292.7, 463.6, 345.9,
      451.6, 401.9, 422.9, 455.2, 378.7, 500.7, 320.7, 512, 256, 512, 191.3, 512, 142.1, 482.5, 103.4, 437.3, 80.7, 385.7, 76.7, 332.2,
      76.5, 286.9, 78.8, 245.9, 89.1, 207.8, 103.4, 172.4, 119.4, 136.9, 142.9, 104.1, 174.4, 76.5, 212.9, 57.2,
    ]),
  }),
  Object.freeze({
    id: "ren",
    label: "Ren",
    points: Object.freeze([
      150.6, 292.1, 148.3, 321.6, 148.3, 352.3, 150.6, 380.1, 156.4, 406.7, 166.8, 429.8, 185.4, 451.3, 213.2, 469.8, 250.8, 486, 289, 475,
      313.9, 458.2, 330.7, 436.8, 341.7, 413.6, 349.2, 386.4, 352.7, 358.6, 354.4, 329.1, 355.6, 296.7, 166.8, 259.6, 178.4, 251.5, 196.9, 247.5,
      217.2, 247.5, 234, 252.7, 283.2, 250.9, 298.3, 245.7, 316.8, 245.2, 333.6, 247.5, 341.7, 254.4, 259.5, 282.8, 261.2, 303.1, 262.9, 323.3,
      265.8, 345.3, 240.9, 355.2, 250.8, 359.8, 264.7, 362.1, 278, 359.8, 289.6, 353.4, 173.2, 286.3, 188.8, 278.2, 209.1, 278.2, 228.8, 288,
      209.1, 292.1, 190.6, 292.1, 284.4, 286.3, 298.3, 276.4, 316.8, 275.3, 333.6, 281.1, 318.5, 289.7, 300, 291.5, 198.7, 400.9, 215.5, 390.5,
      235.7, 387.6, 256, 388.2, 274.5, 386.4, 293.1, 389.3, 302.9, 392.2, 296.5, 419.4, 279.7, 436.2, 256, 441.4, 232.3, 436.2, 212, 422.9,
      203.9, 401.5, 225.9, 398.6, 256, 399.2, 283.2, 397.4, 300, 393.9, 281.5, 422.9, 256, 426.4, 228.8, 422.9,
    ]),
    head: Object.freeze([
      254.3, 33.2, 298.7, 52, 334.5, 79.9, 359.6, 114.4, 375.4, 149.9, 387.2, 182.5, 397.2, 213.9, 406.2, 246.5, 418.8, 284.1, 432.1, 332.2,
      431.7, 388, 412.2, 444.6, 376.4, 500.2, 318.9, 512, 254.3, 512, 187.6, 512, 122.2, 512, 58.8, 491.7, 23.2, 430.8, 2.7, 367.7,
      0, 304.6, 9.3, 246.5, 25.4, 194.3, 46.2, 146.3, 71.7, 100.9, 109.3, 64.7, 154.8, 40.1, 204.4, 28,
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
