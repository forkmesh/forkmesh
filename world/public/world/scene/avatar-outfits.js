import { OUTFIT_COLOR_OPTIONS, OUTFIT_STYLE_OPTIONS } from "../world-data.js";
import { emojiPixels } from "./avatar-shared.js";
import { canvasArtTexture, roundedRect, softwareCanvas } from "./canvas.js";

const OUTFIT_COLOR_HEX = Object.fromEntries(
  OUTFIT_COLOR_OPTIONS.map((option) => [option.id, option.color]),
);
export const OUTFIT_STYLE_IDS = OUTFIT_STYLE_OPTIONS.map((option) => option.id);

// Leading colours of the drawn flag glyph, strongest first, bucketed the same
// way as dominantSampleColor. Buckets under 6% of the opaque pixels are noise
// (antialiasing, thin emblems) and are dropped. Empty when the canvas is
// tainted or the glyph did not render as a colour flag.
const flagShirtPalettes = new Map();
let flagShirtSampler = null;

function cachedFlagShirtPalette(flag) {
  const cached = flagShirtPalettes.get(flag);
  if (cached) return cached;
  if (!flagShirtSampler) {
    const canvas = document.createElement("canvas");
    canvas.width = 256;
    canvas.height = 256;
    const context = canvas.getContext("2d", { willReadFrequently: true });
    flagShirtSampler = context ? { canvas, context } : null;
  }
  if (!flagShirtSampler) return [];
  const { canvas, context } = flagShirtSampler;
  context.clearRect(0, 0, 256, 256);
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.font = '224px system-ui, "Apple Color Emoji", "Segoe UI Emoji"';
  context.fillText(flag, 128, 128);
  const palette = flagShirtPalette(context, canvas);
  if (flagShirtPalettes.size >= 512) flagShirtPalettes.clear();
  flagShirtPalettes.set(flag, palette);
  return palette;
}

function flagShirtPalette(context, canvas) {
  const pixels = emojiPixels(context, canvas);
  if (!pixels) return [];
  const buckets = new Map();
  let total = 0;
  for (let i = 0; i < pixels.length; i += 4) {
    if (pixels[i + 3] < 224) continue;
    total += 1;
    const key =
      ((pixels[i] >> 4) << 8) |
      ((pixels[i + 1] >> 4) << 4) |
      (pixels[i + 2] >> 4);
    const bucket = buckets.get(key);
    if (bucket) {
      bucket.r += pixels[i];
      bucket.g += pixels[i + 1];
      bucket.b += pixels[i + 2];
      bucket.count += 1;
    } else {
      buckets.set(key, {
        r: pixels[i],
        g: pixels[i + 1],
        b: pixels[i + 2],
        count: 1,
      });
    }
  }
  if (!total) return [];
  const channel = (sum, count) =>
    Math.max(0, Math.min(255, Math.round(sum / count)))
      .toString(16)
      .padStart(2, "0");
  return [...buckets.values()]
    .filter((bucket) => bucket.count / total >= 0.06)
    .sort((a, b) => b.count - a.count)
    .slice(0, 3)
    .map(
      (bucket) =>
        `#${channel(bucket.r, bucket.count)}${channel(
          bucket.g,
          bucket.count,
        )}${channel(bucket.b, bucket.count)}`,
    );
}

// ---------------------------------------------------------------------------
// The procedural outfit tailor.
//
// Mirrors the Qt client's procedural avatar faces: the visitor's public name
// is folded through FNV-1a into a splitmix32 stream, and that one seed picks
// an entire tailored kit — cut, colourway, collar, hem, fastenings, pocket,
// seams and a stitched monogram — so the same coder wears the same outfit on
// every device while the huge feature space keeps any two names dressed
// differently. Supporting members may pin a specific cut and colourway
// (identity.outfitStyle / identity.outfitColor); everyone else wears what
// their name tailors, dyed with their flag's palette when a country is shared.
// ---------------------------------------------------------------------------

export function outfitRandom(seedText) {
  // FNV-1a fold of the seed, then splitmix32 streams unlimited draws out of
  // it — the same seeding recipe the Qt client's forkMeshAvatarPng uses.
  let state = 0x811c9dc5;
  for (const char of String(seedText || "")) {
    state = Math.imul(state ^ char.charCodeAt(0), 0x01000193) >>> 0;
  }
  const next = () => {
    state = (state + 0x9e3779b9) >>> 0;
    let word = state;
    word = Math.imul(word ^ (word >>> 16), 0x21f0aaad);
    word = Math.imul(word ^ (word >>> 15), 0x735a2d97);
    return ((word ^ (word >>> 15)) >>> 0) / 4294967296;
  };
  return {
    next,
    int: (n) => (n > 0 ? Math.floor(next() * n) % n : 0),
    chance: (pct) => next() * 100 < pct,
    range: (low, high) => low + next() * (high - low),
  };
}

function mixHex(hex, target, amount) {
  const parse = (value) => {
    const raw = String(value).replace("#", "");
    const packed = parseInt(
      raw.length === 3 ? raw.replace(/./g, "$&$&") : raw,
      16,
    );
    return [(packed >> 16) & 255, (packed >> 8) & 255, packed & 255];
  };
  const from = parse(hex);
  const to = parse(target);
  const channel = (index) =>
    Math.round(from[index] + (to[index] - from[index]) * amount)
      .toString(16)
      .padStart(2, "0");
  return `#${channel(0)}${channel(1)}${channel(2)}`;
}
const shadeHex = (hex, amount) => mixHex(hex, "#06110e", amount);
const tintHex = (hex, amount) => mixHex(hex, "#f7fbf8", amount);

// Curated colourways a name can draw when no flag palette dresses the kit:
// [body, accent, trim].
export const OUTFIT_COLORWAYS = [
  ["#174f3d", "#9ef7c6", "#f7fbf8"],
  ["#183f62", "#77d9ff", "#f2f8ff"],
  ["#633e23", "#f7c96b", "#fff4dc"],
  ["#573965", "#d5b6ff", "#f6eeff"],
  ["#6a3346", "#ff9eb7", "#ffeef3"],
  ["#0f3a3a", "#3fd8c2", "#e8fffb"],
  ["#40320f", "#e0b23e", "#f7ecc8"],
  ["#2a2f5e", "#8b9dff", "#eef1ff"],
  ["#4a1f1f", "#ff8a5c", "#ffe9dc"],
  ["#123b1f", "#7ce07c", "#eaffea"],
  ["#3b1f4a", "#e07ce0", "#fbe9ff"],
  ["#20343c", "#9ec9f7", "#eff7ff"],
  ["#5e2a10", "#ffd166", "#fff1cf"],
  ["#101d3a", "#f7e96b", "#fffbe0"],
];

// One painter per outfit cut. Each draws over a canvas already filled with
// kit.base; ids stay in lockstep with OUTFIT_STYLE_OPTIONS in world-data.js.
const OUTFIT_CUTS = {
  sash(context, rng, kit) {
    // The classic kit: an accent sash with trim piping over a faint weave.
    context.save();
    context.globalAlpha = 0.16;
    context.fillStyle = kit.deep;
    for (let y = 0; y < 256; y += 14) context.fillRect(0, y, 256, 5);
    context.restore();
    context.save();
    context.translate(128, 128);
    context.rotate(rng.chance(50) ? -Math.PI / 8 : Math.PI / 8);
    const width = rng.range(64, 96);
    context.fillStyle = kit.accent;
    context.fillRect(-256, -width / 2, 512, width);
    context.fillStyle = kit.trim;
    context.fillRect(-256, -width / 2 - 24, 512, 12);
    context.fillRect(-256, width / 2 + 12, 512, 12);
    if (rng.chance(45)) {
      context.fillStyle = kit.deep;
      context.fillRect(-256, -6, 512, 12);
    }
    context.restore();
  },
  racer(context, rng, kit) {
    const x = rng.range(84, 130);
    const width = rng.range(22, 34);
    const gap = rng.range(10, 18);
    context.fillStyle = kit.accent;
    context.fillRect(x, 0, width, 256);
    context.fillRect(x + width + gap, 0, width * 0.55, 256);
    context.fillStyle = kit.trim;
    context.fillRect(x - 6, 0, 4, 256);
    context.fillRect(x + width + gap + width * 0.55 + 2, 0, 4, 256);
    if (rng.chance(60)) {
      // A pit-lane checker band across one shoulder line.
      const bandY = rng.range(30, 60);
      for (let i = 0; i < 16; i += 1) {
        context.fillStyle = i % 2 ? kit.trim : kit.deep;
        context.fillRect(i * 16, bandY, 16, 12);
      }
    }
  },
  chevron(context, rng, kit) {
    const count = 3 + rng.int(3);
    const step = rng.range(34, 46);
    const drop = rng.range(50, 90);
    for (let i = 0; i < count; i += 1) {
      context.fillStyle = i % 2 ? kit.trim : kit.accent;
      context.beginPath();
      const top = 30 + i * step;
      context.moveTo(-8, top);
      context.lineTo(128, top + drop);
      context.lineTo(264, top);
      context.lineTo(264, top + 18);
      context.lineTo(128, top + drop + 18);
      context.lineTo(-8, top + 18);
      context.closePath();
      context.fill();
    }
  },
  argyle(context, rng, kit) {
    const size = rng.range(40, 56);
    context.save();
    context.translate(128, 128);
    context.rotate(Math.PI / 4);
    for (let x = -220; x < 220; x += size) {
      for (let y = -220; y < 220; y += size) {
        const even = (Math.round(x / size) + Math.round(y / size)) % 2 === 0;
        context.globalAlpha = even ? 0.85 : 0.6;
        context.fillStyle = even ? kit.accent : shadeHex(kit.base, 0.28);
        context.fillRect(x + 2, y + 2, size - 4, size - 4);
      }
    }
    context.globalAlpha = 1;
    context.strokeStyle = kit.trim;
    context.lineWidth = 2;
    context.setLineDash([6, 6]);
    for (let d = -220; d < 220; d += size) {
      context.beginPath();
      context.moveTo(d, -220);
      context.lineTo(d, 220);
      context.stroke();
      context.beginPath();
      context.moveTo(-220, d);
      context.lineTo(220, d);
      context.stroke();
    }
    context.setLineDash([]);
    context.restore();
  },
  circuit(context, rng, kit) {
    context.fillStyle = shadeHex(kit.base, 0.35);
    context.fillRect(0, 0, 256, 256);
    context.strokeStyle = kit.accent;
    context.lineWidth = 3;
    const pads = [];
    for (let trace = 0; trace < 9; trace += 1) {
      let x = rng.range(16, 240);
      let y = rng.range(16, 240);
      context.beginPath();
      context.moveTo(x, y);
      const turns = 2 + rng.int(3);
      for (let turn = 0; turn < turns; turn += 1) {
        // Manhattan routing: each turn moves along one axis only.
        if (rng.chance(50)) x = rng.range(16, 240);
        else y = rng.range(16, 240);
        context.lineTo(x, y);
      }
      context.stroke();
      pads.push([x, y]);
    }
    pads.forEach(([x, y]) => {
      context.fillStyle = kit.trim;
      context.beginPath();
      context.arc(x, y, 5, 0, Math.PI * 2);
      context.fill();
      context.fillStyle = shadeHex(kit.base, 0.5);
      context.beginPath();
      context.arc(x, y, 2, 0, Math.PI * 2);
      context.fill();
    });
    const chipX = rng.range(60, 160);
    const chipY = rng.range(60, 160);
    context.fillStyle = kit.deep;
    context.fillRect(chipX, chipY, 44, 30);
    context.fillStyle = kit.trim;
    for (let leg = 0; leg < 4; leg += 1) {
      context.fillRect(chipX + 6 + leg * 9, chipY - 6, 4, 6);
      context.fillRect(chipX + 6 + leg * 9, chipY + 30, 4, 6);
    }
  },
  pixel(context, rng, kit) {
    // A mirrored identicon mosaic — the same trick the fallback avatar
    // services use, worn as knitwear.
    const cells = 6;
    const size = 256 / cells;
    const palette = [kit.accent, kit.trim, shadeHex(kit.base, 0.3), kit.light];
    for (let x = 0; x < cells / 2; x += 1) {
      for (let y = 0; y < cells; y += 1) {
        if (!rng.chance(52)) continue;
        context.fillStyle = palette[rng.int(palette.length)];
        context.fillRect(x * size + 1, y * size + 1, size - 2, size - 2);
        context.fillRect(
          (cells - 1 - x) * size + 1,
          y * size + 1,
          size - 2,
          size - 2,
        );
      }
    }
  },
  waves(context, rng, kit) {
    const bands = 3 + rng.int(3);
    for (let band = 0; band < bands; band += 1) {
      const baseY = 40 + band * (190 / bands);
      const amplitude = rng.range(10, 26);
      const wavelength = rng.range(60, 120);
      const phase = rng.range(0, Math.PI * 2);
      context.globalAlpha = band % 2 ? 0.9 : 0.55;
      context.fillStyle = band % 2 ? kit.accent : kit.trim;
      context.beginPath();
      context.moveTo(0, 256);
      for (let x = 0; x <= 256; x += 4) {
        context.lineTo(
          x,
          baseY + Math.sin(phase + (x / wavelength) * Math.PI * 2) * amplitude,
        );
      }
      context.lineTo(256, 256);
      context.closePath();
      context.fill();
    }
    context.globalAlpha = 1;
  },
  starfield(context, rng, kit) {
    context.fillStyle = shadeHex(kit.base, 0.55);
    context.fillRect(0, 0, 256, 256);
    for (let star = 0; star < 70; star += 1) {
      const x = rng.range(0, 256);
      const y = rng.range(0, 256);
      const radius = rng.range(0.6, 2.2);
      context.globalAlpha = rng.range(0.35, 1);
      context.fillStyle = rng.chance(20) ? kit.accent : kit.trim;
      context.beginPath();
      context.arc(x, y, radius, 0, Math.PI * 2);
      context.fill();
    }
    context.globalAlpha = 1;
    const planetX = rng.range(60, 196);
    const planetY = rng.range(120, 210);
    const planetR = rng.range(16, 26);
    context.fillStyle = kit.accent;
    context.beginPath();
    context.arc(planetX, planetY, planetR, 0, Math.PI * 2);
    context.fill();
    context.strokeStyle = kit.trim;
    context.lineWidth = 3;
    context.save();
    context.translate(planetX, planetY);
    context.rotate(-0.5);
    context.beginPath();
    context.ellipse(0, 0, planetR * 1.7, planetR * 0.5, 0, 0, Math.PI * 2);
    context.stroke();
    context.restore();
  },
  hex(context, rng, kit) {
    const radius = rng.range(16, 24);
    const height = radius * Math.sqrt(3);
    context.lineWidth = 2.5;
    for (let row = -1; row * height * 0.5 < 290; row += 1) {
      for (let col = -1; col * radius * 3 < 290; col += 1) {
        const x = col * radius * 3 + (row % 2 ? radius * 1.5 : 0);
        const y = row * height * 0.5;
        context.beginPath();
        for (let corner = 0; corner < 6; corner += 1) {
          const angle = (Math.PI / 3) * corner;
          const px = x + Math.cos(angle) * radius;
          const py = y + Math.sin(angle) * radius;
          if (corner === 0) context.moveTo(px, py);
          else context.lineTo(px, py);
        }
        context.closePath();
        if (rng.chance(18)) {
          context.globalAlpha = 0.8;
          context.fillStyle = kit.accent;
          context.fill();
        }
        context.globalAlpha = 0.55;
        context.strokeStyle = kit.trim;
        context.stroke();
        context.globalAlpha = 1;
      }
    }
  },
  bolt(context, rng, kit) {
    // One big embroidered hotfix bolt with a glow halo.
    const cx = rng.range(96, 160);
    const lean = rng.range(-18, 18);
    const points = [
      [cx + 26 + lean, 18],
      [cx - 30 + lean * 0.5, 118],
      [cx - 2, 118],
      [cx - 26, 238],
      [cx + 34, 108],
      [cx + 2, 108],
    ];
    context.save();
    context.shadowColor = kit.accent;
    context.shadowBlur = 22;
    context.fillStyle = kit.accent;
    context.beginPath();
    points.forEach(([x, y], index) =>
      index ? context.lineTo(x, y) : context.moveTo(x, y),
    );
    context.closePath();
    context.fill();
    context.restore();
    context.strokeStyle = kit.trim;
    context.lineWidth = 3;
    context.stroke();
  },
  tartan(context, rng, kit) {
    const step = rng.range(44, 64);
    const bandWidth = rng.range(14, 24);
    const xOffset = rng.range(0, step);
    const yOffset = rng.range(0, step);
    context.globalAlpha = 0.85;
    context.fillStyle = kit.accent;
    for (let x = xOffset; x < 256; x += step) {
      context.fillRect(x, 0, bandWidth, 256);
    }
    context.globalAlpha = 0.55;
    context.fillStyle = kit.deep;
    for (let y = yOffset; y < 256; y += step) {
      context.fillRect(0, y, 256, bandWidth);
    }
    context.globalAlpha = 1;
    context.strokeStyle = kit.trim;
    context.lineWidth = 2;
    context.setLineDash([4, 4]);
    for (let x = xOffset; x < 256; x += step) {
      context.beginPath();
      context.moveTo(x + bandWidth + 6, 0);
      context.lineTo(x + bandWidth + 6, 256);
      context.stroke();
    }
    context.setLineDash([]);
  },
  binary(context, rng, kit) {
    context.fillStyle = shadeHex(kit.base, 0.4);
    context.fillRect(0, 0, 256, 256);
    context.font = '700 18px "ForkMesh Mono", ui-monospace, monospace';
    context.textAlign = "center";
    context.textBaseline = "middle";
    const columns = 9;
    for (let col = 0; col < columns; col += 1) {
      const x = 16 + col * (224 / (columns - 1));
      const drop = rng.range(0, 20);
      const bright = rng.int(12);
      for (let row = 0; row < 12; row += 1) {
        context.globalAlpha = row === bright ? 1 : rng.range(0.16, 0.5);
        context.fillStyle = row === bright ? kit.trim : kit.accent;
        context.fillText(rng.chance(50) ? "1" : "0", x, drop + 10 + row * 21);
      }
    }
    context.globalAlpha = 1;
  },
};

// Tailoring details layered over every cut. Each is its own seeded roll, so
// two coders who happen to share a cut still differ in collar, hem,
// fastenings, pocket, seams, and monogram.
function tailorOutfitDetails(context, rng, kit, monogram) {
  if (rng.chance(70)) {
    // Ribbed collar band along the shoulder line.
    context.fillStyle = rng.chance(50) ? kit.trim : kit.deep;
    context.fillRect(0, 0, 256, 16);
    context.fillStyle = kit.accent;
    context.fillRect(0, 16, 256, 3);
  }
  if (rng.chance(55)) {
    context.fillStyle = rng.chance(50) ? kit.deep : kit.accent;
    context.fillRect(0, 244, 256, 12);
  }
  const fastening = rng.int(3); // 0 plain · 1 zip · 2 buttons
  if (fastening === 1) {
    context.strokeStyle = kit.trim;
    context.lineWidth = 3;
    context.beginPath();
    context.moveTo(128, 20);
    context.lineTo(128, 236);
    context.stroke();
    context.fillStyle = kit.trim;
    context.fillRect(124, 26, 8, 12);
  } else if (fastening === 2) {
    context.fillStyle = kit.trim;
    for (let button = 0; button < 4; button += 1) {
      context.beginPath();
      context.arc(128, 48 + button * 48, 4.5, 0, Math.PI * 2);
      context.fill();
    }
  }
  if (rng.chance(45)) {
    // A chest pocket with a trim flap.
    const x = rng.chance(50) ? 52 : 168;
    context.fillStyle = shadeHex(kit.base, 0.22);
    context.fillRect(x, 140, 40, 34);
    context.fillStyle = kit.trim;
    context.fillRect(x, 140, 40, 7);
  }
  if (rng.chance(50)) {
    // Dashed side seams.
    context.strokeStyle = kit.trim;
    context.globalAlpha = 0.7;
    context.lineWidth = 2;
    context.setLineDash([5, 7]);
    [10, 246].forEach((x) => {
      context.beginPath();
      context.moveTo(x, 22);
      context.lineTo(x, 240);
      context.stroke();
    });
    context.setLineDash([]);
    context.globalAlpha = 1;
  }
  if (monogram) {
    // The stitched monogram patch — the first letter of the public name, the
    // same letter the Qt fallback avatar tile shows.
    context.fillStyle = kit.deep;
    roundedRect(context, 22, 30, 34, 34, 8);
    context.fill();
    context.strokeStyle = kit.trim;
    context.lineWidth = 2;
    context.setLineDash([3, 3]);
    context.stroke();
    context.setLineDash([]);
    context.fillStyle = kit.light;
    context.font = '700 22px "ForkMesh Favorit", system-ui, sans-serif';
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillText(monogram, 39, 48);
  }
}

// The one flag glyph the avatar wears is the chest badge's; the shirt never
// prints it. When a country is shared its sampled palette dyes the kit, and
// the visitor's public name tailors everything else — cut, trims, monogram —
// falling back entirely to the name-seeded colourway when the flag cannot be
// drawn or sampled.
export function countryShirtTexture(THREE, identity) {
  const code = /^[A-Z]{2}$/.test(String(identity.countryCode || ""))
    ? String(identity.countryCode)
    : "";
  const flag = code && identity.flag && identity.flag !== "◌"
    ? identity.flag
    : "◌";
  const wornCut = OUTFIT_STYLE_IDS.includes(String(identity.outfitStyle || ""))
    ? String(identity.outfitStyle)
    : "";
  const wornColor = OUTFIT_COLOR_HEX[identity.outfitColor] || "";
  return softwareCanvas(() => canvasArtTexture(THREE, 256, 256, (context, canvas) => {
    const rng = outfitRandom(
      "outfit:" + String(identity.name || "guest").trim().toLowerCase(),
    );
    // Fixed draw order keeps the stream stable: the seeded cut and colourway
    // are always consumed, even when a Supporting member's pick replaces them.
    const seededCut = OUTFIT_STYLE_IDS[rng.int(OUTFIT_STYLE_IDS.length)];
    let [base, accent, trim] =
      OUTFIT_COLORWAYS[rng.int(OUTFIT_COLORWAYS.length)];
    if (flag !== "◌") {
      // Drawn big only to be sampled, then painted over entirely: nothing of
      // the glyph itself survives onto the cloth.
      const sampled = cachedFlagShirtPalette(flag);
      if (sampled.length >= 2) {
        [base, accent] = sampled;
        trim = sampled[2] || "#f7fbf8";
      }
    }
    if (wornColor) {
      // The Supporting-member colourway perk re-dyes the whole kit.
      base = wornColor;
      accent = tintHex(wornColor, 0.55);
      trim = shadeHex(wornColor, 0.45);
    }
    const kit = {
      base,
      accent,
      trim,
      deep: shadeHex(base, 0.42),
      light: tintHex(base, 0.6),
    };
    context.clearRect(0, 0, 256, 256);
    context.textAlign = "left";
    context.textBaseline = "alphabetic";
    context.fillStyle = kit.base;
    context.fillRect(0, 0, 256, 256);
    const cut = OUTFIT_CUTS[wornCut || seededCut] || OUTFIT_CUTS.sash;
    cut(context, rng, kit);
    const name = String(identity.name || "").trim();
    tailorOutfitDetails(
      context,
      rng,
      kit,
      /^[A-Za-z0-9]/.test(name) ? name[0].toUpperCase() : "",
    );
  }));
}
