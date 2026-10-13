// Side-effect import: ForkMesh's own QR generator publishes globalThis.ForkMeshQR,
// used for the reward-pool treasury address board.
// Wallet QR codes are optional scene decoration. Start loading the encoder as
// soon as this module evaluates, but keep its sizeable implementation outside
// the initial World module graph. Scene construction no longer waits for it;
// the shell applies wallet badges after the encoder settles.
export const QR_MODULE_READY = import("../../qr.js").catch(() => null);

// Phones do not die of slow frames; they die of memory. Painted at their
// desktop resolution the World's ~96 plates allocate around 200 MB of 2D
// backing stores (plus the same again once uploaded as RGBA textures) before
// the first frame, which is more than a mobile browser lets one tab hold: the
// tab is killed seconds into the boot and reloads into the crash guard. The
// compact renderer therefore paints its large plates at quarter resolution —
// one sixteenth of the desktop bytes. A merely oversized desktop surface uses
// half resolution. The draw code keeps its original coordinate system through
// a pre-scaled context, so only the backing store shrinks.
const CANVAS_TEXTURE_SCALE_MIN_PIXELS = 512 * 512;
export let canvasTextureScale = 1;
export function setCanvasTextureScale(value) {
  canvasTextureScale = value;
}

function canvasTextureScaleFor(width, height, draw, plateScale = 1) {
  // A painter that takes the canvas reads canvas.width/height for its own
  // layout, which a scaled context would silently halve underneath it. Those
  // keep full resolution; together they are a rounding error of the total.
  if (draw.length >= 2) return 1;
  // A caller-supplied plate scale is a decision about one plate rather than
  // about the renderer — a hundred chest cards read from across a clearing do
  // not each need a megabyte — so it applies at any size and multiplies with
  // the renderer-wide reduction below.
  const plate = plateScale > 0 && plateScale < 1 ? plateScale : 1;
  if (canvasTextureScale >= 1) return plate;
  return width * height >= CANVAS_TEXTURE_SCALE_MIN_PIXELS
    ? canvasTextureScale * plate
    : plate;
}

const WORLD_PANEL_TEXT_FONT =
  '"ForkMesh Sans", system-ui, -apple-system, "Segoe UI", sans-serif';
const worldPanelColorCache = new Map();
let worldPanelProbe = null;
let worldPanelArtDepth = 0;
let worldPanelSoftwareDepth = 0;
const SOFTWARE_CANVAS_OPTIONS = Object.freeze({ willReadFrequently: true });
export let deferredCanvasPaints = null;
export function setDeferredCanvasPaints(value) {
  deferredCanvasPaints = value;
}
const pendingCanvasPaints = new WeakMap();
const DEFERRED_CANVAS_SLICE_MS = 8;

export function paintCanvasJob(job) {
  if (!job || pendingCanvasPaints.get(job.canvas) !== job) return false;
  pendingCanvasPaints.delete(job.canvas);
  try {
    job.draw(job.art ? job.context : worldPanelContext(job.context), job.canvas);
  } catch (_) {}
  job.texture.needsUpdate = true;
  return true;
}

export function paintDeferredCanvases(
  jobs,
  isDisposed = () => false,
  upload = null,
  canUpload = () => true,
) {
  const queue = jobs.slice();
  const schedule = (step) =>
    typeof window.requestIdleCallback === "function"
      ? window.requestIdleCallback(step, { timeout: 400 })
      : window.setTimeout(step, 32);
  const step = () => {
    if (isDisposed()) return;
    const started = performance.now();
    while (queue.length && performance.now() - started < DEFERRED_CANVAS_SLICE_MS) {
      if (upload && !canUpload(queue[0].texture)) break;
      const job = queue.shift();
      if (paintCanvasJob(job) && upload) {
        try {
          upload(job.texture);
        } catch (_) {}
      }
    }
    if (queue.length) schedule(step);
  };
  if (queue.length) schedule(step);
}

function worldPanelRgba(value) {
  worldPanelProbe ||= document.createElement("canvas").getContext("2d");
  if (!worldPanelProbe) return null;
  worldPanelProbe.fillStyle = "#010203";
  worldPanelProbe.fillStyle = value;
  const normalized = String(worldPanelProbe.fillStyle);
  if (normalized === "#010203" && String(value).trim().toLowerCase() !== "#010203") {
    return null;
  }
  if (normalized.startsWith("#")) {
    return [
      parseInt(normalized.slice(1, 3), 16),
      parseInt(normalized.slice(3, 5), 16),
      parseInt(normalized.slice(5, 7), 16),
      1,
    ];
  }
  const parts = normalized.match(/[\d.]+/g)?.map(Number) || [];
  return parts.length >= 3 ? [parts[0], parts[1], parts[2], parts[3] ?? 1] : null;
}

function worldPanelHsl(red, green, blue) {
  const r = red / 255;
  const g = green / 255;
  const b = blue / 255;
  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  const light = (max + min) / 2;
  const chroma = max - min;
  let hue = 0;
  let sat = 0;
  if (chroma > 0) {
    sat = chroma / (1 - Math.abs(2 * light - 1));
    if (max === r) hue = ((g - b) / chroma) % 6;
    else if (max === g) hue = (b - r) / chroma + 2;
    else hue = (r - g) / chroma + 4;
    hue = (hue * 60 + 360) % 360;
  }
  return { hue, sat: Math.min(1, sat), light, chroma };
}

function worldPanelRgbString(hue, sat, light, alpha) {
  const c = (1 - Math.abs(2 * light - 1)) * sat;
  const x = c * (1 - Math.abs(((hue / 60) % 2) - 1));
  const m = light - c / 2;
  const [r, g, b] =
    hue < 60
      ? [c, x, 0]
      : hue < 120
        ? [x, c, 0]
        : hue < 180
          ? [0, c, x]
          : hue < 240
            ? [0, x, c]
            : hue < 300
              ? [x, 0, c]
              : [c, 0, x];
  const channel = (value) =>
    Math.round(Math.min(1, Math.max(0, value + m)) * 255);
  return `rgba(${channel(r)}, ${channel(g)}, ${channel(b)}, ${
    Math.round(alpha * 1000) / 1000
  })`;
}

function worldPanelColor(value, role) {
  if (typeof value !== "string") return value;
  const key = `${role}|${value}`;
  const cached = worldPanelColorCache.get(key);
  if (cached) return cached;
  const rgba = worldPanelRgba(value);
  let mapped = value;
  if (rgba && rgba[3] > 0) {
    const { hue, sat, light, chroma } = worldPanelHsl(rgba[0], rgba[1], rgba[2]);
    const accentHue = hue >= 140 && hue <= 185 ? 217 : hue;
    const warm = accentHue >= 35 && accentHue <= 70;
    let next;
    if (chroma < 0.2) {
      const neutral = Math.min(sat, 0.1);
      if (role === "text") {
        next = [hue, neutral, light >= 0.5 ? 0.1 + (1 - light) * 0.75 : 0.12];
      } else if (role === "stroke") {
        next = [hue, neutral, light >= 0.5 ? Math.max(0.3, 1 - light) : 1 - light * 0.55];
      } else {
        next = [hue, neutral, light >= 0.5 ? 0.9 + (light - 0.5) * 0.2 : 1 - light * 0.45];
      }
    } else if (role === "text") {
      next = [accentHue, Math.max(sat, 0.55), Math.min(Math.max(light, 0.28), warm ? 0.34 : 0.44)];
    } else if (role === "stroke") {
      next = [accentHue, Math.max(sat, 0.55), warm ? 0.45 : 0.56];
    } else if (light < 0.3) {
      next = [accentHue, Math.min(sat, 0.35), 1 - light * 0.35];
    } else {
      next = [accentHue, Math.min(Math.max(sat, 0.5), 0.85), warm ? 0.82 : 0.86];
    }
    mapped = worldPanelRgbString(next[0], next[1], next[2], rgba[3]);
  }
  if (worldPanelColorCache.size >= 4096) worldPanelColorCache.clear();
  worldPanelColorCache.set(key, mapped);
  return mapped;
}

function worldPanelFont(value) {
  const font = String(value || "");
  if (!/ForkMesh (?:Mono|Favorit)/.test(font)) return font;
  const match = font.match(
    /^\s*(?:(italic|oblique)\s+)?(?:(\d{3}|bold|normal)\s+)?((?:\d*\.)?\d+px(?:\/\S+)?)\s/,
  );
  if (!match) return font;
  const requested =
    match[2] === "bold" ? 700 : match[2] === "normal" ? 400 : Number(match[2]) || 400;
  const weight = requested >= 700 ? 700 : requested >= 550 ? 600 : requested;
  return `${match[1] ? `${match[1]} ` : ""}${weight} ${match[3]} ${WORLD_PANEL_TEXT_FONT}`;
}

function worldPanelGradient(gradient) {
  const addColorStop = gradient.addColorStop.bind(gradient);
  gradient.addColorStop = (offset, color) =>
    addColorStop(offset, worldPanelColor(color, "fill"));
  return gradient;
}

function worldPanelContext(context) {
  const state = {
    fill: context.fillStyle,
    stroke: context.strokeStyle,
    appliedFill: undefined,
    appliedStroke: undefined,
    stack: [],
  };
  const methods = new Map();
  const paint = (target, role, method, args) => {
    if (role === "stroke" || method === "strokeText") {
      const color = worldPanelColor(state.stroke, role);
      if (color !== state.appliedStroke) {
        target.strokeStyle = color;
        state.appliedStroke = color;
      }
    } else {
      const color = worldPanelColor(state.fill, role);
      if (color !== state.appliedFill) {
        target.fillStyle = color;
        state.appliedFill = color;
      }
    }
    return target[method](...args);
  };
  const method = (target, property) => {
    switch (property) {
      case "save":
        return () => {
          state.stack.push([
            state.fill,
            state.stroke,
            state.appliedFill,
            state.appliedStroke,
          ]);
          target.save();
        };
      case "restore":
        return () => {
          const saved = state.stack.pop();
          if (saved) {
            [state.fill, state.stroke, state.appliedFill, state.appliedStroke] =
              saved;
          } else {
            state.appliedFill = undefined;
            state.appliedStroke = undefined;
          }
          target.restore();
        };
      case "fillText":
      case "strokeText":
        return (...args) => paint(target, "text", property, args);
      case "fill":
      case "fillRect":
        return (...args) => paint(target, "fill", property, args);
      case "stroke":
      case "strokeRect":
        return (...args) => paint(target, "stroke", property, args);
      case "createLinearGradient":
      case "createRadialGradient":
      case "createConicGradient":
        return (...args) => worldPanelGradient(target[property](...args));
      default:
        return null;
    }
  };
  return new Proxy(context, {
    get(target, property) {
      if (property === "fillStyle") return state.fill;
      if (property === "strokeStyle") return state.stroke;
      let cached = methods.get(property);
      if (cached !== undefined) return cached;
      cached = method(target, property);
      if (cached === null) {
        const value = Reflect.get(target, property, target);
        if (typeof value !== "function") return value;
        cached = value.bind(target);
      }
      methods.set(property, cached);
      return cached;
    },
    set(target, property, value) {
      if (property === "fillStyle") state.fill = value;
      else if (property === "strokeStyle") state.stroke = value;
      else if (property === "shadowColor") target.shadowColor = "rgba(0, 0, 0, 0)";
      else if (property === "font") target.font = worldPanelFont(value);
      else target[property] = value;
      return true;
    },
  });
}

export function softwareCanvas(build) {
  worldPanelSoftwareDepth += 1;
  try {
    return build();
  } finally {
    worldPanelSoftwareDepth -= 1;
  }
}

export function canvasArtTexture(...args) {
  worldPanelArtDepth += 1;
  try {
    return canvasTexture(...args);
  } finally {
    worldPanelArtDepth -= 1;
  }
}

export function canvasTexture(
  THREE,
  width,
  height,
  draw,
  plateScale = 1,
  // An exact backing-store scale, for a plate whose own legibility decides
  // its resolution rather than the renderer-wide memory reduction.
  scaleOverride = 0,
) {
  const canvas = document.createElement("canvas");
  const scale =
    scaleOverride > 0
      ? scaleOverride
      : canvasTextureScaleFor(width, height, draw, plateScale);
  canvas.width = Math.max(1, Math.round(width * scale));
  canvas.height = Math.max(1, Math.round(height * scale));
  const context = worldPanelSoftwareDepth > 0
    ? canvas.getContext("2d", SOFTWARE_CANVAS_OPTIONS)
    : canvas.getContext("2d");
  if (scale !== 1) {
    // Recorded so an in-place repaint can restore the same mapping.
    canvas.dataset.textureScale = String(scale);
    context.setTransform(scale, 0, 0, scale, 0, 0);
  }
  if (worldPanelArtDepth > 0) canvas.dataset.worldArt = "true";
  const texture = new THREE.CanvasTexture(canvas);
  if (Array.isArray(deferredCanvasPaints) && worldPanelArtDepth === 0) {
    const job = { canvas, context, draw, texture, art: worldPanelArtDepth > 0 };
    pendingCanvasPaints.set(canvas, job);
    deferredCanvasPaints.push(job);
  } else {
    draw(worldPanelArtDepth > 0 ? context : worldPanelContext(context), canvas);
  }
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.minFilter = THREE.LinearFilter;
  texture.magFilter = THREE.LinearFilter;
  return texture;
}

// Canvas text is usually read on a small plane at an oblique camera angle.
// Trilinear mip sampling keeps strokes stable as the avatar moves, while a
// bounded anisotropy level preserves horizontal detail without changing the
// backing-store sizes chosen by the mobile memory budget above.
export function sharpenCanvasTextTexture(THREE, texture) {
  texture.generateMipmaps = true;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.magFilter = THREE.LinearFilter;
  texture.anisotropy = 8;
  texture.needsUpdate = true;
  return texture;
}

// Redraws an existing CanvasTexture in place. Recurring plates (the
// per-second sync countdowns and per-minute staleness readouts) must never
// allocate a fresh canvas or swap material.map: doing that once per plate per
// second disposed and re-created a GL texture and re-validated the material
// each tick, which surfaced as a metronomic ~50ms render stall while the
// World idled. Reusing the canvas keeps a tick at one cheap 2D redraw plus a
// same-size upload of a small texture that was already resident.
export function repaintCanvasTexture(material, draw) {
  const texture = material?.map;
  const canvas = texture?.image;
  const context = canvas?.getContext?.("2d");
  if (!context) return false;
  pendingCanvasPaints.delete(canvas);
  // Large plates are painted through a scaled context on the compact
  // renderer. Restore that mapping (a painter that saved and restored the
  // context may have dropped it) so the repaint lands in the same coordinate
  // system the first paint used, and clear in those same logical units.
  const scale = Number(canvas.dataset?.textureScale) || 1;
  context.setTransform(scale, 0, 0, scale, 0, 0);
  context.clearRect(0, 0, canvas.width / scale, canvas.height / scale);
  draw(
    canvas.dataset?.worldArt === "true" ? context : worldPanelContext(context),
    canvas,
  );
  texture.needsUpdate = true;
  return true;
}

export function repaintCanvasRegion(material, x, y, width, height, draw) {
  const texture = material?.map;
  const canvas = texture?.image;
  const context = canvas?.getContext?.("2d");
  if (!context) return false;
  if (pendingCanvasPaints.has(canvas)) return true;
  const scale = Number(canvas.dataset?.textureScale) || 1;
  context.setTransform(scale, 0, 0, scale, 0, 0);
  context.save();
  context.beginPath();
  context.rect(x, y, width, height);
  context.clip();
  context.clearRect(x, y, width, height);
  draw(
    canvas.dataset?.worldArt === "true" ? context : worldPanelContext(context),
    canvas,
  );
  context.restore();
  texture.needsUpdate = true;
  return true;
}

export function roundedRect(context, x, y, width, height, radius) {
  const r = Math.min(radius, width / 2, height / 2);
  context.beginPath();
  context.moveTo(x + r, y);
  context.arcTo(x + width, y, x + width, y + height, r);
  context.arcTo(x + width, y + height, x, y + height, r);
  context.arcTo(x, y + height, x, y, r);
  context.arcTo(x, y, x + width, y, r);
  context.closePath();
}

export function wordTexture(THREE, title, subtitle, color = "#9ef7c6") {
  return canvasTexture(THREE, 768, 256, (context) => {
    context.clearRect(0, 0, 768, 256);
    roundedRect(context, 4, 4, 760, 248, 12);
    context.fillStyle = "rgba(6,17,14,0.92)";
    context.fill();
    context.strokeStyle = color;
    context.lineWidth = 4;
    context.stroke();
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 54px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText(String(title || "").slice(0, 22), 42, 99);
    context.fillStyle = color;
    context.font = '400 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(subtitle || "").toUpperCase().slice(0, 36), 42, 165);
  });
}
