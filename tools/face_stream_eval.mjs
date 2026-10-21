#!/usr/bin/env node
import { createRequire } from "node:module";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const publicDir = path.join(root, "app", "public");
const require = createRequire(import.meta.url);
const TYPES = { ".js": "text/javascript", ".mjs": "text/javascript", ".jpg": "image/jpeg", ".png": "image/png", ".fmnn": "application/octet-stream" };

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
  const out = { dir: "", frames: 180, every: 6, precision: 96, loss: 0.1, face: "maya", noise: 0.6, stills: [0, 36, 72, 108, 144, 174] };
  for (let i = 0; i < argv.length; i += 1) {
    const [key, value] = argv[i].startsWith("--") ? [argv[i].slice(2), argv[i + 1]] : ["dir", argv[i]];
    if (key !== "dir" || !argv[i].startsWith("--")) {
      if (key === "dir") out.dir = value;
      else if (key === "face") out.face = value;
      else out[key] = Number(value);
      if (argv[i].startsWith("--")) i += 1;
    }
  }
  return out;
}

async function main() {
  const options = args(process.argv.slice(2));
  if (!options.dir) throw new Error("usage: face_stream_eval.mjs <output dir> [--frames 180] [--every 6] [--precision 96] [--loss 0.1] [--face maya]");
  await mkdir(options.dir, { recursive: true });
  const { chromium } = loadPlaywright();
  const browser = await chromium.launch({ args: ["--enable-unsafe-swiftshader", "--use-angle=swiftshader", "--ignore-gpu-blocklist"] });
  try {
    const page = await browser.newPage();
    page.on("pageerror", (error) => console.error("page error:", error.message));
    await page.route("http://fm.test/**", async (route) => {
      const url = new URL(route.request().url());
      if (url.pathname === "/") {
        await route.fulfill({ status: 200, contentType: "text/html", body: "<!doctype html><html><body></body></html>" });
        return;
      }
      const file = path.join(publicDir, decodeURIComponent(url.pathname));
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
    const result = await page.evaluate(evaluate, options);
    for (const still of result.stills) {
      const name = `face-stream-${String(still.frame).padStart(3, "0")}.png`;
      await writeFile(path.join(options.dir, name), Buffer.from(still.png.split(",")[1], "base64"));
      still.file = name;
      delete still.png;
    }
    if (result.reference) {
      await writeFile(path.join(options.dir, "face-stream-reference.jpg"), Buffer.from(result.reference, "base64"));
      result.reference = "face-stream-reference.jpg";
    }
    await page.locator(".fm-face-stream").screenshot({ path: path.join(options.dir, "face-stream-tile.png") });
    result.tile.file = "face-stream-tile.png";
    const report = path.join(options.dir, "face-stream-eval.json");
    await writeFile(report, JSON.stringify(result, null, 2) + "\n");
    console.log(JSON.stringify(result.summary));
  } finally {
    await browser.close();
  }
}

async function evaluate(options) {
  const fs = await import("/chat-call-facestream.js");
  const faces = await import("/chat-call-faces.js");
  const geo = await import("/chat-call-face-geometry.js");
  const W = 1280;
  const H = 720;
  const FPS = 30;
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
    return { points, expression: { lidLeft: blink, lidRight: blink, gazeX: gaze, gazeY: gaze * 0.3, mouth: open, smile, tongue }, yaw, pitch, roll };
  };

  const camera = document.createElement("canvas");
  const truthRenderer = fs.createFaceStreamRenderer(camera);
  const [photo, matte, room] = await Promise.all([
    image(`/assets/call-faces/${person.id}.jpg`),
    image(`/assets/call-faces/${person.id}-matte.png`),
    image("/assets/call-backgrounds/office.jpg"),
  ]);
  const roomCanvas = document.createElement("canvas");
  roomCanvas.width = W;
  roomCanvas.height = H;
  const roomCtx = roomCanvas.getContext("2d", { willReadFrequently: true });
  roomCtx.drawImage(room, 0, 0, W, H);
  const roomPixels = roomCtx.getImageData(0, 0, W, H);
  truthRenderer.setBackdrop(roomPixels);
  truthRenderer.setFace({ photo, matte, mesh: head.mesh, layout: head.layout, model: head.model, size: 512 });
  const grab = document.createElement("canvas");
  grab.width = W;
  grab.height = H;
  const grabCtx = grab.getContext("2d", { willReadFrequently: true });
  const renderTruth = (truth) => {
    truthRenderer.draw(W, H, truth.points, truth.expression);
    grabCtx.drawImage(camera, 0, 0);
    return grabCtx.getImageData(0, 0, W, H);
  };
  const personMask = (frame) => {
    const bytes = new Uint8Array(W * H);
    for (let i = 0; i < W * H; i += 1) {
      const d = Math.abs(frame.data[i * 4] - roomPixels.data[i * 4]) + Math.abs(frame.data[i * 4 + 1] - roomPixels.data[i * 4 + 1]) + Math.abs(frame.data[i * 4 + 2] - roomPixels.data[i * 4 + 2]);
      bytes[i] = d > 24 ? 255 : 0;
    }
    return { bytes, width: W, height: H };
  };

  const sender = fs.createFaceStreamSender({ width: W, height: H, fps: FPS, precision: options.precision });
  const smoother = geo.createPointSmoother(68, 1.5, 1.5);
  const prepared = [];
  const frames = [];
  let referenceBytes = 0;
  let referenceCount = 0;
  let firstReference = null;
  const referencePackets = [];
  for (let f = 0; f < options.frames; f += 1) {
    const time = 1000 + Math.round((f * 1000) / FPS);
    const truth = truthAt(f);
    const seen = truth.points.map(([x, y]) => [x + options.noise * gauss(), y + options.noise * gauss()]);
    const points = smoother.smooth(seen, time, geo.faceFrame(seen).span).map(([x, y]) => [x, y]);
    const reading = { close: [truth.expression.lidLeft, truth.expression.lidRight], look: [[truth.expression.gazeX, truth.expression.gazeY], [truth.expression.gazeX, truth.expression.gazeY]], tongue: truth.expression.tongue };
    const pose = fs.faceStreamPose(points);
    const expression = fs.faceStreamExpression(reading, points);
    const packet = sender.frame({ time, points, pose, expression });
    const sent = sender.encoder.last;
    let reference = null;
    const needFrame = f % options.every === 0 || options.stills.includes(f);
    const score = sender.score({ points, pose, expression });
    let picture = null;
    if (sender.due(time, score) || needFrame) picture = renderTruth(truth);
    if (sender.due(time, score)) {
      const source = document.createElement("canvas");
      source.width = W;
      source.height = H;
      source.getContext("2d").putImageData(picture, 0, 0);
      reference = await sender.reference({ time, source, points, mask: personMask(picture), score });
      referenceBytes += reference.length;
      referencePackets.push({ f, packet: reference });
      referenceCount += 1;
      const decoded = fs.decodeFaceStreamReference(reference);
      prepared.push({ from: f, value: await fs.prepareFaceStreamReference(decoded) });
      if (!firstReference) {
        let text = "";
        for (let i = 0; i < decoded.image.length; i += 0x8000) text += String.fromCharCode(...decoded.image.subarray(i, i + 0x8000));
        firstReference = btoa(text);
      }
    }
    frames.push({ f, time, truth, points, packet, sent, picture: needFrame ? picture : null });
  }

  const tile = () => {
    const canvas = document.createElement("canvas");
    canvas.width = TILE[0];
    canvas.height = TILE[1];
    return { canvas, renderer: fs.createFaceStreamRenderer(canvas), id: -1 };
  };
  const decodedView = tile();
  const exactView = tile();
  const lossView = tile();
  const read = (canvas) => {
    const c = document.createElement("canvas");
    c.width = TILE[0];
    c.height = TILE[1];
    const ctx = c.getContext("2d", { willReadFrequently: true });
    ctx.drawImage(canvas, 0, 0);
    return ctx.getImageData(0, 0, TILE[0], TILE[1]);
  };
  const truthTile = (picture, ref) => {
    const source = document.createElement("canvas");
    source.width = W;
    source.height = H;
    source.getContext("2d").putImageData(picture, 0, 0);
    const c = document.createElement("canvas");
    c.width = TILE[0];
    c.height = TILE[1];
    const ctx = c.getContext("2d", { willReadFrequently: true });
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(source, ref.cropX, ref.cropY, ref.cropW, ref.cropH, 0, 0, TILE[0], TILE[1]);
    return { data: ctx.getImageData(0, 0, TILE[0], TILE[1]), canvas: c };
  };
  const box = (points) => {
    const xs = points.map((p) => p[0]);
    const ys = points.map((p) => p[1]);
    const pad = 0.15 * (Math.max(...xs) - Math.min(...xs));
    return [Math.max(0, Math.floor(Math.min(...xs) - pad)), Math.max(0, Math.floor(Math.min(...ys) - 2.5 * pad)), Math.min(TILE[0], Math.ceil(Math.max(...xs) + pad)), Math.min(TILE[1], Math.ceil(Math.max(...ys) + pad))];
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
        let ma = 0;
        let mb = 0;
        const va = [];
        const vb = [];
        for (let y = y0; y < y0 + 8; y += 1) {
          for (let x = x0; x < x0 + 8; x += 1) {
            const at = (y * TILE[0] + x) * 4;
            va.push(luma(a, at));
            vb.push(luma(b, at));
          }
        }
        ma = va.reduce((s, v) => s + v, 0) / 64;
        mb = vb.reduce((s, v) => s + v, 0) / 64;
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
  const useReference = (view, f) => {
    let active = null;
    for (const entry of prepared) if (entry.from <= f) active = entry.value;
    if (active && view.id !== active.id + 1000 * prepared.indexOf(prepared.find((e) => e.value === active))) {
      view.id = active.id + 1000 * prepared.indexOf(prepared.find((e) => e.value === active));
      view.renderer.setBackdrop(active.backdrop);
      view.renderer.setFace({ photo: active.built.photo, matte: active.built.matte, mesh: active.head.mesh, layout: active.head.layout, model: active.head.model, size: 512 });
    }
    return active;
  };

  const decoder = fs.createFaceStreamDecoder();
  const lossy = fs.createFaceStreamDecoder();
  let lossSeed = 0x51;
  const dropped = () => {
    lossSeed = (lossSeed * 1103515245 + 12345) % 2147483648;
    return lossSeed / 2147483648 < options.loss;
  };
  let bytes = 0;
  let keyBytes = 0;
  let keys = 0;
  let deltaBytes = 0;
  let deltas = 0;
  let codecSq = 0;
  let truthSq = 0;
  let worst = 0;
  let n = 0;
  let lost = 0;
  let stale = 0;
  let lossSq = 0;
  let lossN = 0;
  const scores = { full: [], face: [], ssim: [], faceSsim: [], codecPsnr: [], codecSsim: [], lossPsnr: [], lossFacePsnr: [], frozenPsnr: [], frozenFacePsnr: [], frozenSsim: [] };
  const frozenTile = async (active) => {
    if (active.frozen) return active.frozen;
    const bitmap = await createImageBitmap(new Blob([active.reference.image], { type: "image/jpeg" }));
    const c = document.createElement("canvas");
    c.width = TILE[0];
    c.height = TILE[1];
    const ctx = c.getContext("2d", { willReadFrequently: true });
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(bitmap, 0, 0, TILE[0], TILE[1]);
    active.frozen = ctx.getImageData(0, 0, TILE[0], TILE[1]);
    return active.frozen;
  };
  const stills = [];
  let lossFrame = null;
  let goodMouth = NaN;
  for (const entry of frames) {
    const frame = decoder.decode(entry.packet);
    bytes += entry.packet.length;
    if (frame.key) {
      keys += 1;
      keyBytes += entry.packet.length;
    } else {
      deltas += 1;
      deltaBytes += entry.packet.length;
    }
    frame.points.forEach((p, i) => {
      for (let axis = 0; axis < 2; axis += 1) {
        const e = p[axis] - entry.points[i][axis];
        codecSq += e * e;
        worst = Math.max(worst, Math.abs(e));
        const t = p[axis] - entry.truth.points[i][axis];
        truthSq += t * t;
        n += 1;
      }
    });
    if (dropped() && entry.f > 0) {
      lost += 1;
    } else {
      try {
        lossFrame = lossy.decode(entry.packet);
        if (!lossFrame.stale) goodMouth = lossFrame.expression.mouth;
        lossFrame = fs.faceStreamConceal(lossFrame, goodMouth);
        if (lossFrame.stale) {
          stale += 1;
          lossFrame.points.forEach((p, i) => {
            lossSq += (p[0] - entry.points[i][0]) ** 2 + (p[1] - entry.points[i][1]) ** 2;
            lossN += 2;
          });
        }
      } catch (_) {}
    }
    if (!entry.picture) continue;
    const active = useReference(decodedView, entry.f);
    useReference(exactView, entry.f);
    useReference(lossView, entry.f);
    if (!active) continue;
    const ref = active.reference;
    const live = fs.faceStreamLivePoints(frame, ref, TILE[0], TILE[1]);
    decodedView.renderer.draw(TILE[0], TILE[1], live, frame.expression);
    const exact = fs.faceStreamLivePoints({ points: entry.points, pose: entry.sent.pose }, ref, TILE[0], TILE[1]);
    const reading = fs.faceStreamExpression({ close: [entry.truth.expression.lidLeft, entry.truth.expression.lidRight], look: [[entry.truth.expression.gazeX, entry.truth.expression.gazeY], [entry.truth.expression.gazeX, entry.truth.expression.gazeY]], tongue: entry.truth.expression.tongue }, entry.points);
    exactView.renderer.draw(TILE[0], TILE[1], exact, reading);
    if (lossFrame) lossView.renderer.draw(TILE[0], TILE[1], fs.faceStreamLivePoints(lossFrame, ref, TILE[0], TILE[1]), lossFrame.expression);
    const got = read(decodedView.canvas);
    const ideal = read(exactView.canvas);
    const lossy2 = read(lossView.canvas);
    const truth = truthTile(entry.picture, ref);
    const region = box(fs.faceStreamLivePoints({ points: entry.truth.points.map(([x, y]) => [x, y]), pose: frame.pose }, ref, TILE[0], TILE[1]));
    scores.full.push(psnr(got, truth.data));
    scores.face.push(psnr(got, truth.data, region));
    scores.ssim.push(ssim(got, truth.data));
    scores.faceSsim.push(ssim(got, truth.data, region));
    scores.codecPsnr.push(psnr(got, ideal));
    scores.codecSsim.push(ssim(got, ideal));
    scores.lossPsnr.push(psnr(lossy2, truth.data));
    scores.lossFacePsnr.push(psnr(lossy2, truth.data, region));
    const frozen = await frozenTile(active);
    scores.frozenPsnr.push(psnr(frozen, truth.data));
    scores.frozenFacePsnr.push(psnr(frozen, truth.data, region));
    scores.frozenSsim.push(ssim(frozen, truth.data, region));
    if (options.stills.includes(entry.f)) {
      const strip = document.createElement("canvas");
      strip.width = TILE[0] * 3 + 16;
      strip.height = TILE[1] + 28;
      const ctx = strip.getContext("2d");
      ctx.fillStyle = "#111";
      ctx.fillRect(0, 0, strip.width, strip.height);
      ctx.drawImage(truth.canvas, 0, 28);
      ctx.drawImage(decodedView.canvas, TILE[0] + 8, 28);
      ctx.drawImage(lossView.canvas, 2 * TILE[0] + 16, 28);
      ctx.fillStyle = "#fff";
      ctx.font = "14px sans-serif";
      ctx.fillText(`camera (truth), frame ${entry.f}`, 6, 19);
      ctx.fillText(`face stream, ${entry.packet.length} B`, TILE[0] + 14, 19);
      ctx.fillText(`face stream, ${Math.round(options.loss * 100)}% loss${lossFrame?.stale ? " (stale)" : ""}`, 2 * TILE[0] + 22, 19);
      stills.push({ frame: entry.f, png: strip.toDataURL("image/png") });
    }
  }
  const view = fs.createFaceStreamView({ width: TILE[0], height: TILE[1] });
  document.body.append(view.element);
  const shown = Math.min(frames.length - 1, Math.round(frames.length * 0.4));
  for (const entry of frames.slice(0, shown + 1)) {
    for (const ref of referencePackets) if (ref.f === entry.f) await view.receive(ref.packet);
    await view.receive(entry.packet);
  }
  const tileState = { frame: shown, ready: view.ready, badge: view.badge.textContent, label: view.canvas.getAttribute("aria-label") };
  const mean = (list) => list.reduce((s, v) => s + v, 0) / Math.max(1, list.length);
  const round = (v, k = 3) => Math.round(v * 10 ** k) / 10 ** k;
  const summary = {
    frames: frames.length,
    precision: options.precision,
    bytesPerFrame: round(bytes / frames.length, 2),
    keyBytes: round(keyBytes / Math.max(1, keys), 2),
    deltaBytes: round(deltaBytes / Math.max(1, deltas), 2),
    frameKbps: round((bytes * 8 * FPS) / frames.length / 1000, 2),
    references: referenceCount,
    referenceBytes: round(referenceBytes / Math.max(1, referenceCount), 0),
    totalKbps: round(((bytes + referenceBytes) * 8) / (frames.length / FPS) / 1000, 2),
    codecRmsPx: round(Math.sqrt((2 * codecSq) / n)),
    codecWorstPx: round(worst),
    truthRmsPx: round(Math.sqrt((2 * truthSq) / n)),
    psnrTile: round(mean(scores.full), 2),
    psnrFace: round(mean(scores.face), 2),
    ssimTile: round(mean(scores.ssim)),
    ssimFace: round(mean(scores.faceSsim)),
    psnrCodecOnly: round(mean(scores.codecPsnr), 2),
    ssimCodecOnly: round(mean(scores.codecSsim), 4),
    loss: options.loss,
    lostFrames: lost,
    staleFrames: stale,
    staleRmsPx: lossN ? round(Math.sqrt((2 * lossSq) / lossN)) : 0,
    psnrTileFrozen: round(mean(scores.frozenPsnr), 2),
    psnrFaceFrozen: round(mean(scores.frozenFacePsnr), 2),
    ssimFaceFrozen: round(mean(scores.frozenSsim)),
    psnrTileWithLoss: round(mean(scores.lossPsnr), 2),
    psnrFaceWithLoss: round(mean(scores.lossFacePsnr), 2),
    renderer: (() => {
      const gl = decodedView.renderer.gl;
      return gl.getParameter(gl.RENDERER);
    })(),
  };
  return { summary, scores, stills, reference: firstReference, tile: tileState };
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
