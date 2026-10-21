#!/usr/bin/env node
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { createRequire } from "node:module";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const publicDir = path.join(root, "app", "public");
const clipDir = path.join(root, "tools", "testdata", "face-stream");
const require = createRequire(import.meta.url);
const TYPES = { ".js": "text/javascript", ".mjs": "text/javascript", ".jpg": "image/jpeg", ".png": "image/png", ".webm": "video/webm", ".fmnn": "application/octet-stream" };

function loadPlaywright() {
  const places = [
    process.env.FM_PLAYWRIGHT_CORE,
    path.join(root, "app", "browser_tests", "node_modules", "playwright-core"),
    path.join(os.homedir(), "projects", "forkmesh", "src", "app", "browser_tests", "node_modules", "playwright-core"),
  ].filter(Boolean);
  for (const place of places) {
    try {
      return require(place);
    } catch (_) {}
  }
  throw new Error("playwright-core not found: set FM_PLAYWRIGHT_CORE to its folder");
}

function args(argv) {
  const out = { dir: "", frames: 0, every: 0, precision: 96, loss: 0.1, face: "maya", clip: "", baseline: "", noise: 0.6, stills: [] };
  for (let i = 0; i < argv.length; i += 1) {
    if (!argv[i].startsWith("--")) {
      out.dir = argv[i];
      continue;
    }
    const key = argv[i].slice(2);
    const value = argv[i + 1];
    i += 1;
    if (key === "face" || key === "clip" || key === "baseline") out[key] = value;
    else if (key === "stills") out.stills = value.split(",").map(Number);
    else out[key] = Number(value);
  }
  return out;
}

async function clipInfo(name) {
  const clips = JSON.parse(await readFile(path.join(clipDir, "clips.json"), "utf8"));
  const clip = clips[name];
  if (!clip) throw new Error(`unknown clip ${name}: ${Object.keys(clips).join(", ")}`);
  const bytes = await readFile(path.join(clipDir, clip.file));
  const sha = createHash("sha256").update(bytes).digest("hex");
  if (sha !== clip.sha256) throw new Error(`${clip.file} does not match its pinned sha256`);
  return { ...clip, name, bytes };
}

function baselineFile(rev, rel) {
  return execFileSync("git", ["-C", root, "show", `${rev}:app/public/${rel}`], { maxBuffer: 64 * 1024 * 1024 });
}

async function main() {
  const options = args(process.argv.slice(2));
  if (!options.dir) throw new Error("usage: face_stream_eval.mjs <output dir> [--face maya | --clip keshav] [--baseline <git rev>] [--frames N] [--every N] [--precision 96] [--loss 0.1]");
  const clip = options.clip ? await clipInfo(options.clip) : null;
  if (clip) {
    options.frames = Math.min(options.frames || clip.frames, clip.frames);
    options.every = options.every || 1;
    options.fps = clip.fps;
    options.clipMeta = { name: clip.name, width: clip.width, height: clip.height, licence: clip.licence, author: clip.author, page: clip.page, sha256: clip.sha256 };
  } else {
    options.frames = options.frames || 180;
    options.every = options.every || 6;
    options.fps = 30;
  }
  if (!options.stills.length) {
    const step = clip ? 40 : 36;
    for (let f = 0; f < options.frames; f += step) options.stills.push(f);
    options.stills.push(options.frames - 1 - ((options.frames - 1) % options.every));
  }
  await mkdir(options.dir, { recursive: true });
  const { chromium } = loadPlaywright();
  const browser = await chromium.launch({ args: ["--enable-unsafe-swiftshader", "--use-angle=swiftshader", "--ignore-gpu-blocklist", "--autoplay-policy=no-user-gesture-required"] });
  try {
    const page = await browser.newPage();
    page.on("pageerror", (error) => console.error("page error:", error.message));
    await page.route("http://fm.test/**", async (route) => {
      const url = new URL(route.request().url());
      const name = decodeURIComponent(url.pathname);
      if (name === "/") {
        await route.fulfill({ status: 200, contentType: "text/html", body: "<!doctype html><html><body></body></html>" });
        return;
      }
      if (name === "/clip.webm" && clip) {
        await route.fulfill({ status: 200, contentType: "video/webm", body: clip.bytes });
        return;
      }
      if (name.startsWith("/v1/") && options.baseline) {
        const rel = path.posix.normalize(name.slice(4));
        try {
          await route.fulfill({ status: 200, contentType: TYPES[path.extname(rel)] || "application/octet-stream", body: baselineFile(options.baseline, rel) });
        } catch (_) {
          await route.fulfill({ status: 404, body: "" });
        }
        return;
      }
      const file = path.join(publicDir, name);
      if (!file.startsWith(publicDir)) {
        await route.fulfill({ status: 403, body: "" });
        return;
      }
      try {
        await route.fulfill({ status: 200, contentType: TYPES[path.extname(file)] || "application/octet-stream", body: await readFile(file) });
      } catch (_) {
        await route.fulfill({ status: 404, body: "" });
      }
    });
    await page.goto("http://fm.test/");
    const { clipMeta, ...pageOptions } = options;
    const result = await page.evaluate(evaluate, { ...pageOptions, clip: Boolean(clip) });
    if (clipMeta) result.clip = clipMeta;
    for (const still of result.stills) {
      const label = String(still.frame).padStart(3, "0");
      still.file = `face-stream-${label}.png`;
      await writeFile(path.join(options.dir, still.file), Buffer.from(still.png.split(",")[1], "base64"));
      delete still.png;
      if (still.compare) {
        still.compareFile = `compare-${label}.png`;
        await writeFile(path.join(options.dir, still.compareFile), Buffer.from(still.compare.split(",")[1], "base64"));
        delete still.compare;
      }
    }
    if (result.reference) {
      await writeFile(path.join(options.dir, "face-stream-reference.jpg"), Buffer.from(result.reference, "base64"));
      result.reference = "face-stream-reference.jpg";
    }
    if (result.atlas) {
      await writeFile(path.join(options.dir, "face-stream-atlas.jpg"), Buffer.from(result.atlas, "base64"));
      result.atlas = "face-stream-atlas.jpg";
    }
    await page.locator(".fm-face-stream").first().screenshot({ path: path.join(options.dir, "face-stream-tile.png") });
    result.tile.file = "face-stream-tile.png";
    const report = path.join(options.dir, "face-stream-eval.json");
    await writeFile(report, JSON.stringify(result, null, 2) + "\n");
    console.log(JSON.stringify(result.baseline ? { ...result.summary, baseline: result.baseline } : result.summary));
  } finally {
    await browser.close();
  }
}

async function evaluate(options) {
  const fs = await import("/chat-call-facestream.js");
  const old = options.baseline ? await import("/v1/chat-call-facestream.js") : null;
  const faces = await import("/chat-call-faces.js");
  const geo = await import("/chat-call-face-geometry.js");
  const render = await import("/chat-call-face-render.js");
  const seg = await import("/chat-call-segment.js");
  const probeModule = await import("/chat-call-face-expression.js");
  const FPS = options.fps;
  const TILE = [320, 400];
  const image = (src) => new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = src;
  });
  let seed = 0x2390;
  const random = () => {
    seed ^= seed << 13;
    seed ^= seed >>> 17;
    seed ^= seed << 5;
    return (seed >>> 0) / 4294967296;
  };
  const gauss = () => Math.sqrt(-2 * Math.log(random() + 1e-12)) * Math.cos(2 * Math.PI * random());
  const quat = (yaw, pitch, roll) => {
    const mul = (a, b) => [
      a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
      a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
      a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
      a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0],
    ];
    return mul(mul([Math.cos(roll / 2), 0, 0, Math.sin(roll / 2)], [Math.cos(yaw / 2), 0, Math.sin(yaw / 2), 0]), [Math.cos(pitch / 2), Math.sin(pitch / 2), 0, 0]);
  };
  const models = await seg.loadSegmentModels();
  const canvasOf = (w, h) => {
    const c = document.createElement("canvas");
    c.width = w;
    c.height = h;
    return c;
  };

  let W = 1280;
  let H = 720;
  let getFrame;
  let trackSource = null;
  let personMask;
  let synthetic = null;
  if (options.clip) {
    const video = document.createElement("video");
    video.src = URL.createObjectURL(await (await fetch("/clip.webm")).blob());
    video.muted = true;
    video.preload = "auto";
    await new Promise((resolve, reject) => {
      video.onloadeddata = resolve;
      video.onerror = () => reject(new Error(`video error ${video.error?.code}`));
    });
    W = video.videoWidth;
    H = video.videoHeight;
    const frameCanvas = canvasOf(W, H);
    const frameCtx = frameCanvas.getContext("2d", { willReadFrequently: true });
    getFrame = async (f) => {
      video.currentTime = (f + 0.5) / FPS;
      await new Promise((resolve) => {
        video.onseeked = resolve;
      });
      frameCtx.drawImage(video, 0, 0, W, H);
      return frameCanvas;
    };
    const matter = render.createFaceRenderer(canvasOf(W, H), { models });
    const tracker = matter;
    const smoother = geo.createPointSmoother(68, 1.5, 1.5);
    const probe = probeModule.createExpressionProbe();
    trackSource = (canvas, time) => {
      const found = tracker.trackFace(canvas, W, H);
      if (!found) return null;
      const points = smoother.smooth(found.points, time, geo.faceFrame(found.points).span);
      const reading = probe.measure(canvas, points, time, { lids: true });
      return { points: points.map(([x, y]) => [x, y]), reading };
    };
    const gl = matter.gl;
    const readProgram = render.FACE_WARP.compile(gl, `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}`, `
precision highp float;
uniform sampler2D uCam;
uniform sampler2D uCoef;
varying vec2 vUv;
${seg.MATTE_SAMPLE}
void main() {
  float a = callMatte(uCoef, texture2D(uCam, vUv).rgb, vUv);
  gl_FragColor = vec4(a, a, a, 1.0);
}`);
    const camTexture = render.FACE_WARP.texture(gl);
    const out = render.FACE_WARP.target(gl, W, H, gl.UNSIGNED_BYTE);
    const quad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    personMask = (canvas, points) => {
      matter.matte?.reset();
      for (let k = 0; k < 2; k += 1) matter.draw(canvas, W, H, null, { needMatte: true, body: { points, frame: null } });
      if (!matter.matte?.ready) return null;
      gl.bindTexture(gl.TEXTURE_2D, camTexture);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, canvas);
      gl.bindFramebuffer(gl.FRAMEBUFFER, out.framebuffer);
      gl.viewport(0, 0, W, H);
      gl.useProgram(readProgram.program);
      [["uCam", camTexture], ["uCoef", matter.matte.coefficients], ["uMatteMask", matter.matte.mask]].forEach(([name, handle], unit) => {
        gl.activeTexture(gl.TEXTURE0 + unit);
        gl.bindTexture(gl.TEXTURE_2D, handle);
        gl.uniform1i(readProgram.uniforms[name], unit);
      });
      gl.bindBuffer(gl.ARRAY_BUFFER, quad);
      const at = gl.getAttribLocation(readProgram.program, "aPos");
      gl.enableVertexAttribArray(at);
      gl.vertexAttribPointer(at, 2, gl.FLOAT, false, 0, 0);
      gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      gl.disableVertexAttribArray(at);
      const rgba = new Uint8Array(W * H * 4);
      gl.readPixels(0, 0, W, H, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      const bytes = new Uint8Array(W * H);
      for (let i = 0; i < W * H; i += 1) bytes[i] = rgba[i * 4];
      return { bytes, width: W, height: H };
    };
  } else {
    const person = faces.callFaceById(options.face);
    const head = faces.faceHead(person);
    const shape = geo.normalizeShape(faces.facePoints(person)).map(([x, y], i) => [x, y, fs.FACE_STREAM_TEMPLATE[i][2]]);
    const truthAt = (f) => {
      const t = f / FPS;
      const yaw = 0.3 * Math.sin((2 * Math.PI * t) / 4.3);
      const pitch = 0.12 * Math.sin((2 * Math.PI * t) / 3.1 + 0.7);
      const roll = 0.07 * Math.sin((2 * Math.PI * t) / 5.3 + 1.3);
      const scale = 96 * (1 + 0.03 * Math.sin((2 * Math.PI * t) / 6.1));
      const cx = 640 + 40 * Math.sin((2 * Math.PI * t) / 7);
      const cy = 330 + 15 * Math.sin((2 * Math.PI * t) / 4.9 + 0.4);
      const open = Math.max(0, Math.sin(2 * Math.PI * 2.7 * t)) * (0.5 + 0.5 * Math.sin(2 * Math.PI * 0.4 * t));
      const smile = 0.5 + 0.5 * Math.sin((2 * Math.PI * t) / 8);
      const blink = Math.max(0, 1 - Math.abs((t % 3.2) - 1.5) / 0.08);
      const tongue = t % 5 > 4.2 ? 0.8 : 0;
      const gaze = 0.2 * Math.sin((2 * Math.PI * t) / 2.3);
      const rows = fs.rotationRows(quat(yaw, pitch, roll));
      const points = shape.map((p, i) => {
        const q = p.slice();
        if (i >= 5 && i <= 11) q[1] += open * 0.22 * (1 - Math.abs(i - 8) / 4);
        else if ((i >= 55 && i <= 59) || (i >= 65 && i <= 67)) q[1] += open * 0.32;
        else if (i >= 61 && i <= 63) q[1] -= open * 0.03;
        if (i === 48 || i === 60) {
          q[0] -= smile * 0.05;
          q[1] -= smile * 0.04;
        }
        if (i === 54 || i === 64) {
          q[0] += smile * 0.05;
          q[1] -= smile * 0.04;
        }
        const dot = (r) => r[0] * q[0] + r[1] * q[1] + r[2] * q[2];
        return [cx + scale * dot(rows[0]), cy + scale * dot(rows[1]), scale * dot(rows[2])];
      });
      return { points, expression: { lidLeft: blink, lidRight: blink, gazeX: gaze, gazeY: gaze * 0.3, mouth: open, smile, tongue } };
    };
    const camera = canvasOf(W, H);
    const truthRenderer = fs.createFaceStreamRenderer(camera);
    const [photo, matte, room] = await Promise.all([
      image(`/assets/call-faces/${person.id}.jpg`),
      image(`/assets/call-faces/${person.id}-matte.png`),
      image("/assets/call-backgrounds/office.jpg"),
    ]);
    const roomCanvas = canvasOf(W, H);
    const roomCtx = roomCanvas.getContext("2d", { willReadFrequently: true });
    roomCtx.drawImage(room, 0, 0, W, H);
    const roomPixels = roomCtx.getImageData(0, 0, W, H);
    truthRenderer.setBackdrop(roomPixels);
    truthRenderer.setFace({ photo, matte, mesh: head.mesh, layout: head.layout, model: head.model, size: 512 });
    const grab = canvasOf(W, H);
    const grabCtx = grab.getContext("2d", { willReadFrequently: true });
    const smoother = geo.createPointSmoother(68, 1.5, 1.5);
    synthetic = { truthAt };
    getFrame = async (f) => {
      const truth = truthAt(f);
      truthRenderer.draw(W, H, truth.points, truth.expression);
      grabCtx.drawImage(camera, 0, 0);
      return grab;
    };
    trackSource = (canvas, time, f) => {
      const truth = truthAt(f);
      const seen = truth.points.map(([x, y]) => [x + options.noise * gauss(), y + options.noise * gauss()]);
      const points = smoother.smooth(seen, time, geo.faceFrame(seen).span).map(([x, y]) => [x, y]);
      const reading = { close: [truth.expression.lidLeft, truth.expression.lidRight], look: [[truth.expression.gazeX, truth.expression.gazeY], [truth.expression.gazeX, truth.expression.gazeY]], tongue: truth.expression.tongue };
      return { points, reading };
    };
    personMask = (canvas) => {
      const frame = canvas.getContext("2d", { willReadFrequently: true }).getImageData(0, 0, W, H);
      const bytes = new Uint8Array(W * H);
      for (let i = 0; i < W * H; i += 1) {
        const d = Math.abs(frame.data[i * 4] - roomPixels.data[i * 4]) + Math.abs(frame.data[i * 4 + 1] - roomPixels.data[i * 4 + 1]) + Math.abs(frame.data[i * 4 + 2] - roomPixels.data[i * 4 + 2]);
        bytes[i] = d > 24 ? 255 : 0;
      }
      return { bytes, width: W, height: H };
    };
  }

  const versions = [{ name: "v2", fs, primary: true }];
  if (old) versions.push({ name: "v1", fs: old, primary: false });
  for (const v of versions) {
    v.sender = v.fs.createFaceStreamSender({ width: W, height: H, fps: FPS, precision: options.precision });
    v.stream = [];
    v.referenceBytes = 0;
    v.references = 0;
    v.atlasBytes = 0;
    v.atlases = 0;
    v.firstReference = null;
    v.lastAtlas = null;
  }
  const frames = [];
  let lastPoints = null;
  for (let f = 0; f < options.frames; f += 1) {
    const time = 1000 + Math.round((f * 1000) / FPS);
    const canvas = await getFrame(f);
    const tracked = trackSource(canvas, time, f) || (lastPoints ? { points: lastPoints, reading: null } : null);
    if (!tracked) {
      frames.push({ f, time, missing: true });
      continue;
    }
    lastPoints = tracked.points;
    const truthPoints = synthetic ? synthetic.truthAt(f).points.map(([x, y]) => [x, y]) : tracked.points;
    const pose = fs.faceStreamPose(tracked.points);
    const expression = fs.faceStreamExpression(tracked.reading, tracked.points);
    const entry = { f, time, points: tracked.points, truthPoints, reading: tracked.reading, expression };
    let mask = null;
    for (const v of versions) {
      const extras = [];
      const packet = v.sender.frame({ time, points: tracked.points, pose, expression });
      const sent = v.sender.encoder.last;
      const score = v.sender.score({ points: tracked.points, pose, expression });
      if (v.sender.due(time, score)) {
        if (!mask) mask = personMask(canvas, tracked.points);
        const reference = await v.sender.reference({ time, source: canvas, points: tracked.points, mask, score });
        v.referenceBytes += reference.length;
        v.references += 1;
        extras.push(reference);
        if (!v.firstReference) {
          const decoded = v.fs.decodeFaceStreamReference(reference);
          let text = "";
          for (let i = 0; i < decoded.image.length; i += 0x8000) text += String.fromCharCode(...decoded.image.subarray(i, i + 0x8000));
          v.firstReference = btoa(text);
        }
      }
      if (typeof v.sender.mouthTile === "function") {
        v.sender.mouthTile({ time, source: canvas, points: tracked.points, pose, expression });
        if (v.sender.atlasDue(time)) {
          const atlas = await v.sender.atlas(time);
          v.atlasBytes += atlas.length;
          v.atlases += 1;
          extras.push(atlas);
          v.lastAtlas = atlas;
        }
      }
      v.stream.push({ f, extras, packet, sent });
    }
    frames.push(entry);
  }

  const grabTile = (canvas) => {
    const c = canvasOf(TILE[0], TILE[1]);
    const ctx = c.getContext("2d", { willReadFrequently: true });
    ctx.drawImage(canvas, 0, 0);
    return { data: ctx.getImageData(0, 0, TILE[0], TILE[1]), canvas: c };
  };
  const truthTile = (source, ref) => {
    const c = canvasOf(TILE[0], TILE[1]);
    const ctx = c.getContext("2d", { willReadFrequently: true });
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(source, ref.cropX, ref.cropY, ref.cropW, ref.cropH, 0, 0, TILE[0], TILE[1]);
    return { data: ctx.getImageData(0, 0, TILE[0], TILE[1]), canvas: c };
  };
  const toTile = (points, ref) => points.map(([x, y]) => [(x - ref.cropX) * (TILE[0] / ref.cropW), (y - ref.cropY) * (TILE[1] / ref.cropH)]);
  const box = (points) => {
    const xs = points.map((p) => p[0]);
    const ys = points.map((p) => p[1]);
    const pad = 0.15 * (Math.max(...xs) - Math.min(...xs));
    return [Math.max(0, Math.floor(Math.min(...xs) - pad)), Math.max(0, Math.floor(Math.min(...ys) - 2.5 * pad)), Math.min(TILE[0], Math.ceil(Math.max(...xs) + pad)), Math.min(TILE[1], Math.ceil(Math.max(...ys) + pad))];
  };
  const mouthBox = (points) => {
    const m = points.slice(48, 68);
    const cx = m.reduce((s, p) => s + p[0], 0) / m.length;
    const cy = m.reduce((s, p) => s + p[1], 0) / m.length;
    const span = geo.faceFrame(points).span;
    const half = [0.75 * span, 0.5 * span];
    return [Math.max(0, Math.floor(cx - half[0])), Math.max(0, Math.floor(cy - half[1])), Math.min(TILE[0], Math.ceil(cx + half[0])), Math.min(TILE[1], Math.ceil(cy + half[1]))];
  };
  const psnr = (a, b, region = [0, 0, TILE[0], TILE[1]]) => {
    let sum = 0;
    let n = 0;
    for (let y = region[1]; y < region[3]; y += 1) {
      for (let x = region[0]; x < region[2]; x += 1) {
        const at = (y * TILE[0] + x) * 4;
        for (let c = 0; c < 3; c += 1) {
          const d = a.data[at + c] - b.data[at + c];
          sum += d * d;
          n += 1;
        }
      }
    }
    const mse = sum / Math.max(1, n);
    return mse > 0 ? 10 * Math.log10((255 * 255) / mse) : 99;
  };
  const ssim = (a, b, region = [0, 0, TILE[0], TILE[1]]) => {
    const luma = (img, at) => 0.299 * img.data[at] + 0.587 * img.data[at + 1] + 0.114 * img.data[at + 2];
    const c1 = (0.01 * 255) ** 2;
    const c2 = (0.03 * 255) ** 2;
    let total = 0;
    let count = 0;
    for (let y0 = region[1]; y0 + 8 <= region[3]; y0 += 4) {
      for (let x0 = region[0]; x0 + 8 <= region[2]; x0 += 4) {
        const va = [];
        const vb = [];
        for (let y = y0; y < y0 + 8; y += 1) {
          for (let x = x0; x < x0 + 8; x += 1) {
            const at = (y * TILE[0] + x) * 4;
            va.push(luma(a, at));
            vb.push(luma(b, at));
          }
        }
        const ma = va.reduce((s, v) => s + v, 0) / 64;
        const mb = vb.reduce((s, v) => s + v, 0) / 64;
        let sa = 0;
        let sb = 0;
        let sab = 0;
        for (let k = 0; k < 64; k += 1) {
          sa += (va[k] - ma) ** 2 / 63;
          sb += (vb[k] - mb) ** 2 / 63;
          sab += ((va[k] - ma) * (vb[k] - mb)) / 63;
        }
        total += ((2 * ma * mb + c1) * (2 * sab + c2)) / ((ma * ma + mb * mb + c1) * (sa + sb + c2));
        count += 1;
      }
    }
    return total / Math.max(1, count);
  };
  const correlation = (a, b) => {
    const n = Math.min(a.length, b.length);
    if (n < 3) return 0;
    const ma = a.slice(0, n).reduce((s, v) => s + v, 0) / n;
    const mb = b.slice(0, n).reduce((s, v) => s + v, 0) / n;
    let sab = 0;
    let sa = 0;
    let sb = 0;
    for (let i = 0; i < n; i += 1) {
      sab += (a[i] - ma) * (b[i] - mb);
      sa += (a[i] - ma) ** 2;
      sb += (b[i] - mb) ** 2;
    }
    return sa > 0 && sb > 0 ? sab / Math.sqrt(sa * sb) : 0;
  };
  const tileRenderer = render.createFaceRenderer(canvasOf(16, 16), { models });
  const tileTracker = () => (canvas) => {
    tileRenderer.faceMesh?.reset();
    const found = tileRenderer.trackFace(canvas, TILE[0], TILE[1]);
    return found ? geo.mouthGap(found.points) : null;
  };
  const frozenTile = async (reference, cache) => {
    if (cache.has(reference)) return cache.get(reference);
    const bitmap = await createImageBitmap(new Blob([reference.image], { type: "image/jpeg" }));
    const c = canvasOf(TILE[0], TILE[1]);
    const ctx = c.getContext("2d", { willReadFrequently: true });
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(bitmap, 0, 0, TILE[0], TILE[1]);
    const data = ctx.getImageData(0, 0, TILE[0], TILE[1]);
    cache.set(reference, data);
    return data;
  };

  for (const v of versions) {
    v.view = v.fs.createFaceStreamView({ width: TILE[0], height: TILE[1] });
    document.body.append(v.view.element);
    v.measure = tileTracker();
    v.scores = { full: [], face: [], ssim: [], faceSsim: [], mouthPsnr: [], frozenPsnr: [], frozenFacePsnr: [], frozenSsim: [], gaps: [], truthGaps: [] };
    v.bytes = 0;
    v.keyBytes = 0;
    v.keys = 0;
    v.deltaBytes = 0;
    v.deltas = 0;
    v.frozen = new Map();
  }
  const main = versions[0];
  main.lossView = fs.createFaceStreamView({ width: TILE[0], height: TILE[1] });
  main.exactView = fs.createFaceStreamView({ width: TILE[0], height: TILE[1] });
  const truthMeasure = tileTracker();
  const decoder = fs.createFaceStreamDecoder();
  let lossSeed = 0x51;
  const dropped = () => {
    lossSeed = (lossSeed * 1103515245 + 12345) % 2147483648;
    return lossSeed / 2147483648 < options.loss;
  };
  let codecSq = 0;
  let truthSq = 0;
  let worst = 0;
  let n = 0;
  let lost = 0;
  let stale = 0;
  const extra = { codecPsnr: [], codecSsim: [], lossPsnr: [], lossFacePsnr: [] };
  const stills = [];
  const truthGaps = [];
  for (const entry of frames) {
    if (entry.missing) continue;
    const at = frames.indexOf(entry);
    const evaluate = entry.f % options.every === 0 || options.stills.includes(entry.f);
    const drop = dropped() && entry.f > 0;
    for (const v of versions) {
      const s = v.stream[at];
      for (const packet of s.extras) {
        await v.view.receive(packet);
        if (v.primary) {
          await main.lossView.receive(packet);
          await main.exactView.receive(packet);
        }
      }
      await v.view.receive(s.packet);
      v.bytes += s.packet.length;
      if (s.sent.key) {
        v.keys += 1;
        v.keyBytes += s.packet.length;
      } else {
        v.deltas += 1;
        v.deltaBytes += s.packet.length;
      }
      if (v.primary) {
        const frame = decoder.decode(s.packet);
        frame.points.forEach((p, i) => {
          for (let axis = 0; axis < 2; axis += 1) {
            const e = p[axis] - entry.points[i][axis];
            codecSq += e * e;
            worst = Math.max(worst, Math.abs(e));
            const t = p[axis] - entry.truthPoints[i][axis];
            truthSq += t * t;
            n += 1;
          }
        });
        if (drop) {
          lost += 1;
        } else {
          const result = await main.lossView.receive(s.packet);
          if (result.type === "frame" && result.frame.stale) stale += 1;
        }
      }
    }
    if (!evaluate) continue;
    const source = await getFrame(entry.f);
    let truthShown = null;
    let truthGap = null;
    for (const v of versions) {
      const ref = v.view.receiver.reference;
      if (!ref || !v.view.ready) continue;
      v.view.draw();
      const got = grabTile(v.view.canvas);
      const truth = truthTile(source, ref);
      const tilePoints = toTile(entry.truthPoints, ref);
      const region = box(tilePoints);
      const mouthRegion = mouthBox(tilePoints);
      v.scores.full.push(psnr(got.data, truth.data));
      v.scores.face.push(psnr(got.data, truth.data, region));
      v.scores.ssim.push(ssim(got.data, truth.data));
      v.scores.faceSsim.push(ssim(got.data, truth.data, region));
      v.scores.mouthPsnr.push(psnr(got.data, truth.data, mouthRegion));
      const frozen = await frozenTile(ref, v.frozen);
      v.scores.frozenPsnr.push(psnr(frozen, truth.data));
      v.scores.frozenFacePsnr.push(psnr(frozen, truth.data, region));
      v.scores.frozenSsim.push(ssim(frozen, truth.data, region));
      if (truthGap === null) {
        truthGap = truthMeasure(truth.canvas);
        truthShown = { truth, region, mouthRegion, ref };
      }
      const gap = v.measure(got.canvas);
      if (gap !== null && truthGap !== null) {
        v.scores.gaps.push(gap);
        v.scores.truthGaps.push(truthGap);
      }
      v.shown = got;
      if (v.primary) {
        const sent = main.stream[at].sent;
        main.exactView.draw({ points: entry.points, pose: sent.pose, expression: fs.faceStreamExpression(entry.reading, entry.points), stale: false });
        const ideal = grabTile(main.exactView.canvas);
        extra.codecPsnr.push(psnr(got.data, ideal.data));
        extra.codecSsim.push(ssim(got.data, ideal.data));
        main.lossView.draw();
        const lossy = grabTile(main.lossView.canvas);
        extra.lossPsnr.push(psnr(lossy.data, truth.data));
        extra.lossFacePsnr.push(psnr(lossy.data, truth.data, region));
        main.lossShown = lossy;
        main.lossStale = Boolean(main.lossView.receiver.frame?.stale);
      }
    }
    if (truthGap !== null) truthGaps.push(truthGap);
    if (options.stills.includes(entry.f) && truthShown && main.shown) {
      const strip = canvasOf(TILE[0] * 3 + 16, TILE[1] + 28);
      const ctx = strip.getContext("2d");
      ctx.fillStyle = "#111";
      ctx.fillRect(0, 0, strip.width, strip.height);
      ctx.drawImage(truthShown.truth.canvas, 0, 28);
      ctx.drawImage(main.shown.canvas, TILE[0] + 8, 28);
      ctx.drawImage(main.lossShown.canvas, 2 * TILE[0] + 16, 28);
      ctx.fillStyle = "#fff";
      ctx.font = "14px sans-serif";
      ctx.fillText(`camera (truth), frame ${entry.f}`, 6, 19);
      ctx.fillText(`face stream, ${main.stream[at].packet.length} B`, TILE[0] + 14, 19);
      ctx.fillText(`face stream, ${Math.round(options.loss * 100)}% loss${main.lossStale ? " (stale)" : ""}`, 2 * TILE[0] + 22, 19);
      const still = { frame: entry.f, png: strip.toDataURL("image/png") };
      const base = versions.find((v) => !v.primary);
      if (base?.shown) {
        const zoom = 2;
        const mr = truthShown.mouthRegion;
        const mw = mr[2] - mr[0];
        const mh = mr[3] - mr[1];
        const compare = canvasOf(TILE[0] * 3 + 16, TILE[1] + 28 + mh * zoom + 8);
        const cctx = compare.getContext("2d");
        cctx.fillStyle = "#111";
        cctx.fillRect(0, 0, compare.width, compare.height);
        const panels = [[truthShown.truth.canvas, `camera (truth), frame ${entry.f}`], [base.shown.canvas, "face stream v1"], [main.shown.canvas, "face stream v2"]];
        panels.forEach(([canvas, label], k) => {
          const x = k * (TILE[0] + 8);
          cctx.drawImage(canvas, x, 28);
          cctx.imageSmoothingEnabled = false;
          const zx = x + Math.max(0, Math.floor((TILE[0] - mw * zoom) / 2));
          cctx.drawImage(canvas, mr[0], mr[1], mw, mh, zx, TILE[1] + 36, mw * zoom, mh * zoom);
          cctx.fillStyle = "#fff";
          cctx.font = "14px sans-serif";
          cctx.fillText(label, x + 6, 19);
        });
        still.compare = compare.toDataURL("image/png");
      }
      stills.push(still);
    }
  }
  main.view.receiver.frame && main.view.draw();
  const tileState = { frame: frames.length - 1, ready: main.view.ready, badge: main.view.badge.textContent, label: main.view.canvas.getAttribute("aria-label") };
  const mean = (list) => list.reduce((s, v) => s + v, 0) / Math.max(1, list.length);
  const round = (v, k = 3) => Math.round(v * 10 ** k) / 10 ** k;
  const seconds = frames.length / FPS;
  const summarize = (v) => ({
    frames: frames.length,
    precision: options.precision,
    bytesPerFrame: round(v.bytes / frames.length, 2),
    keyBytes: round(v.keyBytes / Math.max(1, v.keys), 2),
    deltaBytes: round(v.deltaBytes / Math.max(1, v.deltas), 2),
    frameKbps: round((v.bytes * 8) / seconds / 1000, 2),
    references: v.references,
    referenceBytes: round(v.referenceBytes / Math.max(1, v.references), 0),
    atlases: v.atlases,
    atlasBytes: round(v.atlasBytes / Math.max(1, v.atlases), 0),
    totalKbps: round(((v.bytes + v.referenceBytes + v.atlasBytes) * 8) / seconds / 1000, 2),
    psnrTile: round(mean(v.scores.full), 2),
    psnrFace: round(mean(v.scores.face), 2),
    psnrMouth: round(mean(v.scores.mouthPsnr), 2),
    ssimTile: round(mean(v.scores.ssim)),
    ssimFace: round(mean(v.scores.faceSsim)),
    psnrTileFrozen: round(mean(v.scores.frozenPsnr), 2),
    psnrFaceFrozen: round(mean(v.scores.frozenFacePsnr), 2),
    ssimFaceFrozen: round(mean(v.scores.frozenSsim)),
    lipSync: round(correlation(v.scores.gaps, v.scores.truthGaps)),
    lipGapError: round(mean(v.scores.gaps.map((g, i) => Math.abs(g - v.scores.truthGaps[i])))),
    lipFrames: v.scores.gaps.length,
  });
  const summary = {
    source: options.clip ? "clip" : `stand-in ${options.face}`,
    ...summarize(main),
    codecRmsPx: round(Math.sqrt((2 * codecSq) / Math.max(1, n))),
    codecWorstPx: round(worst),
    truthRmsPx: round(Math.sqrt((2 * truthSq) / Math.max(1, n))),
    psnrCodecOnly: round(mean(extra.codecPsnr), 2),
    ssimCodecOnly: round(mean(extra.codecSsim), 4),
    loss: options.loss,
    lostFrames: lost,
    staleFrames: stale,
    psnrTileWithLoss: round(mean(extra.lossPsnr), 2),
    psnrFaceWithLoss: round(mean(extra.lossFacePsnr), 2),
    truthGapRange: truthGaps.length ? [round(Math.min(...truthGaps)), round(Math.max(...truthGaps))] : null,
    renderer: main.view.renderer?.gl ? main.view.renderer.gl.getParameter(main.view.renderer.gl.RENDERER) : "",
  };
  const base = versions.find((v) => !v.primary);
  const toBase64 = (bytes) => {
    let text = "";
    for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return btoa(text);
  };
  return {
    summary,
    baseline: base ? { rev: options.baseline, ...summarize(base) } : null,
    scores: { v2: main.scores, v1: base?.scores || null, ...extra },
    stills,
    reference: main.firstReference,
    atlas: main.lastAtlas ? toBase64(fs.decodeFaceStreamAtlas(main.lastAtlas).image) : null,
    tile: tileState,
  };
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
