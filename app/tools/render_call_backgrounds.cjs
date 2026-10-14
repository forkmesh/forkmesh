#!/usr/bin/env node
"use strict";

const fs = require("fs");
const http = require("http");
const path = require("path");

const REPOSITORY = path.resolve(__dirname, "..", "..");
const PLAYWRIGHT_CANDIDATES = [
  process.env.PLAYWRIGHT_DIR,
  path.join(REPOSITORY, "app", "browser_tests", "node_modules", "playwright"),
  path.join(REPOSITORY, "world", "browser_tests", "node_modules", "playwright"),
  "playwright",
].filter(Boolean);
const STEMS = ["world", "office", "meeting", "library", "skyline"];
const WORLD_HOUR = 16.5;
const TYPES = {
  ".js": "text/javascript",
  ".mjs": "text/javascript",
  ".html": "text/html",
  ".json": "application/json",
  ".css": "text/css",
  ".png": "image/png",
  ".svg": "image/svg+xml",
  ".webp": "image/webp",
  ".jpg": "image/jpeg",
  ".avif": "image/avif",
  ".woff2": "font/woff2",
  ".glb": "model/gltf-binary",
  ".wasm": "application/wasm",
};

function loadPlaywright() {
  for (const candidate of PLAYWRIGHT_CANDIDATES) {
    try {
      return require(candidate);
    } catch {
      continue;
    }
  }
  throw new Error("playwright is not installed; run npm ci in app/browser_tests");
}

function parseArgs(argv) {
  const options = { stems: [], samples: 0, portrait: false, out: path.join(REPOSITORY, "app", "public", "assets", "call-backgrounds") };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === "--samples") options.samples = Number(argv[++i]);
    else if (arg === "--portrait") options.portrait = true;
    else if (arg === "--out") options.out = path.resolve(argv[++i]);
    else if (STEMS.includes(arg)) options.stems.push(arg);
    else throw new Error(`unknown argument ${arg}; stems are ${STEMS.join(", ")}`);
  }
  if (!options.stems.length) options.stems = STEMS.slice();
  return options;
}

const IMPORTMAP = '<script type="importmap">{"imports":{"three":"/world/vendor/three-0.184.0/three.module.min.js"}}</script>';

const SCENE_PAGE = `<!doctype html><html><head><meta charset="utf-8">${IMPORTMAP}</head><body style="margin:0;background:#000">
<script type="module">
import * as S from "/app/tools/call-backgrounds/scenes.js";
window.__S = S;
window.__ready = true;
</script></body></html>`;

const WORLD_PAGE = `<!doctype html><html><head><meta charset="utf-8">${IMPORTMAP}
<style>html,body{margin:0;background:#000;overflow:hidden}#world{width:1920px;height:1080px;position:relative}#world.portrait{width:1080px;height:1920px}#labels{display:none}</style></head>
<body><div id="world"></div><div id="labels"></div>
<script>
(() => {
  const RealDate = Date;
  const hour = ${WORLD_HOUR};
  class FixedDate extends RealDate {
    constructor(...args) {
      if (args.length) {
        super(...args);
      } else {
        const now = new RealDate();
        now.setHours(Math.floor(hour), Math.round((hour % 1) * 60), 0, 0);
        super(now.getTime());
      }
    }
    static now() {
      return new FixedDate().getTime();
    }
  }
  window.Date = FixedDate;
  window.__seed = 1;
  Math.random = () => {
    window.__seed = (window.__seed * 16807) % 2147483647;
    return (window.__seed - 1) / 2147483646;
  };
})();
</script>
<script type="module">
import * as THREE from "three";
import * as S from "/app/tools/call-backgrounds/scenes.js";
import * as world from "/world/world-scene.js";
window.__S = S;
if (new URLSearchParams(location.search).has("portrait")) {
  S.setPortrait(true);
  document.getElementById("world").classList.add("portrait");
}
try { await world.QR_MODULE_READY; } catch {}
window.__world = await world.createWorldScene({
  THREE,
  container: document.getElementById("world"),
  labelLayer: document.getElementById("labels"),
  identity: { id: "call-background", name: "Guest", flag: "", countryCode: "", browser: "Hidden", os: "Hidden", accountStatus: "guest", emailVerified: false, isAdmin: false },
  qualityTier: "desktop-high",
  adaptiveQuality: false,
  reducedMotion: true,
});
window.__ready = true;
</script></body></html>`;

function serve() {
  const roots = [path.join(REPOSITORY, "world", "public"), REPOSITORY];
  const server = http.createServer((request, response) => {
    const url = decodeURIComponent(request.url.split("?")[0]);
    if (url === "/__scene.html" || url === "/__world.html") {
      response.writeHead(200, { "content-type": "text/html" });
      response.end(url === "/__scene.html" ? SCENE_PAGE : WORLD_PAGE);
      return;
    }
    for (const root of roots) {
      const file = path.join(root, url);
      if (!file.startsWith(REPOSITORY)) continue;
      if (fs.existsSync(file) && fs.statSync(file).isFile()) {
        response.writeHead(200, { "content-type": TYPES[path.extname(file)] || "application/octet-stream" });
        fs.createReadStream(file).pipe(response);
        return;
      }
    }
    response.writeHead(404);
    response.end();
  });
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve(server)));
}

function writeResult(out, stem, result, portrait) {
  fs.mkdirSync(out, { recursive: true });
  const decode = (url) => Buffer.from(url.slice(url.indexOf(",") + 1), "base64");
  if (portrait) {
    fs.writeFileSync(path.join(out, `${stem}-portrait.jpg`), decode(result.full));
  } else {
    fs.writeFileSync(path.join(out, `${stem}.jpg`), decode(result.full));
    fs.writeFileSync(path.join(out, `${stem}-thumb.jpg`), decode(result.thumb));
  }
  const summary = { ...result };
  delete summary.full;
  delete summary.thumb;
  process.stdout.write(`${stem}${portrait ? " portrait" : ""}: ${JSON.stringify(summary)}\n`);
}

async function openPage(browser, port, name, portrait) {
  const viewport = portrait ? { width: 1080, height: 1920 } : { width: 1920, height: 1080 };
  const page = await browser.newPage({ viewport, deviceScaleFactor: 1 });
  page.on("pageerror", (error) => process.stderr.write(`${name} page error: ${String(error).slice(0, 400)}\n`));
  page.on("console", (message) => {
    if (message.type() === "error") process.stderr.write(`${name} console: ${message.text().slice(0, 400)}\n`);
  });
  await page.goto(`http://127.0.0.1:${port}/__${name}.html${portrait ? "?portrait" : ""}`);
  await page.waitForFunction("window.__ready === true", null, { timeout: 900000, polling: 1000 });
  return page;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const { chromium } = loadPlaywright();
  const server = await serve();
  const port = server.address().port;
  const browser = await chromium.launch({
    args: ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"],
  });
  try {
    const sceneStems = options.stems.filter((stem) => stem !== "world");
    if (sceneStems.length) {
      const page = await openPage(browser, port, "scene", options.portrait);
      for (const stem of sceneStems) {
        const result = await page.evaluate(
          ([name, samples, portrait]) => {
            window.__S.setPortrait(portrait);
            return window.__S.renderStem(name, samples ? { samples } : {});
          },
          [stem, options.samples, options.portrait],
        );
        writeResult(options.out, stem, result, options.portrait);
      }
      await page.close();
    }
    if (options.stems.includes("world")) {
      const page = await openPage(browser, port, "world", options.portrait);
      const result = await page.evaluate(
        (samples) => window.__S.renderWorld(window.__world, samples ? { samples } : {}),
        options.samples,
      );
      writeResult(options.out, "world", result, options.portrait);
      await page.close();
    }
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch((error) => {
  process.stderr.write(`${error.stack || error}\n`);
  process.exit(1);
});
