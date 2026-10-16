#!/usr/bin/env node
"use strict";

const fs = require("fs");
const http = require("http");
const path = require("path");

const REPOSITORY = path.resolve(__dirname, "..", "..");
const PUBLIC = path.join(REPOSITORY, "app", "public");
const NATIVE_PAGE = path.join(REPOSITORY, "native", "ios", "ForkMesh", "Features", "FaceChanger");
const TYPES = {
  ".js": "text/javascript", ".html": "text/html", ".jpg": "image/jpeg", ".png": "image/png",
  ".json": "application/json", ".fmnn": "application/octet-stream",
};

function playwright() {
  const roots = [
    process.env.PLAYWRIGHT_DIR,
    path.join(REPOSITORY, "app", "browser_tests", "node_modules", "playwright"),
    path.join(REPOSITORY, "world", "browser_tests", "node_modules", "playwright"),
  ].filter(Boolean);
  for (const root of roots) {
    try {
      return require(root);
    } catch (_) {}
  }
  throw new Error("playwright is not installed; set PLAYWRIGHT_DIR");
}

function options(argv) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i += 1) {
    if (argv[i].startsWith("--")) {
      const key = argv[i].slice(2);
      const next = argv[i + 1];
      if (next === undefined || next.startsWith("--")) out[key] = true;
      else {
        out[key] = next;
        i += 1;
      }
    } else out._.push(argv[i]);
  }
  return out;
}

const OFFLINE_PAGE = `<!doctype html><meta charset=utf-8><body style="margin:0"><script type=module>
const faces = await import('/chat-call-faces.js');
window.makeStage = async (o) => {
  window.stage = faces.createFaceStage({ manual: true, faceId: o.face || '', backgroundId: o.background || '', backgroundImage: o.image || '', outfitId: o.outfit || '' });
  const t0 = performance.now();
  while (!window.stage.ready() && performance.now() - t0 < 60000) await new Promise((r) => setTimeout(r, 100));
  return window.stage.ready();
};
window.step = async (url, t, keep) => {
  const img = new Image(); img.src = url; await img.decode();
  const gl = window.stage.canvas.getContext('webgl');
  const a = performance.now();
  await window.stage.processFrame(img, t);
  gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array(4));
  const ms = performance.now() - a;
  return { ms, png: keep ? window.stage.canvas.toDataURL('image/png') : '', marks: window.stage.landmarks() };
};
window.ready = true;
</script>`;

function serve(frames) {
  return new Promise((resolve) => {
    const server = http.createServer((req, res) => {
      const url = decodeURIComponent(req.url.split("?")[0]);
      if (url === "/__harness/offline.html") {
        res.writeHead(200, { "Content-Type": "text/html" });
        res.end(OFFLINE_PAGE);
        return;
      }
      const roots = url.startsWith("/__frames/") ? [[frames, url.slice("/__frames".length)]] :
        url.startsWith("/__native/") ? [[NATIVE_PAGE, url.slice("/__native".length)]] : [[PUBLIC, url]];
      for (const [root, rest] of roots) {
        if (!root) continue;
        const file = path.join(root, rest);
        if (file.startsWith(root) && fs.existsSync(file) && fs.statSync(file).isFile()) {
          res.writeHead(200, { "Content-Type": TYPES[path.extname(file)] || "application/octet-stream" });
          fs.createReadStream(file).pipe(res);
          return;
        }
      }
      res.writeHead(404);
      res.end("not found");
    });
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

function launchArgs(opts, extra = []) {
  const gpu = opts.gpu ? ["--use-gl=angle", "--use-angle=gl-egl", "--enable-gpu"] :
    ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"];
  return [...gpu, "--ignore-gpu-blocklist", ...extra];
}

async function render(opts) {
  const frames = path.resolve(opts.frames);
  const out = path.resolve(opts.out);
  fs.mkdirSync(out, { recursive: true });
  const server = await serve(frames);
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await playwright().chromium.launch({ args: launchArgs(opts) });
  const page = await browser.newPage();
  page.on("pageerror", (error) => console.error("page error:", error.message));
  await page.goto(base + "/__harness/offline.html");
  await page.waitForFunction(() => window.ready);
  const ready = await page.evaluate((o) => window.makeStage(o), { face: opts.face || "", background: opts.background || "", outfit: opts.outfit || "" });
  if (!ready) console.error("the stage did not become ready (missing model or image?)");
  const files = fs.readdirSync(frames).filter((name) => /\.(png|jpg)$/.test(name)).sort().slice(0, Number(opts.count || 1e9));
  const every = Number(opts.every || 1);
  const marks = [];
  const times = [];
  for (let i = 0; i < files.length; i += 1) {
    const keep = i % every === 0;
    const step = await page.evaluate(([url, t, k]) => window.step(url, t, k), [`/__frames/${files[i]}`, (i * 1000) / 24, keep]);
    marks.push(step.marks);
    times.push(step.ms);
    if (keep) fs.writeFileSync(path.join(out, `out_${String(i).padStart(4, "0")}.png`), Buffer.from(step.png.split(",")[1], "base64"));
  }
  const stats = await page.evaluate(() => window.stage.stats);
  fs.writeFileSync(path.join(out, "marks.json"), JSON.stringify(marks));
  const sorted = [...times].sort((a, b) => a - b);
  const summary = {
    frames: files.length,
    medianMs: sorted[Math.floor(sorted.length / 2)],
    p90Ms: sorted[Math.floor(sorted.length * 0.9)],
    level: stats.level,
    network: stats.network,
    hair: stats.hair,
  };
  fs.writeFileSync(path.join(out, "summary.json"), JSON.stringify(summary, null, 2));
  console.log(JSON.stringify(summary));
  await browser.close();
  server.close();
}

async function native(opts) {
  const server = await serve(null);
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await playwright().chromium.launch({
    args: launchArgs(opts, [
      "--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream",
      `--use-file-for-fake-video-capture=${path.resolve(opts.clip)}`, "--autoplay-policy=no-user-gesture-required",
    ]),
  });
  const page = await browser.newPage({ viewport: { width: Number(opts.width || 640), height: Number(opts.height || 360) } });
  const errors = [];
  page.on("pageerror", (error) => errors.push(error.message));
  page.on("console", (message) => { if (message.type() === "error") errors.push(message.text()); });
  await page.goto(base + "/__native/direct-call.html");
  await page.evaluate(() => {
    window.__results = [];
    window.FmCallBridge = { post: (text) => window.__results.push(JSON.parse(text)) };
  });
  await page.evaluate(([face, background]) => window.fmCall(1, "preview", ["video", "", face, background]), [opts.face || "", opts.background || ""]);
  await page.waitForTimeout(Number(opts.wait || 20000));
  await page.screenshot({ path: path.resolve(opts.out) });
  const results = await page.evaluate(() => window.__results);
  console.log(JSON.stringify({ preview: results.find((r) => r.id === 1) || null, errors }));
  await browser.close();
  server.close();
}

async function models(opts) {
  const server = await serve(null);
  const base = `http://127.0.0.1:${server.address().port}`;
  const browser = await playwright().chromium.launch({ args: launchArgs(opts) });
  const page = await browser.newPage();
  await page.goto(base + "/__harness/offline.html");
  const result = await page.evaluate(async () => {
    const seg = await import("/chat-call-segment.js");
    const loaded = await seg.loadSegmentModels();
    const gl = document.createElement("canvas").getContext("webgl");
    const out = {};
    for (const [name, model] of Object.entries(loaded)) {
      const net = seg.createNetwork(gl, model);
      const times = [];
      for (let i = 0; i < 23; i += 1) {
        const started = performance.now();
        net.run();
        net.read();
        if (i >= 3) times.push(performance.now() - started);
      }
      times.sort((a, b) => a - b);
      out[name] = { medianMs: Math.round(times[10] * 100) / 100, layers: model.ops.length, programs: net.programCount };
      net.dispose();
    }
    return out;
  });
  console.log(JSON.stringify(result));
  await browser.close();
  server.close();
}

const COMMANDS = { render, native, models };

async function main() {
  const [command, ...rest] = process.argv.slice(2);
  if (!COMMANDS[command]) {
    console.log("usage: call_video_harness.cjs render --frames DIR [--face ID] [--background ID] [--outfit ID] --out DIR [--every N] [--count N] [--gpu]");
    console.log("       call_video_harness.cjs native --clip FILE.y4m [--face ID] [--background ID] --out FILE.png [--gpu]");
    console.log("       call_video_harness.cjs models [--gpu]");
    process.exit(2);
  }
  await COMMANDS[command](options(rest));
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
