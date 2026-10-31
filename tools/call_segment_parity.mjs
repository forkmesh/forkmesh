#!/usr/bin/env node
import { createRequire } from "node:module";
import { readFile, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const publicDir = path.join(root, "app", "public");
const require = createRequire(import.meta.url);

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

const TYPES = { ".js": "text/javascript", ".mjs": "text/javascript", ".jpg": "image/jpeg", ".png": "image/png", ".fmnn": "application/octet-stream", ".html": "text/html" };

async function main() {
  const outputs = process.argv.slice(2);
  if (!outputs.length) throw new Error("usage: call_segment_parity.mjs <fixture.json> [...]");
  const { chromium } = loadPlaywright();
  const browser = await chromium.launch({ args: ["--enable-unsafe-swiftshader", "--use-angle=swiftshader", "--ignore-gpu-blocklist"] });
  try {
    const page = await browser.newPage();
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
        const body = await readFile(file);
        await route.fulfill({ status: 200, contentType: TYPES[path.extname(file)] || "application/octet-stream", body });
      } catch (_) {
        await route.fulfill({ status: 404, body: "" });
      }
    });
    await page.goto("http://fm.test/");
    const fixture = await page.evaluate(buildFixture);
    const text = JSON.stringify({ generator: "tools/call_segment_parity.mjs", ...fixture }) + "\n";
    for (const output of outputs) await writeFile(output, text);
    console.log(`wrote ${outputs.join(", ")} (${text.length} bytes, ${fixture.renderer})`);
  } finally {
    await browser.close();
  }
}

async function buildFixture() {
  const seg = await import("/chat-call-segment.js");
  const toBase64 = (bytes) => {
    let text = "";
    for (let i = 0; i < bytes.length; i += 0x8000) text += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
    return btoa(text);
  };
  const halfOf = (value) => {
    const f32 = new Float32Array([value]);
    const x = new Uint32Array(f32.buffer)[0];
    const sign = (x >>> 16) & 0x8000;
    const exponent = (x >>> 23) & 0xff;
    let mantissa = x & 0x7fffff;
    if (exponent === 0xff) return sign | (mantissa ? 0x7e00 : 0x7c00);
    const unbiased = exponent - 127 + 15;
    if (unbiased >= 31) return sign | 0x7c00;
    if (unbiased <= 0) {
      if (unbiased < -10) return sign;
      mantissa |= 0x800000;
      const shift = 14 - unbiased;
      let half = mantissa >>> shift;
      const rest = mantissa & ((1 << shift) - 1);
      const halfway = 1 << (shift - 1);
      if (rest > halfway || (rest === halfway && (half & 1))) half += 1;
      return sign | half;
    }
    let half = (unbiased << 10) | (mantissa >>> 13);
    const rest = mantissa & 0x1fff;
    if (rest > 0x1000 || (rest === 0x1000 && (half & 1))) half += 1;
    return sign | half;
  };
  const halves = (values) => {
    const out = new Uint16Array(values.length);
    for (let i = 0; i < values.length; i += 1) out[i] = halfOf(values[i]);
    return toBase64(new Uint8Array(out.buffer));
  };
  const image = (src) => new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = src;
  });
  const canvas = document.createElement("canvas");
  const gl = canvas.getContext("webgl", { premultipliedAlpha: false, preserveDrawingBuffer: true });
  const format = seg.floatTargets(gl);
  const half = gl.getExtension("OES_texture_half_float");
  gl.getExtension("OES_texture_half_float_linear");
  gl.getExtension("OES_texture_float_linear");
  const renderer = `${gl.getParameter(gl.VERSION)} ${format.half ? "half" : "float"} targets`;

  const scene = async (width, height, photo, rect) => {
    const c = document.createElement("canvas");
    c.width = width;
    c.height = height;
    const ctx = c.getContext("2d");
    ctx.drawImage(await image("/assets/call-backgrounds/office.jpg"), 0, 0, width, height);
    ctx.drawImage(await image(`/assets/call-faces/${photo}.jpg`), rect[0], rect[1], rect[2], rect[3]);
    return ctx.getImageData(0, 0, width, height).data;
  };

  const readTexture = (texture, width, height, type) => {
    const fb = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    const pixels = type === gl.FLOAT ? new Float32Array(width * height * 4) : new Uint8Array(width * height * 4);
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    gl.readPixels(0, 0, width, height, gl.RGBA, type, pixels);
    const error = gl.getError();
    if (status !== gl.FRAMEBUFFER_COMPLETE || error) throw new Error(`readPixels failed: status ${status}, error ${error}`);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(fb);
    return pixels;
  };

  const nhwc = (read) => {
    const [h, w, c] = read.shape;
    const slices = Math.ceil(c / 4);
    const out = new Float32Array(h * w * c);
    for (let y = 0; y < h; y += 1) {
      for (let x = 0; x < w; x += 1) {
        for (let k = 0; k < c; k += 1) {
          const s = k >> 2;
          out[(y * w + x) * c + k] = read.pixels[(y * read.width + s * w + x) * 4 + (k & 3)];
        }
      }
    }
    return out;
  };

  const uploadInput = (net, shape, bytes) => {
    const [h, w, c] = shape;
    const slices = Math.ceil(c / 4);
    const data = new Uint16Array(w * slices * h * 4);
    for (let y = 0; y < h; y += 1) {
      for (let x = 0; x < w; x += 1) {
        for (let k = 0; k < c; k += 1) {
          data[(y * w * slices + (k >> 2) * w + x) * 4 + (k & 3)] = halfOf(bytes[(y * w + x) * c + k] / 255);
        }
      }
    }
    gl.bindTexture(gl.TEXTURE_2D, net.input.texture);
    if (format.half) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w * slices, h, 0, gl.RGBA, half.HALF_FLOAT_OES, data);
    } else {
      const floats = new Float32Array(data.length);
      for (let i = 0; i < data.length; i += 1) floats[i] = seg.halfToFloat(data[i]);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w * slices, h, 0, gl.RGBA, gl.FLOAT, floats);
    }
  };

  const summary = (values) => {
    let sum = 0;
    let abs = 0;
    for (const v of values) {
      sum += v;
      abs += Math.abs(v);
    }
    const samples = [];
    for (let k = 0; k < 8; k += 1) samples.push(values[Math.floor(((k * 2654435761) >>> 0) % values.length)]);
    return { count: values.length, sum, abs, samples };
  };

  const models = {};
  for (const [name, file] of [["hair", "hair-192.fmnn"], ["square", "selfie-square.fmnn"]]) {
    models[name] = seg.parseSegmentModel(await (await fetch(`/assets/call-segmentation/${file}`)).arrayBuffer());
  }

  const networks = {};
  const scenes = {
    hair: { photo: "maya", rect: [0, 0, 192, 192] },
    square: { photo: "kofi", rect: [48, 56, 160, 200] },
  };
  for (const [name, model] of Object.entries(models)) {
    const { layers, shapes } = seg.expandLayers(model);
    const shape = shapes[model.input];
    const [h, w, c] = shape;
    const pixels = await scene(w, h, scenes[name].photo, scenes[name].rect);
    const bytes = new Uint8Array(h * w * c);
    for (let i = 0; i < h * w; i += 1) for (let k = 0; k < Math.min(c, 3); k += 1) bytes[i * c + k] = pixels[i * 4 + k];
    const net = seg.createNetwork(gl, model, format);
    if (name === "hair") {
      uploadInput(net, shape, bytes);
      net.run();
      const first = nhwc(net.read());
      for (let i = 0; i < h * w; i += 1) {
        const p = 1 / (1 + Math.exp(Math.max(-30, Math.min(30, first[i * 2] - first[i * 2 + 1]))));
        bytes[i * c + 3] = Math.round(p * 255);
      }
    }
    uploadInput(net, shape, bytes);
    net.run();
    const output = nhwc(net.read());
    const traced = layers.map((layer) => ({ index: layer.out, op: layer.op, ...summary(nhwc(net.read(layer.out))) }));
    networks[name] = {
      layers: layers.length,
      input: { shape, bytes: toBase64(bytes) },
      output: { shape: shapes[model.output], half: halves(output) },
      trace: traced,
    };
    net.dispose();
  }

  const width = 256;
  const height = 192;
  const cameraPixels = await scene(width, height, "maya", [70, 6, 120, 120]);
  const cameraRgb = new Uint8Array(width * height * 3);
  for (let i = 0; i < width * height; i += 1) for (let k = 0; k < 3; k += 1) cameraRgb[i * 3 + k] = cameraPixels[i * 4 + k];
  const camera = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, camera);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  const opaque = new Uint8Array(cameraPixels.length);
  for (let i = 0; i < width * height; i += 1) {
    for (let k = 0; k < 3; k += 1) opaque[i * 4 + k] = cameraPixels[i * 4 + k];
    opaque[i * 4 + 3] = 255;
  }
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, opaque);

  const hair = seg.createHairMatte(gl, models.hair, { format });
  const hairSteps = [];
  for (const [region, run] of [[{ center: [130, 58], size: [150, 150] }, true], [{ center: [132, 60], size: [146, 146] }, true], [{ center: [132, 60], size: [146, 146] }, false]]) {
    const texture = hair.update(camera, width, height, region, { run });
    const read = readTexture(texture, width, height, gl.UNSIGNED_BYTE);
    const state = new Uint8Array(width * height);
    for (let i = 0; i < width * height; i += 1) state[i] = read[i * 4];
    hairSteps.push({ region, run, width, height, state: toBase64(state) });
  }
  hair.dispose();

  const maskWidth = 128;
  const maskHeight = 96;
  const silhouette = (dx, dy) => {
    const bytes = new Uint8Array(maskWidth * maskHeight);
    for (let y = 0; y < maskHeight; y += 1) {
      for (let x = 0; x < maskWidth; x += 1) {
        const hx = (x - 65 - dx) / 26;
        const hy = (y - 32 - dy) / 31;
        const head = 1 - Math.sqrt(hx * hx + hy * hy);
        const sx = (x - 64 - dx) / 60;
        const sy = (y - 100 - dy) / 34;
        const body = 1 - Math.sqrt(sx * sx + sy * sy);
        const speck = (x * 37 + y * 101) % 97 === 0 ? 0.6 : 0;
        const v = Math.max(0, Math.min(1, Math.max(head, body) * 6 + 0.5, 1)) + speck;
        bytes[y * maskWidth + x] = Math.round(Math.max(0, Math.min(1, v)) * 255);
      }
    }
    return bytes;
  };
  const matte = seg.createMatte(gl, { square: models.square }, { format });
  const matteSteps = [];
  for (const [dx, dy, face] of [[0, 0, [0.5, 0.33, 0.28]], [2, 1, [0.51, 0.34, 0.28]]]) {
    const mask = silhouette(dx, dy);
    matte.setHostMask(mask, maskWidth, maskHeight);
    if (!matte.update(camera, width, height, { face })) throw new Error("matte did not update");
    const clean = readTexture(matte.mask, maskWidth, maskHeight, gl.FLOAT);
    const cleanR = new Float32Array(maskWidth * maskHeight);
    for (let i = 0; i < cleanR.length; i += 1) cleanR[i] = clean[i * 4];
    const coefficients = readTexture(matte.coefficients, width, height, gl.FLOAT);
    const step = 4;
    const grid = [];
    for (let y = 0; y < height; y += step) for (let x = 0; x < width; x += step) for (let k = 0; k < 3; k += 1) grid.push(coefficients[(y * width + x) * 4 + k]);
    matteSteps.push({
      mask: { width: maskWidth, height: maskHeight, bytes: toBase64(mask) },
      face,
      ready: matte.ready,
      clean: { width: maskWidth, height: maskHeight, half: halves(cleanR) },
      coefficients: { width, height, step, half: halves(grid) },
    });
  }
  matte.dispose();

  return {
    renderer,
    networks,
    camera: { width, height, rgb: toBase64(cameraRgb) },
    hairMatte: hairSteps,
    matte: matteSteps,
  };
}

main().catch((error) => {
  console.error(error);
  process.exit(1);
});
