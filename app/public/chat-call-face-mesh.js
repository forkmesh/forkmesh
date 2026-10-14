import { landmarksFromRegions } from "./chat-call-face-geometry.js";
import { createNetwork, floatTargets } from "./chat-call-segment.js";

const DETECT_SIZE = 128;
const MESH_SIZE = 192;
const DETECT_SCORE = 0.5;
const FACE_SCORE = 0.5;
const ROI_SCALE = 1.5;

export const MESH_REGIONS = Object.freeze({
  faceContour: [127, 234, 93, 132, 58, 172, 136, 150, 149, 176, 148, 152, 377, 400, 378, 379, 365, 397, 288, 361, 323, 454, 356],
  rightEyebrow: [70, 63, 105, 66, 107],
  leftEyebrow: [336, 296, 334, 293, 300],
  rightEye: [33, 246, 161, 160, 159, 158, 157, 173, 133, 155, 154, 153, 145, 144, 163, 7],
  leftEye: [263, 466, 388, 387, 386, 385, 384, 398, 362, 382, 381, 380, 374, 373, 390, 249],
  noseCrest: [168, 6, 197, 195, 5, 4],
  nose: [6, 197, 195, 5, 64, 98, 97, 2, 326, 327, 294],
  outerLips: [61, 185, 40, 39, 37, 0, 267, 269, 270, 409, 291, 375, 321, 405, 314, 17, 84, 181, 91, 146],
  innerLips: [78, 191, 80, 81, 82, 13, 312, 311, 310, 415, 308, 324, 318, 402, 317, 14, 87, 178, 88, 95],
});

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
uniform float uTexel;
varying vec2 vUv;
void main() {
  vec2 p = uCenter + (vUv.x - 0.5) * uAxisX + (vUv.y - 0.5) * uAxisY;
  vec2 uv = p / uFrame;
  vec3 c = vec3(0.0);
  if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) {
    vec2 sx = uAxisX / uFrame * uTexel / 3.0;
    vec2 sy = uAxisY / uFrame * uTexel / 3.0;
    for (int i = -1; i <= 1; i++) {
      for (int j = -1; j <= 1; j++) c += texture2D(uCam, uv + float(i) * sx + float(j) * sy).rgb;
    }
    c /= 9.0;
  }
  gl_FragColor = vec4(c * uRange.x + uRange.y, 0.0);
}`;

export function blazeAnchors() {
  const anchors = [];
  for (const [grid, count] of [[16, 2], [8, 6]]) {
    for (let y = 0; y < grid; y += 1) {
      for (let x = 0; x < grid; x += 1) {
        for (let k = 0; k < count; k += 1) anchors.push([(x + 0.5) / grid, (y + 0.5) / grid]);
      }
    }
  }
  return anchors;
}

export function decodeDetections(scores, boxes, anchors) {
  let best = null;
  const found = [];
  for (let i = 0; i < anchors.length; i += 1) {
    const logit = Math.max(-100, Math.min(100, scores[i]));
    const score = 1 / (1 + Math.exp(-logit));
    if (score < DETECT_SCORE) continue;
    const r = boxes.subarray(i * 16, i * 16 + 16);
    const [ax, ay] = anchors[i];
    const box = {
      score,
      cx: r[0] / DETECT_SIZE + ax,
      cy: r[1] / DETECT_SIZE + ay,
      w: r[2] / DETECT_SIZE,
      h: r[3] / DETECT_SIZE,
      keypoints: Array.from({ length: 6 }, (_, k) => [r[4 + k * 2] / DETECT_SIZE + ax, r[5 + k * 2] / DETECT_SIZE + ay]),
    };
    found.push(box);
    if (!best || score > best.score) best = box;
  }
  if (!best) return null;
  const overlap = (a, b) => {
    const x0 = Math.max(a.cx - a.w / 2, b.cx - b.w / 2);
    const x1 = Math.min(a.cx + a.w / 2, b.cx + b.w / 2);
    const y0 = Math.max(a.cy - a.h / 2, b.cy - b.h / 2);
    const y1 = Math.min(a.cy + a.h / 2, b.cy + b.h / 2);
    const inter = Math.max(0, x1 - x0) * Math.max(0, y1 - y0);
    return inter / (a.w * a.h + b.w * b.h - inter || 1);
  };
  const group = found.filter((box) => overlap(box, best) > 0.3);
  const total = group.reduce((sum, box) => sum + box.score, 0);
  const mean = (pick) => group.reduce((sum, box) => sum + pick(box) * box.score, 0) / total;
  return {
    score: best.score,
    cx: mean((b) => b.cx),
    cy: mean((b) => b.cy),
    w: mean((b) => b.w),
    h: mean((b) => b.h),
    keypoints: best.keypoints.map((_, k) => [mean((b) => b.keypoints[k][0]), mean((b) => b.keypoints[k][1])]),
  };
}

export function roiFromDetection(detection, letterbox) {
  const toFrame = ([x, y]) => [(x - letterbox.x) * letterbox.scale, (y - letterbox.y) * letterbox.scale];
  const center = toFrame([detection.cx, detection.cy]);
  const right = toFrame(detection.keypoints[0]);
  const left = toFrame(detection.keypoints[1]);
  const angle = Math.atan2(left[1] - right[1], left[0] - right[0]);
  const size = Math.max(detection.w, detection.h) * letterbox.scale * ROI_SCALE;
  return { cx: center[0], cy: center[1], size, angle };
}

export function roiFromMesh(points) {
  const a = points[33];
  const b = points[263];
  const angle = Math.atan2(b[1] - a[1], b[0] - a[0]);
  const c = Math.cos(-angle);
  const s = Math.sin(-angle);
  let minX = Infinity;
  let minY = Infinity;
  let maxX = -Infinity;
  let maxY = -Infinity;
  const ox = points[1][0];
  const oy = points[1][1];
  for (const [x, y] of points) {
    const rx = (x - ox) * c - (y - oy) * s;
    const ry = (x - ox) * s + (y - oy) * c;
    minX = Math.min(minX, rx);
    maxX = Math.max(maxX, rx);
    minY = Math.min(minY, ry);
    maxY = Math.max(maxY, ry);
  }
  const mx = (minX + maxX) / 2;
  const my = (minY + maxY) / 2;
  const ci = Math.cos(angle);
  const si = Math.sin(angle);
  return {
    cx: ox + mx * ci - my * si,
    cy: oy + mx * si + my * ci,
    size: Math.max(maxX - minX, maxY - minY) * ROI_SCALE,
    angle,
  };
}

export function meshRegions(points) {
  const out = {};
  for (const [name, indices] of Object.entries(MESH_REGIONS)) out[name] = indices.map((i) => [points[i][0], points[i][1]]);
  return out;
}

export function meshToLandmarks(points) {
  return landmarksFromRegions(meshRegions(points));
}

export function createFaceMesh(gl, models, format = floatTargets(gl)) {
  if (!format || !models.detector || !models.mesh) return null;
  const detector = createNetwork(gl, models.detector, format);
  const mesh = createNetwork(gl, models.mesh, format);
  const anchors = blazeAnchors();
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
  const uniform = (name) => gl.getUniformLocation(program, name);
  const u = Object.fromEntries(["uCam", "uCenter", "uAxisX", "uAxisY", "uFrame", "uRange", "uTexel"].map((name) => [name, uniform(name)]));
  let roi = null;
  let lastScore = 0;

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
    gl.uniform2f(u.uAxisX, c * region.sizeX, s * region.sizeX);
    gl.uniform2f(u.uAxisY, -s * region.sizeY, c * region.sizeY);
    gl.uniform2f(u.uFrame, width, height);
    gl.uniform2f(u.uRange, range[0], range[1]);
    gl.uniform1f(u.uTexel, 1 / size);
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.disable(gl.BLEND);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.disableVertexAttribArray(0);
  }

  function readChannels(net, group) {
    const parts = [];
    for (const index of group) {
      const r = net.read(index);
      const [h, w, channels] = r.shape;
      const values = new Float32Array(h * w * channels);
      let at = 0;
      for (let y = 0; y < h; y += 1) {
        for (let x = 0; x < w; x += 1) {
          for (let c = 0; c < channels; c += 1) values[at++] = r.pixels[((y * r.width) + (c >> 2) * w + x) * 4 + (c & 3)];
        }
      }
      parts.push(values);
    }
    const total = parts.reduce((sum, part) => sum + part.length, 0);
    const out = new Float32Array(total);
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
    crop(camera, detector, DETECT_SIZE, { cx: width / 2, cy: height / 2, sizeX: side, sizeY: side, angle: 0 }, width, height, [2, -1]);
    detector.run();
    const [boxGroup, scoreGroup] = models.detector.outputs;
    const boxes = readChannels(detector, boxGroup);
    const scores = readChannels(detector, scoreGroup);
    const detection = decodeDetections(scores, boxes, anchors);
    return detection ? roiFromDetection(detection, letterbox) : null;
  }

  return {
    get score() {
      return lastScore;
    },
    reset() {
      roi = null;
    },
    track(camera, width, height) {
      if (!roi) roi = detect(camera, width, height);
      if (!roi) {
        lastScore = 0;
        return null;
      }
      crop(camera, mesh, MESH_SIZE, { ...roi, sizeX: roi.size, sizeY: roi.size }, width, height, [1, 0]);
      mesh.run();
      const [pointGroup, flagGroup] = models.mesh.outputs;
      const raw = readChannels(mesh, pointGroup);
      const flag = readChannels(mesh, flagGroup)[0];
      lastScore = 1 / (1 + Math.exp(-Math.max(-60, Math.min(60, flag))));
      if (lastScore < FACE_SCORE) {
        roi = null;
        return null;
      }
      const c = Math.cos(roi.angle);
      const s = Math.sin(roi.angle);
      const points = [];
      for (let i = 0; i < raw.length; i += 3) {
        const x = (raw[i] / MESH_SIZE - 0.5) * roi.size;
        const y = (raw[i + 1] / MESH_SIZE - 0.5) * roi.size;
        points.push([roi.cx + x * c - y * s, roi.cy + x * s + y * c, raw[i + 2]]);
      }
      roi = roiFromMesh(points);
      return { mesh: points, points: meshToLandmarks(points), score: lastScore };
    },
    dispose() {
      detector.dispose();
      mesh.dispose();
      gl.deleteProgram(program);
      gl.deleteBuffer(quad);
    },
  };
}
