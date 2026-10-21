// ForkMesh code map (adhoc #187).
//
// A full-screen, zoomable treemap of a whole repository with every directory
// expanded: each file is a cell sized by its bytes, and once a cell is large
// enough on screen its real source is drawn inside it (a minimap of line bars
// from far away, readable highlighted text up close). A toggle swaps the flat
// map for a rotatable 3D city built from the SAME layout, where every folder is
// a slab whose thickness follows its size and files are tiles on top. Images
// and fonts show their own picture instead of text, and every other binary a
// card naming its type.
//
// The module is standalone and loaded on demand by the repository page (it is
// not part of the dashboard bundle). It owns no network code: the caller hands
// it the host's /sizes tree plus `fetchBlobs` / `fetchRaw` / `fetchTree`
// callbacks, so reads keep the dashboard's mirror routing, ref pinning and
// batching. three.js is imported lazily, only when the 3D view is first opened.
(() => {
  "use strict";

  const SCRIPT_BASE = (() => {
    const src = (typeof document !== "undefined" && document.currentScript && document.currentScript.src) || "";
    return src ? src.slice(0, src.lastIndexOf("/") + 1) : "/assets/codemap/";
  })();
  const THREE_URL = `${SCRIPT_BASE}vendor/three-0.184.0/three.module.min.js`;

  // ---------------------------------------------------------------------
  // Themes
  // ---------------------------------------------------------------------
  //
  // Language hues are the validated eight-slot categorical order (blue, orange,
  // aqua, yellow, magenta, green, violet, red), assigned to a repository's
  // languages by bytes and never cycled: a ninth language is gray "Other". A
  // treemap puts every pair of hues side by side, which no eight-hue order can
  // keep separable for every reader, so identity never rides on color alone —
  // the legend, the folder/file labels and the hover readout all name it.
  const THEMES = {
    dark: {
      bg: "#010409", panel: "#0d1117", border: "#3d444d", fg: "#f0f6fc", muted: "#9198a1",
      tile: "#10141a", accent: "#58a6ff", gray: "#8a8a8a", pill: "rgba(1,4,9,0.82)",
      cats: ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#008300", "#9085e9", "#e66767"],
      code: ["#c9d1d9", "#ff7b72", "#a5d6ff", "#8b949e", "#79c0ff", "#d2a8ff", "#8b949e"],
      dirFill: (depth) => `hsl(215 22% ${5 + Math.min(depth, 8) * 2.2}%)`,
    },
    light: {
      bg: "#ffffff", panel: "#f6f8fa", border: "#d0d7de", fg: "#1f2328", muted: "#59636e",
      tile: "#ffffff", accent: "#0969da", gray: "#8a8a8a", pill: "rgba(255,255,255,0.88)",
      cats: ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948"],
      code: ["#24292f", "#cf222e", "#0a3069", "#6e7781", "#0550ae", "#8250df", "#6e7781"],
      dirFill: (depth) => `hsl(210 18% ${97 - Math.min(depth, 8) * 2}%)`,
    },
  };

  // ---------------------------------------------------------------------
  // Languages
  // ---------------------------------------------------------------------
  const LANGUAGE_BY_EXT = {
    js: "JavaScript", mjs: "JavaScript", cjs: "JavaScript", jsx: "JavaScript",
    ts: "TypeScript", tsx: "TypeScript", py: "Python", pyi: "Python",
    cpp: "C++", cc: "C++", cxx: "C++", hpp: "C++", hh: "C++", h: "C++", c: "C",
    go: "Go", rs: "Rust", dart: "Dart", kt: "Kotlin", kts: "Kotlin", java: "Java",
    swift: "Swift", rb: "Ruby", php: "PHP", cs: "C#", lua: "Lua", sql: "SQL",
    html: "HTML", htm: "HTML", css: "CSS", scss: "CSS", less: "CSS",
    md: "Markdown", markdown: "Markdown", rst: "Markdown", txt: "Text",
    json: "JSON", yml: "YAML", yaml: "YAML", toml: "TOML", xml: "XML", qrc: "XML", ui: "XML",
    sh: "Shell", bash: "Shell", zsh: "Shell", ps1: "Shell", bat: "Shell",
    cmake: "CMake", qss: "CSS", svg: "SVG", patch: "Patch", diff: "Patch",
    png: "Image", jpg: "Image", jpeg: "Image", gif: "Image", webp: "Image", ico: "Image", avif: "Image",
    woff: "Font", woff2: "Font", ttf: "Font", otf: "Font",
  };
  const BINARY_EXT = new Set([
    "png", "jpg", "jpeg", "gif", "webp", "ico", "avif", "bmp", "icns", "woff", "woff2", "ttf", "otf", "eot",
    "zip", "gz", "tgz", "xz", "bz2", "7z", "tar", "jar", "apk", "aab", "deb", "rpm", "dmg", "exe", "dll",
    "so", "dylib", "a", "o", "bin", "dat", "db", "sqlite", "pdf", "mp3", "mp4", "webm", "ogg", "wav", "mov",
    "glb", "gltf", "wasm", "pyc", "class", "keystore", "jks", "age", "pack", "idx",
  ]);
  const HASH_COMMENT = new Set(["Python", "Shell", "YAML", "TOML", "Ruby", "CMake"]);
  const DASH_COMMENT = new Set(["SQL", "Lua"]);
  const NO_SLASH_COMMENT = new Set(["Python", "Shell", "YAML", "TOML", "Ruby", "CMake", "Markdown", "Text", "HTML", "XML", "SVG", "JSON", "Patch", "SQL", "Lua"]);
  const PROSE = new Set(["Markdown", "Text", "Patch"]);
  const KEYWORDS = new Set((
    "abstract and as assert async auto await bool break case catch char class const constexpr continue def default defer " +
    "del delete do double dynamic elif else enum except explicit export extends extern false final finally float fn for " +
    "from func function global go if impl implements import in inline instanceof int interface is lambda let long match " +
    "mod mut namespace new nil none None not null nullptr of operator or override package pass private protected pub public " +
    "raise return self short signed sizeof static struct super switch template this throw true True False try type typedef " +
    "typename typeof union unsigned use using var virtual void volatile while with yield select chan range map string"
  ).split(" "));

  function languageOf(name) {
    const base = String(name || "").split("/").pop() || "";
    const lower = base.toLowerCase();
    if (lower === "cmakelists.txt") return "CMake";
    if (lower === "makefile" || lower === "dockerfile") return "Shell";
    const dot = lower.lastIndexOf(".");
    if (dot <= 0) return "Other";
    const ext = lower.slice(dot + 1);
    return LANGUAGE_BY_EXT[ext] || (ext.length <= 5 ? `.${ext}` : "Other");
  }

  function isBinaryName(name) {
    const lower = String(name || "").toLowerCase();
    const dot = lower.lastIndexOf(".");
    return dot > 0 && BINARY_EXT.has(lower.slice(dot + 1));
  }

  // ---------------------------------------------------------------------
  // Non-code previews
  // ---------------------------------------------------------------------
  //
  // Images (and fonts, as a type specimen) show their own picture in the cell,
  // videos a frame from their start; every other binary shows a card naming
  // its type. Kept in step with desktop/src/CodeMapModel.cpp, which has no
  // video decoder and cards videos.
  const IMAGE_PREVIEW_EXT = new Set(["png", "jpg", "jpeg", "gif", "webp", "avif", "bmp", "ico", "svg"]);
  const FONT_PREVIEW_EXT = new Set(["ttf", "otf", "woff", "woff2"]);
  const VIDEO_PREVIEW_EXT = new Set(["mp4", "webm", "mov", "m4v"]);
  const PREVIEW_MIME = {
    png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp",
    avif: "image/avif", bmp: "image/bmp", ico: "image/x-icon", svg: "image/svg+xml",
  };
  const FILE_CATEGORIES = [
    ["Image", "png jpg jpeg gif webp avif bmp ico svg icns tif tiff psd heic xcf"],
    ["Font", "ttf otf woff woff2 eot"],
    ["Archive", "zip gz tgz xz bz2 7z tar jar apk aab deb rpm dmg zst"],
    ["Audio", "mp3 ogg wav flac m4a opus weba aac"],
    ["Video", "mp4 webm mov mkv avi m4v"],
    ["Document", "pdf doc docx xls xlsx ppt pptx odt ods odp"],
    ["3D model", "glb gltf obj stl fbx blend"],
    ["Program", "exe dll so dylib a o bin wasm class pyc"],
    ["Data", "db sqlite dat pack idx keystore jks age"],
  ];
  const CATEGORY_BY_EXT = new Map();
  FILE_CATEGORIES.forEach(([category, exts]) => exts.split(" ").forEach((ext) => CATEGORY_BY_EXT.set(ext, category)));
  const MAX_PREVIEW_BYTES = 8 * 1024 * 1024;
  // A video streamed from its URL only downloads what its first frames need.
  const MAX_VIDEO_BYTES = 256 * 1024 * 1024;
  const VIDEO_FRAME_TIMEOUT_MS = 20000;
  const PREVIEW_MAX_SIDE = 768;
  // Decoded previews are kept for the viewer's lifetime unless this many
  // pixels of them pile up, so zooming never decodes (or fetches) one twice.
  const PREVIEW_BUDGET_PIXELS = 96 * 1024 * 1024;

  function extensionOf(name) {
    const lower = String(name || "").split("/").pop().toLowerCase();
    const dot = lower.lastIndexOf(".");
    return dot > 0 ? lower.slice(dot + 1) : "";
  }

  // "image", "font" and "video" load their bytes for a picture, "card" never
  // loads, "" is source text.
  function previewKindOf(name) {
    const ext = extensionOf(name);
    if (IMAGE_PREVIEW_EXT.has(ext)) return "image";
    if (FONT_PREVIEW_EXT.has(ext)) return "font";
    if (VIDEO_PREVIEW_EXT.has(ext)) return "video";
    return isBinaryName(name) ? "card" : "";
  }

  function fileCategoryOf(name) {
    return CATEGORY_BY_EXT.get(extensionOf(name)) || "Binary";
  }

  // The picture's rect inside a file cell (layout units, relative to the cell):
  // the same inset content box the source text uses, fitted to `aspect`.
  function previewArea(node, aspect) {
    const inset = Math.min(node.w, node.h) * 0.035;
    const cw = Math.max(0, node.w - inset * 2);
    const ch = Math.max(0, node.h - node.band - inset * 2);
    if (!(aspect > 0) || !(cw > 0) || !(ch > 0)) return { ox: inset, oy: node.band + inset, cw, ch };
    let w = cw, h = cw / aspect;
    if (h > ch) { h = ch; w = ch * aspect; }
    return { ox: inset + (cw - w) / 2, oy: node.band + inset + (ch - h) / 2, cw: w, ch: h };
  }

  function previewCanvas(width, height) {
    const canvas = document.createElement("canvas");
    canvas.width = Math.max(1, Math.round(width));
    canvas.height = Math.max(1, Math.round(height));
    return canvas;
  }

  // Resolves once `target` fires `ok`; rejects on `fail` or after `ms`.
  function eventOnce(target, ok, fail, ms) {
    return new Promise((resolve, reject) => {
      const done = (fn) => (value) => {
        clearTimeout(timer);
        target.removeEventListener(ok, onOk);
        target.removeEventListener(fail, onFail);
        fn(value);
      };
      const onOk = done(resolve);
      const onFail = done(() => reject(new Error(`${fail}`)));
      const timer = setTimeout(onFail, ms);
      target.addEventListener(ok, onOk);
      target.addEventListener(fail, onFail);
    });
  }

  // A video's thumbnail: a frame a little way in (first frames are often a
  // black fade), with a play badge so it does not read as a still image.
  async function decodeVideoFrame(source) {
    const video = document.createElement("video");
    const url = typeof source === "string" ? source : URL.createObjectURL(source);
    video.muted = true;
    video.playsInline = true;
    video.preload = "auto";
    try {
      const loaded = eventOnce(video, "loadeddata", "error", VIDEO_FRAME_TIMEOUT_MS);
      video.src = url;
      await loaded;
      const duration = Number.isFinite(video.duration) ? video.duration : 0;
      const at = Math.min(1, duration * 0.25);
      if (at > 0.05) {
        const seeked = eventOnce(video, "seeked", "error", VIDEO_FRAME_TIMEOUT_MS);
        video.currentTime = at;
        await seeked;
      }
      const natural = [video.videoWidth, video.videoHeight];
      if (!(natural[0] > 0) || !(natural[1] > 0)) return null;
      const scale = Math.min(1, PREVIEW_MAX_SIDE / Math.max(natural[0], natural[1]));
      const canvas = previewCanvas(natural[0] * scale, natural[1] * scale);
      const ctx = canvas.getContext("2d");
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
      const r = Math.max(10, Math.min(canvas.width, canvas.height) * 0.12);
      const x = canvas.width / 2, y = canvas.height / 2;
      ctx.fillStyle = "rgba(0,0,0,0.55)";
      ctx.beginPath();
      ctx.arc(x, y, r, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = "rgba(255,255,255,0.92)";
      ctx.beginPath();
      ctx.moveTo(x - r * 0.32, y - r * 0.45);
      ctx.lineTo(x + r * 0.5, y);
      ctx.lineTo(x - r * 0.32, y + r * 0.45);
      ctx.closePath();
      ctx.fill();
      return { canvas, width: natural[0], height: natural[1] };
    } finally {
      // Detaching the source stops any download still running.
      video.removeAttribute("src");
      video.load();
      if (url !== source) URL.revokeObjectURL(url);
    }
  }

  // Decodes an image, a font or a video frame into a bounded bitmap: { canvas,
  // width, height } with the natural pixel size, or null when the bytes do
  // not decode. `blob` is a video's URL when the host can stream it.
  async function decodePreview(node, blob, theme) {
    if (node.pk === "video") return decodeVideoFrame(blob);
    if (node.pk === "font") {
      if (typeof FontFace === "undefined" || !document.fonts) return null;
      const family = `fmcm-font-${node.id}`;
      const face = new FontFace(family, await blob.arrayBuffer());
      await face.load();
      document.fonts.add(face);
      try {
        const canvas = previewCanvas(640, 400);
        const ctx = canvas.getContext("2d");
        ctx.fillStyle = theme.fg;
        ctx.textBaseline = "alphabetic";
        ctx.font = `200px "${family}"`;
        ctx.fillText("Aa", 32, 220);
        ctx.font = `44px "${family}"`;
        ctx.fillText("The quick brown fox", 32, 300, 576);
        ctx.fillStyle = theme.muted;
        ctx.fillText("0123456789 &?!", 32, 364, 576);
        return { canvas, width: 0, height: 0 };
      } finally {
        document.fonts.delete(face);
      }
    }
    const typed = new Blob([blob], { type: PREVIEW_MIME[extensionOf(node.name)] || blob.type || "" });
    const url = URL.createObjectURL(typed);
    try {
      const image = new Image();
      image.decoding = "async";
      image.src = url;
      await image.decode();
      const natural = [image.naturalWidth, image.naturalHeight];
      // A size-less SVG still draws; give it a square.
      const w = natural[0] > 0 ? natural[0] : 512;
      const h = natural[1] > 0 ? natural[1] : 512;
      // Vectors are drawn at the full preview size, however small they say they are.
      const fit = PREVIEW_MAX_SIDE / Math.max(w, h);
      const scale = extensionOf(node.name) === "svg" ? fit : Math.min(1, fit);
      const canvas = previewCanvas(w * scale, h * scale);
      const ctx = canvas.getContext("2d");
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(image, 0, 0, canvas.width, canvas.height);
      return { canvas, width: natural[0], height: natural[1] };
    } finally {
      URL.revokeObjectURL(url);
    }
  }

  // The type card of a binary nothing can picture: a page outline in the
  // file's hue, its extension, and what kind of file that is.
  function paintTypeCard(theme, color, label, category) {
    const canvas = previewCanvas(240, 300);
    const ctx = canvas.getContext("2d");
    const fold = 56;
    ctx.beginPath();
    ctx.moveTo(24, 12);
    ctx.lineTo(216 - fold, 12);
    ctx.lineTo(216, 12 + fold);
    ctx.lineTo(216, 288);
    ctx.lineTo(24, 288);
    ctx.closePath();
    ctx.fillStyle = withAlpha(color, 0.1);
    ctx.fill();
    ctx.lineWidth = 6;
    ctx.lineJoin = "round";
    ctx.strokeStyle = withAlpha(color, 0.85);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(216 - fold, 12);
    ctx.lineTo(216 - fold, 12 + fold);
    ctx.lineTo(216, 12 + fold);
    ctx.stroke();
    ctx.textAlign = "center";
    ctx.textBaseline = "middle";
    ctx.fillStyle = theme.fg;
    const text = label.slice(0, 5);
    ctx.font = `700 ${text.length > 3 ? 44 : 56}px ${MONO}`;
    ctx.fillText(text, 120, 168, 176);
    ctx.fillStyle = theme.muted;
    ctx.font = `26px ${MONO}`;
    ctx.fillText(category, 120, 236, 176);
    return canvas;
  }

  function defaultFormatSize(bytes) {
    const n = Number(bytes) || 0;
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(n < 10240 ? 1 : 0)} KB`;
    return `${(n / 1024 / 1024).toFixed(n < 10485760 ? 1 : 0)} MB`;
  }

  // ---------------------------------------------------------------------
  // Tree model
  // ---------------------------------------------------------------------
  //
  // node.type: "dir" | "file" | "more". A "more" node is the host's "…"
  // overflow summary (it caps a directory at its 40 largest entries); a dir with
  // `lazy` is one whose children are not known yet. Both expand on demand
  // through fetchTree, so nothing in the repository is permanently folded away.
  const MIN_WEIGHT = 48; // bytes: keeps empty/tiny files a visible speck
  let nodeSerial = 0;

  function makeNode(type, name, path, size, parent) {
    return {
      id: ++nodeSerial, type, name, path, size: Math.max(0, Number(size) || 0), parent,
      depth: parent ? parent.depth + 1 : 0, children: null, weight: 0,
      x: 0, y: 0, w: 0, h: 0, band: 0, lang: "", color: "", lazy: false, approx: false,
      cs: 0, code: null, rev: 0, seen: 0, base: 0, top: 0, pk: "", preview: null,
    };
  }

  function buildTree(raw, rootName) {
    const root = makeNode("dir", String(rootName || ""), "", raw && raw.size, null);
    const fill = (node, rawChildren) => {
      node.children = [];
      (rawChildren || []).forEach((item) => {
        if (!item || typeof item !== "object") return;
        const name = String(item.name || "");
        if (item.type === "file") {
          const path = String(item.path || (node.path ? `${node.path}/${name}` : name));
          node.children.push(makeNode("file", name, path, item.size, node));
        } else if (item.type === "summary") {
          node.children.push(makeNode("more", "more…", node.path, item.size, node));
        } else {
          const child = makeNode("dir", name, node.path ? `${node.path}/${name}` : name, item.size, node);
          node.children.push(child);
          fill(child, item.children);
        }
      });
    };
    fill(root, raw && raw.children);
    return root;
  }

  function eachNode(root, visit) {
    const stack = [root];
    while (stack.length) {
      const node = stack.pop();
      visit(node);
      if (node.children) for (let i = node.children.length - 1; i >= 0; i -= 1) stack.push(node.children[i]);
    }
  }

  // Post-order: weights, the per-directory language byte tally, and file langs.
  function measureTree(root) {
    const totals = new Map();
    const walk = (node) => {
      if (node.type === "file") {
        node.lang = languageOf(node.name);
        node.pk = previewKindOf(node.name);
        node.weight = Math.max(node.size, MIN_WEIGHT);
        totals.set(node.lang, (totals.get(node.lang) || 0) + node.weight);
        return new Map([[node.lang, node.weight]]);
      }
      if (node.type === "more" || !node.children || !node.children.length) {
        node.weight = Math.max(node.size, MIN_WEIGHT);
        return new Map();
      }
      const tally = new Map();
      let weight = 0;
      node.children.forEach((child) => {
        walk(child).forEach((bytes, lang) => tally.set(lang, (tally.get(lang) || 0) + bytes));
        weight += child.weight;
      });
      node.weight = weight;
      let best = "";
      let bestBytes = -1;
      tally.forEach((bytes, lang) => { if (bytes > bestBytes) { best = lang; bestBytes = bytes; } });
      node.lang = best;
      return tally;
    };
    walk(root);
    return [...totals.entries()].sort((a, b) => b[1] - a[1]);
  }

  // ---------------------------------------------------------------------
  // Squarified treemap layout (Bruls, Huizing, van Wijk)
  // ---------------------------------------------------------------------
  function squarify(items, x, y, w, h) {
    let total = 0;
    items.forEach((item) => { total += item.weight; });
    if (!(total > 0) || !(w > 0) || !(h > 0)) {
      items.forEach((item) => { item.x = x; item.y = y; item.w = 0; item.h = 0; });
      return;
    }
    const scale = (w * h) / total;
    let rx = x, ry = y, rw = w, rh = h, i = 0;
    while (i < items.length) {
      const short = Math.max(Math.min(rw, rh), 1e-9);
      const s2 = short * short;
      let sum = 0, minA = Infinity, maxA = 0, best = Infinity, j = i;
      while (j < items.length) {
        const area = items[j].weight * scale;
        const nsum = sum + area;
        const nmin = Math.min(minA, area), nmax = Math.max(maxA, area);
        const worst = Math.max((s2 * nmax) / (nsum * nsum), (nsum * nsum) / (s2 * nmin));
        if (j > i && worst > best) break;
        best = worst; sum = nsum; minA = nmin; maxA = nmax; j += 1;
      }
      if (rw >= rh) {
        const stripW = Math.min(rw, sum / Math.max(rh, 1e-9));
        let cy = ry;
        for (let k = i; k < j; k += 1) {
          const item = items[k];
          const hh = stripW > 0 ? (item.weight * scale) / stripW : 0;
          item.x = rx; item.y = cy; item.w = stripW; item.h = hh; cy += hh;
        }
        rx += stripW; rw -= stripW;
      } else {
        const stripH = Math.min(rh, sum / Math.max(rw, 1e-9));
        let cx = rx;
        for (let k = i; k < j; k += 1) {
          const item = items[k];
          const ww = stripH > 0 ? (item.weight * scale) / stripH : 0;
          item.x = cx; item.y = ry; item.w = ww; item.h = stripH; cx += ww;
        }
        ry += stripH; rh -= stripH;
      }
      i = j;
    }
  }

  // Lays `node`'s subtree into its current rect. A directory keeps a label band
  // on top and a thin frame, both proportional to its own size so they stay in
  // scale at every zoom level; files keep a smaller name band.
  function layoutNode(node) {
    const short = Math.min(node.w, node.h);
    if (node.type !== "dir" || !node.children || !node.children.length) {
      node.band = node.type === "file" ? Math.min(node.h * 0.14, short * 0.07, 7) : 0;
      return;
    }
    const pad = Math.min(short * 0.012, 3);
    node.band = node.depth === 0 ? 0 : Math.min(14, short * 0.07, node.h * 0.25);
    const items = node.children.slice().sort((a, b) => b.weight - a.weight || (a.name < b.name ? -1 : 1));
    squarify(items, node.x + pad, node.y + pad + node.band,
      Math.max(0, node.w - pad * 2), Math.max(0, node.h - pad * 2 - node.band));
    items.forEach(layoutNode);
  }

  function layoutTree(root, width, height) {
    root.x = 0; root.y = 0; root.w = width; root.h = height;
    layoutNode(root);
  }

  // 3D elevations. A folder is a slab standing on its parent; its thickness
  // follows the square root of its share of the repository, so a folder four
  // times the size stands twice as tall and deep nests still fit on screen.
  function elevateTree(root, maxHeight) {
    const total = Math.max(root.size, 1);
    root.base = 0;
    root.top = maxHeight * 0.03;
    eachNode(root, (node) => {
      if (!node.parent) return;
      node.base = node.parent.top;
      const solid = node.type === "dir" && node.children && node.children.length;
      node.top = node.base + (solid
        ? Math.max(maxHeight * 0.012, maxHeight * Math.sqrt(node.size / total))
        : maxHeight * 0.006);
    });
  }

  // ---------------------------------------------------------------------
  // Source text: block-comment prepass and a small per-line tokenizer
  // ---------------------------------------------------------------------
  const MAX_LINES = 20000;
  const MAX_LINE_CHARS = 240;
  const CHAR_ASPECT = 0.5; // character cell width / line height
  const TOKEN_RE = /(\s+)|(\/\/.*|\/\*.*?(?:\*\/|$))|(#.*)|(--.*)|(<!--.*?(?:-->|$))|("""(?:.*?)(?:"""|$)|"(?:[^"\\]|\\.)*"?|'(?:[^'\\]|\\.)*'?|`(?:[^`\\]|\\.)*`?)|(\d[\w.]*)|([A-Za-z_$][\w$]*)|(.)/g;

  function prepareCode(text, lang) {
    let lines = String(text || "").replace(/\r\n?/g, "\n").split("\n");
    if (lines.length && lines[lines.length - 1] === "") lines.pop();
    const totalLines = lines.length;
    if (lines.length > MAX_LINES) lines = lines.slice(0, MAX_LINES);
    // Multi-line spans that read as comments: C-style blocks, markup comments,
    // and Python docstrings (one delimiter opens and closes).
    const open = lang === "Python" ? '"""' : NO_SLASH_COMMENT.has(lang) ? "<!--" : "/*";
    const close = open === "<!--" ? "-->" : open === "/*" ? "*/" : open;
    const useBlocks = open !== "<!--" || lang === "HTML" || lang === "XML" || lang === "SVG" || lang === "Markdown";
    const inBlock = new Uint8Array(lines.length);
    const lengths = [];
    let inside = false;
    for (let i = 0; i < lines.length; i += 1) {
      let line = lines[i];
      if (line.indexOf("\t") >= 0) line = line.replace(/\t/g, "    ");
      if (line.length > MAX_LINE_CHARS) line = line.slice(0, MAX_LINE_CHARS);
      lines[i] = line;
      lengths.push(line.length);
      if (!useBlocks) continue;
      inBlock[i] = inside ? 1 : 0;
      let from = 0;
      for (;;) {
        const at = line.indexOf(inside ? close : open, from);
        if (at < 0) break;
        from = at + (inside ? close.length : open.length);
        inside = !inside;
      }
    }
    lengths.sort((a, b) => a - b);
    // Column width follows the 97th-percentile line so one minified line does
    // not squash every other line in the file into a sliver.
    const wide = lengths.length ? lengths[Math.min(lengths.length - 1, Math.floor(lengths.length * 0.97))] : 0;
    return {
      lines, totalLines, lang, inBlock, close,
      maxLen: Math.max(12, Math.min(110, wide)),
      tokens: new Array(lines.length),
      key: "", cols: 1, rows: 1, lh: 0, colW: 0, ox: 0, oy: 0, cw: 0, ch: 0,
    };
  }

  // Tokens are [column, text, class]; class indexes theme.code:
  // 0 plain, 1 keyword, 2 string, 3 comment, 4 number, 5 call, 6 punctuation.
  function tokenizeLine(code, index) {
    const cached = code.tokens[index];
    if (cached) return cached;
    const line = code.lines[index];
    const lang = code.lang;
    const out = [];
    let start = 0;
    if (code.inBlock[index]) {
      const end = line.indexOf(code.close);
      const stop = end < 0 ? line.length : end + code.close.length;
      const lead = line.length - line.trimStart().length;
      if (stop > lead) out.push([lead, line.slice(lead, stop), 3]);
      start = stop;
    }
    const prose = PROSE.has(lang);
    TOKEN_RE.lastIndex = start;
    let match;
    while (start < line.length && (match = TOKEN_RE.exec(line))) {
      const at = match.index;
      const text = match[0];
      let cls = 0;
      let demote = false;
      if (match[1]) continue;
      if (match[2]) { if (NO_SLASH_COMMENT.has(lang)) demote = true; else cls = 3; }
      else if (match[3]) { if (HASH_COMMENT.has(lang)) cls = 3; else demote = true; }
      else if (match[4]) { if (DASH_COMMENT.has(lang)) cls = 3; else demote = true; }
      else if (match[5]) cls = 3;
      else if (match[6]) { if (prose) demote = true; else cls = text.startsWith('"""') ? 3 : 2; }
      else if (match[7]) cls = 4;
      else if (match[8]) {
        if (!prose && KEYWORDS.has(text)) cls = 1;
        else if (!prose && line.charCodeAt(at + text.length) === 40) cls = 5;
      } else cls = 6;
      if (demote) {
        // The comment/string opener means something else in this language:
        // emit its first character alone and rescan from the next one.
        out.push([at, text[0], 6]);
        TOKEN_RE.lastIndex = at + 1;
        continue;
      }
      out.push([at, text, cls]);
    }
    code.tokens[index] = out;
    return out;
  }

  // Fits the file's lines into its cell as newspaper columns, choosing the
  // column count that leaves the largest line height.
  function fitCode(node) {
    const code = node.code;
    const key = `${node.w}|${node.h}`;
    if (code.key === key) return code;
    code.key = key;
    node.rev += 1;
    const inset = Math.min(node.w, node.h) * 0.035;
    const cw = Math.max(0, node.w - inset * 2);
    const ch = Math.max(0, node.h - node.band - inset * 2);
    const count = Math.max(1, code.lines.length);
    const chars = code.maxLen + 3;
    let bestLh = 0, bestCols = 1;
    const limit = Math.min(count, 64);
    for (let cols = 1; cols <= limit; cols += 1) {
      const rows = Math.ceil(count / cols);
      const lh = Math.min(ch / rows, cw / (cols * chars * CHAR_ASPECT));
      if (lh > bestLh) { bestLh = lh; bestCols = cols; }
    }
    code.cols = bestCols;
    code.rows = Math.ceil(count / bestCols);
    code.lh = bestLh;
    code.colW = chars * CHAR_ASPECT * bestLh;
    code.ox = inset; code.oy = node.band + inset; code.cw = cw; code.ch = ch;
    return code;
  }

  // ---------------------------------------------------------------------
  // Code textures: one bitmap per (file, level of detail), LRU by pixels
  // ---------------------------------------------------------------------
  // Texture line height in device pixels. The fractional levels are the far
  // view: thousands of sub-pixel line bars that antialias into a density
  // texture, so a large file still reads as "full of code" from a distance.
  const LODS = [0.125, 0.25, 0.5, 1, 2, 4, 8, 16];
  const MAX_TEXTURE_SIDE = 2048;
  const MAX_TEXTURE_PIXELS = 4 * 1024 * 1024;
  const CLOSEUP_TEXTURE_SCALE = 2;
  // Generous, so every level already painted is still there when the zoom
  // comes back to it.
  const TEXTURE_BUDGET_PIXELS = 64 * 1024 * 1024;
  // The 3D view's GPU textures (code planes and previews) stay uploaded until
  // this many pixels of them pile up.
  const PLANE_BUDGET_PIXELS = 64 * 1024 * 1024;
  const MONO = "ui-monospace, SFMono-Regular, Menlo, Consolas, 'Liberation Mono', monospace";

  // Font size whose glyph advance equals one character cell, measured from the
  // font the browser actually resolved, so tokens placed by column stay flush.
  let monoAdvance = 0;
  function monoFontPx(charW, lineHeight) {
    if (!monoAdvance) {
      const probe = document.createElement("canvas").getContext("2d");
      probe.font = `100px ${MONO}`;
      monoAdvance = probe.measureText("MMMMMMMMMM").width / 1000 || 0.6;
    }
    return Math.min(charW / monoAdvance, lineHeight * 0.95);
  }

  function createTextureCache(getTheme) {
    const entries = new Map();
    let pixels = 0;

    function textureSize(node, lod) {
      const code = fitCode(node);
      if (!(code.lh > 0)) return null;
      const scale = lod / code.lh;
      const width = Math.ceil(code.cw * scale);
      const height = Math.ceil(code.ch * scale);
      if (width < 1 || height < 1) return null;
      // Levels past the ladder are the 3D close-up, where the text cannot be
      // drawn live and only a handful of tiles are near enough to want one.
      const roomy = lod > LODS[LODS.length - 1] ? CLOSEUP_TEXTURE_SCALE : 1;
      if (width > MAX_TEXTURE_SIDE * roomy || height > MAX_TEXTURE_SIDE * roomy
        || width * height > MAX_TEXTURE_PIXELS * roomy * 1.5) return null;
      return { width, height };
    }

    // Largest close-up level (a multiple of 8 above the ladder) that fits.
    function closeupLod(node, want) {
      const code = fitCode(node);
      const side = MAX_TEXTURE_SIDE * CLOSEUP_TEXTURE_SCALE;
      const fit = Math.min(
        (side * code.lh) / Math.max(code.cw, 1e-9),
        (side * code.lh) / Math.max(code.ch, 1e-9),
        Math.sqrt((MAX_TEXTURE_PIXELS * CLOSEUP_TEXTURE_SCALE * 1.5 * code.lh * code.lh) / Math.max(code.cw * code.ch, 1e-9)));
      const lod = Math.floor(Math.min(want, fit, 48) / 8) * 8;
      return lod > LODS[LODS.length - 1] ? lod : LODS[LODS.length - 1];
    }

    function paint(node, lod, size) {
      const theme = getTheme();
      const code = node.code;
      const canvas = document.createElement("canvas");
      canvas.width = size.width;
      canvas.height = size.height;
      const ctx = canvas.getContext("2d");
      ctx.fillStyle = theme.tile;
      ctx.fillRect(0, 0, size.width, size.height);
      const charW = lod * CHAR_ASPECT;
      const colW = code.colW * (lod / code.lh);
      const rows = code.rows;
      const maxLen = code.maxLen;
      if (lod <= 2) {
        const far = lod < 1;
        const bar = far ? lod * 0.75 : Math.max(1, lod * 0.7);
        for (let i = 0; i < code.lines.length; i += 1) {
          const line = code.lines[i];
          const lead = line.length - line.trimStart().length;
          const len = Math.min(line.trimEnd().length, maxLen) - lead;
          if (len <= 0) continue;
          const trimmed = line.charCodeAt(lead);
          const comment = code.inBlock[i] || trimmed === 35 /* # */ || (trimmed === 47 && line.charCodeAt(lead + 1) === 47);
          ctx.globalAlpha = far ? 0.9 : comment ? 0.5 : 0.62;
          ctx.fillStyle = theme.code[comment ? 3 : 0];
          ctx.fillRect(Math.floor(i / rows) * colW + lead * charW, (i % rows) * lod, far ? len * charW : Math.max(1, len * charW), bar);
        }
        ctx.globalAlpha = 1;
        return canvas;
      }
      const text = lod >= 8;
      if (text) {
        ctx.font = `${monoFontPx(charW, lod)}px ${MONO}`;
        ctx.textBaseline = "middle";
      }
      for (let i = 0; i < code.lines.length; i += 1) {
        const x0 = Math.floor(i / rows) * colW;
        const y0 = (i % rows) * lod;
        const tokens = tokenizeLine(code, i);
        for (let t = 0; t < tokens.length; t += 1) {
          const token = tokens[t];
          if (token[0] >= maxLen) break;
          const shown = token[0] + token[1].length > maxLen ? token[1].slice(0, maxLen - token[0]) : token[1];
          ctx.fillStyle = theme.code[token[2]];
          if (text) ctx.fillText(shown, x0 + token[0] * charW, y0 + lod / 2);
          else {
            ctx.globalAlpha = 0.8;
            ctx.fillRect(x0 + token[0] * charW, y0 + lod * 0.2, Math.max(1, shown.length * charW - 0.5), lod * 0.6);
          }
        }
      }
      ctx.globalAlpha = 1;
      return canvas;
    }

    return {
      size: textureSize,
      closeupLod,
      // Returns a canvas, null when this level cannot be a texture (too large),
      // or undefined when it is not built yet and `build` is false.
      get(node, lod, build) {
        const key = `${node.id}:${node.rev}:${lod}`;
        const hit = entries.get(key);
        if (hit) {
          entries.delete(key);
          entries.set(key, hit);
          return hit.canvas;
        }
        const size = textureSize(node, lod);
        if (!size) return null;
        if (!build) return undefined;
        const canvas = paint(node, lod, size);
        const cost = size.width * size.height;
        entries.set(key, { canvas, cost });
        pixels += cost;
        for (const [oldKey, old] of entries) {
          if (pixels <= TEXTURE_BUDGET_PIXELS || oldKey === key) break;
          entries.delete(oldKey);
          pixels -= old.cost;
        }
        return canvas;
      },
      // Any level already built for the node: shown while the right one waits
      // for its turn in the frame budget.
      any(node) {
        for (let i = LODS.length - 1; i >= 0; i -= 1) {
          const hit = entries.get(`${node.id}:${node.rev}:${LODS[i]}`);
          if (hit) return hit.canvas;
        }
        return null;
      },
      clear() { entries.clear(); pixels = 0; },
    };
  }

  function pickLod(devicePixelsPerLine) {
    const want = devicePixelsPerLine * 0.85;
    for (let i = 0; i < LODS.length; i += 1) if (LODS[i] >= want) return LODS[i];
    return want > LODS[LODS.length - 1] * 1.4 ? 0 : LODS[LODS.length - 1]; // 0: draw live text
  }

  // ---------------------------------------------------------------------
  // Content loader: view-driven, batched, two requests in flight at most
  // ---------------------------------------------------------------------
  const BATCH_PATHS = 40;
  const BATCH_BYTES = 1.5 * 1024 * 1024;
  const MAX_TEXT_RETRIES = 3;
  const TEXT_BUDGET_CHARS = 96 * 1024 * 1024;
  // Batches start at most about seven times a second, which keeps even a fast
  // mirror near half of the edge's per-visitor request rate limit.
  const BATCH_GAP_MS = 150;

  // Images, fonts and videos are one raw read each; they share the batches'
  // pacing and in-flight cap, so previews never add to the request rate. A
  // video goes to the decoder as `rawUrl(path)` when there is one, so the
  // browser streams only the start of it.
  function createLoader(fetchBlobs, fetchRaw, decode, onLoaded, onProgress, rawUrl) {
    let queue = [];
    let active = 0;
    let closed = false;
    let loadedChars = 0;
    let frame = 0;
    let lastStart = 0;
    let paceTimer = 0;
    const loaded = new Set();
    const previews = new Set();
    let previewPixels = 0;
    const counts = { done: 0, pending: 0 };
    const pictured = (node) => node.pk === "image" || node.pk === "font" || node.pk === "video";
    const streamed = (node) => node.pk === "video" && Boolean(rawUrl);

    function settlePreview(node, preview) {
      counts.pending -= 1;
      counts.done += 1;
      if (!preview) { node.cs = 3; return; }
      node.preview = preview;
      node.cs = 2;
      node.rev += 1;
      node.seen = frame;
      previews.add(node);
      previewPixels += preview.canvas.width * preview.canvas.height;
      if (previewPixels > PREVIEW_BUDGET_PIXELS) {
        [...previews].sort((a, b) => a.seen - b.seen).some((old) => {
          if (old === node) return false;
          previewPixels -= old.preview.canvas.width * old.preview.canvas.height;
          old.preview = null; old.cs = 0; old.rev += 1;
          previews.delete(old);
          return previewPixels <= PREVIEW_BUDGET_PIXELS * 0.8;
        });
      }
    }

    function finish() {
      active -= 1;
      if (closed) return;
      onProgress(counts);
      onLoaded();
      pump();
    }

    function retryLater(node) {
      counts.pending -= 1;
      node.retries = (node.retries || 0) + 1;
      if (node.retries > MAX_TEXT_RETRIES) { node.cs = 4; counts.done += 1; return; }
      node.cs = 0;
      queue.push(node);
    }

    function settle(node, record) {
      if (!record || record.missing) { retryLater(node); return; }
      counts.pending -= 1;
      counts.done += 1;
      const text = typeof record.text === "string" ? record.text : null;
      if (record.binary || text === null || text.indexOf(String.fromCharCode(0)) >= 0) { node.cs = 3; return; }
      node.code = prepareCode(text, node.lang);
      if (record.truncated) node.code.totalLines = Math.max(node.code.totalLines, node.code.lines.length + 1);
      node.cs = 2;
      node.seen = frame;
      loaded.add(node);
      loadedChars += text.length;
      if (loadedChars > TEXT_BUDGET_CHARS) {
        [...loaded].sort((a, b) => a.seen - b.seen).some((old) => {
          if (old === node) return false;
          loadedChars -= old.code.lines.reduce((sum, line) => sum + line.length + 1, 0);
          old.code = null; old.cs = 0; old.rev += 1;
          loaded.delete(old);
          return loadedChars <= TEXT_BUDGET_CHARS * 0.8;
        });
      }
    }

    function pump() {
      while (!closed && active < 2 && queue.length) {
        const wait = lastStart + BATCH_GAP_MS - Date.now();
        if (wait > 0) {
          clearTimeout(paceTimer);
          paceTimer = setTimeout(pump, wait);
          return;
        }
        queue = queue.filter((node) => node.cs === 0);
        if (!queue.length) break;
        lastStart = Date.now();
        active += 1;
        if (pictured(queue[0])) {
          const node = queue.shift();
          node.cs = 1;
          counts.pending += 1;
          onProgress(counts);
          Promise.resolve()
            .then(() => (streamed(node) ? rawUrl(node.path) : fetchRaw(node.path)))
            .then((blob) => (blob && !closed ? decode(node, blob) : null))
            .then((preview) => settlePreview(node, preview), () => settlePreview(node, null))
            .then(finish);
          continue;
        }
        // A text batch takes the wanted text files in order, leaving previews
        // queued where they were.
        const batch = [];
        let bytes = 0;
        for (let i = 0; i < queue.length && batch.length < BATCH_PATHS; i += 1) {
          const node = queue[i];
          if (pictured(node)) continue;
          if (node.retries && batch.length) continue;
          if (batch.length && bytes + node.size > BATCH_BYTES) break;
          node.cs = 1;
          batch.push(node);
          bytes += node.size;
          if (node.retries) break;
        }
        queue = queue.filter((node) => node.cs === 0);
        counts.pending += batch.length;
        onProgress(counts);
        Promise.resolve()
          .then(() => fetchBlobs(batch.map((node) => node.path)))
          .then((records) => { batch.forEach((node) => settle(node, records && records[node.path])); },
            () => { batch.forEach((node) => retryLater(node)); })
          .then(finish);
      }
    }

    return {
      // `nodes` is the current view's wish list, most important first. It
      // replaces the queue, so content nobody is looking at any more is never
      // fetched.
      want(nodes) {
        queue = nodes.filter((node) => {
          if (node.cs !== 0) return false;
          if (node.size === 0 || node.pk === "card") { node.cs = 3; return false; }
          if (pictured(node) && !(streamed(node) ? node.size <= MAX_VIDEO_BYTES
            : fetchRaw && node.size <= MAX_PREVIEW_BYTES)) { node.cs = 3; return false; }
          return true;
        });
        pump();
      },
      tick() { frame += 1; return frame; },
      close() { closed = true; queue = []; clearTimeout(paceTimer); },
    };
  }

  // ---------------------------------------------------------------------
  // Viewer
  // ---------------------------------------------------------------------
  const WORLD_H = 1000;
  const SCENE_SCALE = 0.1; // scene units per layout unit
  let current = null;

  function hexToRgb(hex) {
    const value = String(hex || "#888888").replace("#", "");
    return [0, 2, 4].map((at) => parseInt(value.slice(at, at + 2), 16) || 0);
  }

  function withAlpha(hex, alpha) {
    const [r, g, b] = hexToRgb(hex);
    return `rgba(${r},${g},${b},${alpha})`;
  }

  function escapeText(value) {
    return String(value == null ? "" : value).replace(/[&<>"']/g, (ch) => (
      { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch]));
  }

  function clipLabel(text, maxWidth, fontPx) {
    const room = Math.floor(maxWidth / (fontPx * 0.6));
    if (room < 3) return "";
    return text.length <= room ? text : `${text.slice(0, room - 1)}…`;
  }

  function viewerCss(theme) {
    return `
.fmcm{position:fixed;inset:0;z-index:90;display:flex;flex-direction:column;background:${theme.bg};color:${theme.fg};font:12px/1.4 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
.fmcm *{box-sizing:border-box}
.fmcm-bar,.fmcm-foot{display:flex;align-items:center;gap:8px;padding:8px 12px;background:${theme.panel};border-bottom:1px solid ${theme.border};min-width:0;flex-wrap:wrap}
.fmcm-foot{border-bottom:0;border-top:1px solid ${theme.border};padding:6px 12px;color:${theme.muted};font-size:11px}
.fmcm-title{font-size:13px;font-weight:600;white-space:nowrap}
.fmcm-crumbs{display:flex;align-items:center;gap:2px;min-width:0;flex:1 1 160px;overflow:hidden;white-space:nowrap;font-family:${MONO};font-size:11px;color:${theme.muted}}
.fmcm-crumbs button{border:0;background:none;color:${theme.fg};font:inherit;padding:2px 4px;border-radius:4px;cursor:pointer;max-width:16ch;overflow:hidden;text-overflow:ellipsis}
.fmcm-crumbs button:hover{background:${withAlpha(theme.muted, 0.2)}}
.fmcm-btn,.fmcm-seg button{height:28px;padding:0 10px;border:1px solid ${theme.border};border-radius:6px;background:${theme.bg};color:${theme.fg};font:inherit;font-weight:600;cursor:pointer}
.fmcm-btn:hover,.fmcm-seg button:hover{background:${withAlpha(theme.muted, 0.16)}}
.fmcm-btn:focus-visible,.fmcm-seg button:focus-visible,.fmcm-search:focus-visible,.fmcm-crumbs button:focus-visible{outline:2px solid ${theme.accent};outline-offset:1px}
.fmcm-seg{display:inline-flex}
.fmcm-seg button{border-radius:0;margin-left:-1px}
.fmcm-seg button:first-child{border-radius:6px 0 0 6px;margin-left:0}
.fmcm-seg button:last-child{border-radius:0 6px 6px 0}
.fmcm-seg button[aria-pressed="true"]{background:${theme.accent};border-color:${theme.accent};color:#fff}
.fmcm-search{height:28px;width:170px;max-width:40vw;padding:0 8px;border:1px solid ${theme.border};border-radius:6px;background:${theme.bg};color:${theme.fg};font:inherit}
.fmcm-stage{position:relative;flex:1 1 auto;min-height:0;overflow:hidden;background:${theme.bg};touch-action:none}
.fmcm-stage canvas{display:block;position:absolute;inset:0;width:100%;height:100%;outline:none;cursor:grab}
.fmcm-stage canvas.is-dragging{cursor:grabbing}
.fmcm-3d,.fmcm-labels{position:absolute;inset:0}
.fmcm-labels{pointer-events:none;overflow:hidden}
.fmcm-labels span{position:absolute;left:0;top:0;padding:1px 5px;border-radius:3px;background:${theme.pill};color:${theme.fg};font:10px/1.35 ${MONO};white-space:nowrap;will-change:transform}
.fmcm-tip{position:absolute;z-index:3;max-width:min(420px,80vw);padding:6px 9px;border:1px solid ${theme.border};border-radius:6px;background:${theme.panel};color:${theme.fg};font:11px/1.45 ${MONO};pointer-events:none;box-shadow:0 6px 24px rgba(0,0,0,.35);overflow-wrap:anywhere}
.fmcm-tip small{display:block;color:${theme.muted};font-size:10px}
.fmcm-note{position:absolute;left:50%;top:14px;transform:translateX(-50%);z-index:4;padding:6px 12px;border:1px solid ${theme.border};border-radius:999px;background:${theme.panel};color:${theme.fg};font-size:11px}
.fmcm-legend{display:flex;flex-wrap:wrap;gap:4px 12px;min-width:0;flex:1 1 240px;color:${theme.fg}}
.fmcm-legend span{display:inline-flex;align-items:center;gap:5px;white-space:nowrap}
.fmcm-legend i{width:9px;height:9px;border-radius:2px;display:inline-block}
.fmcm-focus{display:flex;align-items:center;gap:8px;min-width:0}
.fmcm-focus code{font:11px ${MONO};color:${theme.fg};max-width:38vw;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fmcm-hint{white-space:nowrap}
[hidden].fmcm-tip,[hidden].fmcm-note,[hidden].fmcm-3d,[hidden].fmcm-labels,.fmcm-stage canvas[hidden]{display:none}
@media (max-width:640px){.fmcm-hint{display:none}.fmcm-search{width:120px}}`;
  }

  function open(options) {
    if (current) current.close();
    const themeName = options.theme === "light" ? "light" : "dark";
    const theme = THEMES[themeName];
    const formatSize = options.formatSize || defaultFormatSize;
    const opener = document.activeElement;
    const root = buildTree(options.root, options.title || "repository");
    const ranked = measureTree(root).filter(([lang]) => lang !== "Other");
    const langColor = new Map();
    ranked.slice(0, theme.cats.length).forEach(([lang], index) => langColor.set(lang, theme.cats[index]));
    const colorOf = (lang) => langColor.get(lang) || theme.gray;

    function paintTree() {
      eachNode(root, (node) => {
        node.color = node.type === "more" ? theme.gray : colorOf(node.lang);
        node.stroke = withAlpha(node.color, node.type === "dir" ? 0.55 : 0.8);
        node.speck = withAlpha(node.color, 0.45);
      });
    }
    paintTree();

    // --- DOM -----------------------------------------------------------
    const el = document.createElement("div");
    el.className = "fmcm";
    el.setAttribute("role", "dialog");
    el.setAttribute("aria-modal", "true");
    el.setAttribute("aria-label", `Code map of ${options.title || "repository"}`);
    el.dataset.codemap = "1";
    el.innerHTML = `<style>${viewerCss(theme)}</style>
<header class="fmcm-bar">
  <span class="fmcm-title">Code map</span>
  <nav class="fmcm-crumbs" aria-label="Zoom path"></nav>
  <input class="fmcm-search" type="search" placeholder="Find file…" aria-label="Find file by path" />
  <div class="fmcm-seg" role="group" aria-label="View mode">
    <button type="button" data-codemap-mode="2d" aria-pressed="true">2D</button>
    <button type="button" data-codemap-mode="3d" aria-pressed="false">3D</button>
  </div>
  <button type="button" class="fmcm-btn" data-codemap-act="out" aria-label="Zoom out">−</button>
  <button type="button" class="fmcm-btn" data-codemap-act="in" aria-label="Zoom in">+</button>
  <button type="button" class="fmcm-btn" data-codemap-act="reset">Reset</button>
  <button type="button" class="fmcm-btn" data-codemap-act="close" aria-label="Close code map">Close</button>
</header>
<div class="fmcm-stage">
  <canvas data-codemap-canvas tabindex="0" aria-label="Treemap of every file in the repository, sized by bytes. Scroll to zoom, drag to pan, click to zoom into a folder or file."></canvas>
  <div class="fmcm-3d" hidden></div>
  <div class="fmcm-labels" hidden></div>
  <div class="fmcm-tip" hidden></div>
  <div class="fmcm-note" hidden role="status"></div>
</div>
<footer class="fmcm-foot">
  <div class="fmcm-legend" aria-label="Languages by color"></div>
  <span class="fmcm-status" role="status"></span>
  <span class="fmcm-hint"></span>
  <div class="fmcm-focus"></div>
</footer>`;
    document.body.appendChild(el);
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";

    const stage = el.querySelector(".fmcm-stage");
    const canvas = el.querySelector("[data-codemap-canvas]");
    const holder3d = el.querySelector(".fmcm-3d");
    const labelLayer = el.querySelector(".fmcm-labels");
    const tip = el.querySelector(".fmcm-tip");
    const note = el.querySelector(".fmcm-note");
    const crumbs = el.querySelector(".fmcm-crumbs");
    const statusEl = el.querySelector(".fmcm-status");
    const hintEl = el.querySelector(".fmcm-hint");
    const focusEl = el.querySelector(".fmcm-focus");
    const search = el.querySelector(".fmcm-search");
    const ctx = canvas.getContext("2d");

    const legendLangs = ranked.slice(0, theme.cats.length).map(([lang]) => lang);
    el.querySelector(".fmcm-legend").innerHTML = legendLangs.map((lang) => (
      `<span><i style="background:${colorOf(lang)}"></i>${escapeText(lang)}</span>`)).join("") +
      (ranked.length > legendLangs.length ? `<span><i style="background:${theme.gray}"></i>Other</span>` : "");

    // --- shared state --------------------------------------------------
    let closed = false;
    let mode = "2d";
    let W = 1, H = 1, dpr = 1, worldW = 1600;
    let focus = root;
    let hover = null;
    let matches = [];
    let matchSet = null;
    let matchIndex = -1;
    let scanTimer = 0;
    const view = { k: 1, tx: 0, ty: 0 };
    let atHome = true;
    let focusPinned = false; // goTo chose the focus; free navigation releases it
    let animation = 0;
    const cache = createTextureCache(() => theme);
    const loader = createLoader(
      options.fetchBlobs || (() => Promise.resolve({})),
      options.fetchRaw || null,
      (node, blob) => decodePreview(node, blob, theme),
      () => { if (mode === "2d") requestDraw(); else if (three) { three.planesDirty = true; three.invalidate(); } },
      (counts) => {
        statusEl.textContent = counts.pending > 0
          ? `Loading ${counts.pending} file${counts.pending === 1 ? "" : "s"}…`
          : "";
      },
      options.rawUrl || null);

    function setHint() {
      hintEl.textContent = mode === "2d"
        ? "Scroll to zoom · drag to pan · click to zoom in · Esc to zoom out"
        : "Drag to rotate · right-drag to pan · scroll to zoom · click to fly in";
    }

    function showNote(text) {
      note.textContent = text || "";
      note.hidden = !text;
    }

    // --- layout & view -------------------------------------------------
    function measureStage() {
      const rect = stage.getBoundingClientRect();
      W = Math.max(1, Math.round(rect.width));
      H = Math.max(1, Math.round(rect.height));
      dpr = Math.min(window.devicePixelRatio || 1, 2);
      canvas.width = Math.round(W * dpr);
      canvas.height = Math.round(H * dpr);
    }

    function fitFor(node, margin) {
      const pad = margin == null ? 0.03 : margin;
      const k = Math.min(W / Math.max(node.w, 1e-6), H / Math.max(node.h, 1e-6)) * (1 - pad * 2);
      return { k, tx: W / 2 - (node.x + node.w / 2) * k, ty: H / 2 - (node.y + node.h / 2) * k };
    }

    function clampView() {
      const home = fitFor(root, 0.03).k;
      view.k = Math.max(home * 0.6, Math.min(home * 8000, view.k));
      view.tx = Math.min(W * 0.8, Math.max(W * 0.2 - worldW * view.k, view.tx));
      view.ty = Math.min(H * 0.8, Math.max(H * 0.2 - WORLD_H * view.k, view.ty));
    }

    function animateView(target, duration) {
      cancelAnimationFrame(animation);
      const from = { k: view.k, cx: (W / 2 - view.tx) / view.k, cy: (H / 2 - view.ty) / view.k };
      const to = { k: target.k, cx: (W / 2 - target.tx) / target.k, cy: (H / 2 - target.ty) / target.k };
      const reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
      const ms = reduce ? 0 : (duration == null ? 420 : duration);
      const started = performance.now();
      const step = (now) => {
        if (closed) return;
        const t = ms ? Math.min(1, (now - started) / ms) : 1;
        const e = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
        const k = from.k * Math.pow(to.k / from.k, e);
        view.k = k;
        view.tx = W / 2 - (from.cx + (to.cx - from.cx) * e) * k;
        view.ty = H / 2 - (from.cy + (to.cy - from.cy) * e) * k;
        requestDraw();
        if (t < 1) animation = requestAnimationFrame(step);
      };
      animation = requestAnimationFrame(step);
    }

    function zoomAt(x, y, factor) {
      cancelAnimationFrame(animation);
      const before = view.k;
      view.k *= factor;
      clampView();
      const ratio = view.k / before;
      view.tx = x - (x - view.tx) * ratio;
      view.ty = y - (y - view.ty) * ratio;
      clampView();
      atHome = false;
      focusPinned = false;
      requestDraw();
    }

    function setFocus(node) {
      if (focus === node) return;
      focus = node;
      renderFocus();
    }

    function chainOf(node) {
      const chain = [];
      for (let at = node; at; at = at.parent) chain.unshift(at);
      return chain;
    }

    function renderFocus() {
      const chain = chainOf(focus);
      crumbs.innerHTML = chain.map((node, index) => (
        `${index ? "<span>/</span>" : ""}<button type="button" data-codemap-crumb="${index}">${escapeText(node.name || "repository")}</button>`)).join("");
      crumbs.querySelectorAll("[data-codemap-crumb]").forEach((button) => {
        button.addEventListener("click", () => goTo(chain[Number(button.dataset.codemapCrumb)]));
      });
      const parts = [];
      if (focus !== root) {
        parts.push(`<code title="${escapeText(focus.path)}">${escapeText(focus.path)}</code><span>${escapeText(describeSize(focus))}</span>`);
        if (focus.type === "file" && options.openFile) parts.push('<button type="button" class="fmcm-btn" data-codemap-open="file">Open file</button>');
        if (focus.type === "dir" && options.openFolder) parts.push('<button type="button" class="fmcm-btn" data-codemap-open="folder">Browse folder</button>');
      } else {
        parts.push(`<span>${escapeText(`${formatSize(root.size)} · ${Number(options.root && options.root.fileCount || 0).toLocaleString()} files`)}</span>`);
      }
      focusEl.innerHTML = parts.join("");
      const openButton = focusEl.querySelector("[data-codemap-open]");
      if (openButton) {
        openButton.addEventListener("click", () => {
          const target = focus;
          close();
          if (target.type === "file") options.openFile(target.path);
          else options.openFolder(target.path);
        });
      }
    }

    function describeSize(node) {
      const share = root.size > 0 ? (node.size / root.size) * 100 : 0;
      const pct = share >= 10 ? share.toFixed(0) : share >= 0.1 ? share.toFixed(1) : "<0.1";
      return `${node.approx ? "~" : ""}${formatSize(node.size)} · ${pct}%`;
    }

    // Moves either view to a node and makes it the focus.
    function goTo(node) {
      if (!node) return;
      setFocus(node);
      focusPinned = true;
      if (mode === "2d") {
        atHome = node === root;
        animateView(fitFor(node, node === root ? 0.03 : 0.04));
      } else if (three) three.flyTo(node);
    }

    function nodeAt(wx, wy, minPx) {
      let node = root;
      for (;;) {
        if (!node.children) return node;
        let next = null;
        for (let i = 0; i < node.children.length; i += 1) {
          const child = node.children[i];
          if (wx >= child.x && wx <= child.x + child.w && wy >= child.y && wy <= child.y + child.h) { next = child; break; }
        }
        if (!next || Math.min(next.w, next.h) * view.k < minPx) return node;
        node = next;
      }
    }

    // --- on-demand expansion of folded directories ---------------------
    function expandNode(node) {
      if (node.expanding || !options.fetchTree) return;
      node.expanding = true;
      const dir = node.type === "more" ? node.parent : node;
      Promise.resolve().then(() => options.fetchTree(dir.path)).then((entries) => {
        if (closed) return;
        const known = new Set(node.type === "more"
          ? dir.children.filter((child) => child.type !== "more").map((child) => child.name)
          : []);
        const fresh = (Array.isArray(entries) ? entries : []).filter((entry) => entry && entry.name && !known.has(String(entry.name)));
        const files = fresh.filter((entry) => entry.type !== "tree" && entry.type !== "commit");
        const dirs = fresh.filter((entry) => entry.type === "tree");
        const fileBytes = files.reduce((sum, entry) => sum + (Number(entry.size) || 0), 0);
        const each = dirs.length ? Math.max(MIN_WEIGHT, (node.size - fileBytes) / dirs.length) : 0;
        const prefix = dir.path ? `${dir.path}/` : "";
        const added = files.map((entry) => makeNode("file", String(entry.name), prefix + entry.name, entry.size, dir))
          .concat(dirs.map((entry) => {
            const child = makeNode("dir", String(entry.name), prefix + entry.name, each, dir);
            child.lazy = true;
            child.approx = true;
            return child;
          }));
        if (node.type === "more") {
          if (!added.length) { node.expanding = false; node.done = true; return; }
          dir.children = dir.children.filter((child) => child !== node).concat(added);
        } else {
          dir.children = added;
          dir.lazy = false;
        }
        measureTree(root);
        paintTree();
        layoutNode(dir);
        if (three) three.rebuild();
        requestDraw();
      }).catch(() => { node.expanding = false; node.done = true; });
    }

    const isStub = (node) => node.type === "more" || (node.type === "dir" && (!node.children || !node.children.length));
    const canExpand = (node) => !node.done && !node.expanding && (node.type === "more" || node.lazy);

    // --- 2D renderer ---------------------------------------------------
    let drawQueued = false;
    let labels = [];
    let wish = [];
    let budgetEnd = 0;
    let texturesPending = false;
    let frame = 0;

    function requestDraw() {
      if (drawQueued || closed || mode !== "2d") return;
      drawQueued = true;
      requestAnimationFrame(draw);
    }

    function drawCode(node, sx, sy) {
      const code = fitCode(node);
      const lhPx = code.lh * view.k;
      if (!(lhPx > 0)) return;
      const lod = pickLod(lhPx * dpr);
      let texture = null;
      if (lod) {
        texture = cache.get(node, lod, performance.now() < budgetEnd);
        if (texture === undefined) {
          texturesPending = true;
          texture = cache.any(node);
          if (!texture) return;
        }
      }
      const x0 = sx + code.ox * view.k;
      const y0 = sy + code.oy * view.k;
      if (texture) {
        ctx.drawImage(texture, x0, y0, code.cw * view.k, code.ch * view.k);
        return;
      }
      if (lhPx < 1.5) {
        // Too many lines for even the coarsest bitmap: an even wash stands in
        // for the text until the cell is zoomed far enough to draw it.
        ctx.globalAlpha = 0.14;
        ctx.fillStyle = theme.code[0];
        ctx.fillRect(x0, y0, code.cw * view.k, code.ch * view.k);
        ctx.globalAlpha = 1;
        return;
      }
      // Live drawing: the cell is too large for a bitmap at this zoom, so only
      // the lines inside the viewport are drawn, straight onto the canvas.
      const colW = code.colW * view.k;
      const charW = lhPx * CHAR_ASPECT;
      const c0 = Math.max(0, Math.floor(-x0 / colW));
      const c1 = Math.min(code.cols - 1, Math.floor((W - x0) / colW));
      const r0 = Math.max(0, Math.floor(-y0 / lhPx));
      const r1 = Math.min(code.rows - 1, Math.floor((H - y0) / lhPx));
      const text = lhPx >= 6;
      const tokens = lhPx >= 3;
      ctx.save();
      ctx.beginPath();
      ctx.rect(x0, y0, code.cw * view.k, code.ch * view.k);
      ctx.clip();
      if (text) { ctx.font = `${monoFontPx(charW, lhPx)}px ${MONO}`; ctx.textBaseline = "middle"; }
      for (let c = c0; c <= c1; c += 1) {
        for (let r = r0; r <= r1; r += 1) {
          const index = c * code.rows + r;
          if (index >= code.lines.length) break;
          const lx = x0 + c * colW;
          const ly = y0 + r * lhPx;
          if (!tokens) {
            const line = code.lines[index];
            const lead = line.length - line.trimStart().length;
            const len = Math.min(line.length, code.maxLen) - lead;
            if (len <= 0) continue;
            ctx.globalAlpha = 0.6;
            ctx.fillStyle = theme.code[code.inBlock[index] ? 3 : 0];
            ctx.fillRect(lx + lead * charW, ly, len * charW, Math.max(1, lhPx * 0.7));
            continue;
          }
          const parts = tokenizeLine(code, index);
          for (let t = 0; t < parts.length; t += 1) {
            const part = parts[t];
            if (part[0] >= code.maxLen) break;
            const shown = part[0] + part[1].length > code.maxLen ? part[1].slice(0, code.maxLen - part[0]) : part[1];
            ctx.fillStyle = theme.code[part[2]];
            if (text) ctx.fillText(shown, lx + part[0] * charW, ly + lhPx / 2);
            else { ctx.globalAlpha = 0.8; ctx.fillRect(lx + part[0] * charW, ly + lhPx * 0.2, shown.length * charW - 0.5, lhPx * 0.6); }
          }
        }
      }
      ctx.restore();
    }

    // One type card per (extension, hue), shared by every such file.
    const cards = new Map();
    function cardFor(node) {
      const ext = extensionOf(node.name);
      const key = `${ext}|${node.color}`;
      let card = cards.get(key);
      if (!card) {
        card = paintTypeCard(theme, node.color, (ext || "bin").toUpperCase(), fileCategoryOf(node.name));
        cards.set(key, card);
      }
      return card;
    }

    function drawPreview(node, source, sx, sy) {
      const area = previewArea(node, source.width / source.height);
      const w = area.cw * view.k, h = area.ch * view.k;
      // Icons blown up far past their pixels stay crisp instead of smeared.
      ctx.imageSmoothingEnabled = w < source.width * 4;
      ctx.drawImage(source, sx + area.ox * view.k, sy + area.oy * view.k, w, h);
      ctx.imageSmoothingEnabled = true;
    }

    function drawNode(node) {
      const k = view.k;
      const sx = node.x * k + view.tx, sy = node.y * k + view.ty;
      const sw = node.w * k, sh = node.h * k;
      if (sx > W || sy > H || sx + sw < 0 || sy + sh < 0 || sw < 0.7 || sh < 0.7) return;
      if (node.type === "dir" && node.children && node.children.length) {
        if (sw < 5 || sh < 5) { ctx.fillStyle = node.speck; ctx.fillRect(sx, sy, sw, sh); return; }
        ctx.fillStyle = theme.dirFill(node.depth);
        ctx.fillRect(sx, sy, sw, sh);
        ctx.strokeStyle = node.stroke;
        ctx.lineWidth = 1;
        ctx.strokeRect(sx + 0.5, sy + 0.5, sw - 1, sh - 1);
        for (let i = 0; i < node.children.length; i += 1) drawNode(node.children[i]);
        if (node.depth > 0 && sw >= 40 && sh >= 16) labels.push(node);
        return;
      }
      if (sw < 3.5 || sh < 3.5) { ctx.fillStyle = node.speck; ctx.fillRect(sx, sy, sw, sh); return; }
      ctx.fillStyle = theme.tile;
      ctx.fillRect(sx, sy, sw, sh);
      if (isStub(node)) {
        ctx.save();
        ctx.setLineDash([4, 3]);
        ctx.strokeStyle = withAlpha(theme.muted, 0.8);
        ctx.strokeRect(sx + 0.5, sy + 0.5, sw - 1, sh - 1);
        ctx.restore();
        if (sw >= 46 && sh >= 16) {
          ctx.font = `10px ${MONO}`;
          ctx.textBaseline = "middle";
          ctx.fillStyle = theme.muted;
          const text = node.type === "more" ? `… more (${formatSize(node.size)})` : `${node.name}/`;
          ctx.fillText(clipLabel(node.expanding ? "loading…" : text, sw - 8, 10), sx + 4, sy + Math.min(sh / 2, 10));
        }
        if (canExpand(node) && sw >= 90 && sh >= 50) expandNode(node);
        return;
      }
      let carded = false;
      if (node.preview) {
        node.seen = frame;
        if (sw >= 6 && sh >= 6) drawPreview(node, node.preview.canvas, sx, sy);
      } else if (node.cs === 3) {
        carded = sw >= 18 && sh >= 18;
        if (carded) drawPreview(node, cardFor(node), sx, sy);
      } else if (node.cs === 2) {
        node.seen = frame;
        if (sw >= 10 && sh >= 10) drawCode(node, sx, sy);
      } else if (node.cs === 0 && sw * sh >= 320 && Math.min(sw, sh) >= 12) {
        wish.push([sw * sh, node]);
      }
      ctx.strokeStyle = node.stroke;
      ctx.lineWidth = 1;
      ctx.strokeRect(sx + 0.5, sy + 0.5, sw - 1, sh - 1);
      const bandPx = node.band * k;
      if (bandPx >= 9 && sw >= 30) {
        const font = Math.min(12, bandPx * 0.72);
        ctx.font = `${font}px ${MONO}`;
        ctx.textBaseline = "middle";
        ctx.fillStyle = theme.fg;
        const left = Math.max(sx, 0) + 4;
        ctx.fillText(clipLabel(node.name, Math.min(sx + sw, W) - left - 4, font), left, sy + bandPx / 2 + 1);
      } else if ((node.cs === 3 || node.preview) && sw >= 60 && sh >= 24) {
        const text = clipLabel(node.name, sw - 8, 10);
        ctx.font = `10px ${MONO}`;
        ctx.textBaseline = "middle";
        if (carded || node.preview) {
          ctx.fillStyle = theme.pill;
          ctx.fillRect(sx + 2, sy + 3, text.length * 6.1 + 6, 14);
        }
        ctx.fillStyle = carded || node.preview ? theme.fg : theme.muted;
        ctx.fillText(text, sx + 4, sy + 10);
      }
    }

    function drawLabels() {
      const placed = [];
      const free = (x, y, w, h) => {
        for (let i = 0; i < placed.length; i += 1) {
          const p = placed[i];
          if (x < p[0] + p[2] && x + w > p[0] && y < p[1] + p[3] && y + h > p[1]) return false;
        }
        placed.push([x, y, w, h]);
        return true;
      };
      ctx.textBaseline = "middle";
      for (let i = 0; i < labels.length && placed.length < 400; i += 1) {
        const node = labels[i];
        const sx = node.x * view.k + view.tx, sy = node.y * view.k + view.ty;
        const sw = node.w * view.k, sh = node.h * view.k;
        const bandPx = node.band * view.k;
        const left = Math.max(sx, 0) + 4;
        const room = Math.min(sx + sw, W) - left - 4;
        if (bandPx >= 11 && sy > -bandPx * 0.4) {
          const font = Math.min(13, bandPx * 0.7);
          const text = clipLabel(node.name, room, font);
          if (!text || !free(left, sy, text.length * font * 0.6, bandPx)) continue;
          ctx.font = `600 ${font}px ${MONO}`;
          ctx.fillStyle = theme.fg;
          ctx.fillText(text, left, sy + bandPx / 2 + 0.5);
        } else if (sw >= 64 && sh >= 28) {
          const text = clipLabel(node.name, Math.min(room, 150), 10);
          const top = Math.max(sy, 0) + 2;
          const width = text.length * 6.1 + 8;
          if (!text || top + 14 > sy + sh || !free(left - 2, top, width, 14)) continue;
          ctx.fillStyle = theme.pill;
          ctx.fillRect(left - 2, top, width, 14);
          ctx.font = `10px ${MONO}`;
          ctx.fillStyle = theme.fg;
          ctx.fillText(text, left + 2, top + 7.5);
        }
      }
    }

    function outline(node, color, width) {
      const sx = node.x * view.k + view.tx, sy = node.y * view.k + view.ty;
      const sw = node.w * view.k, sh = node.h * view.k;
      if (sx > W || sy > H || sx + sw < 0 || sy + sh < 0) return;
      ctx.strokeStyle = color;
      ctx.lineWidth = width;
      if (sw < 4 || sh < 4) { ctx.fillStyle = color; ctx.fillRect(sx - 1.5, sy - 1.5, Math.max(4, sw + 3), Math.max(4, sh + 3)); return; }
      ctx.strokeRect(sx, sy, sw, sh);
    }

    function draw() {
      drawQueued = false;
      if (closed || mode !== "2d") return;
      frame = loader.tick();
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.globalAlpha = 1;
      ctx.fillStyle = theme.bg;
      ctx.fillRect(0, 0, W, H);
      labels = [];
      wish = [];
      texturesPending = false;
      budgetEnd = performance.now() + 7;
      drawNode(root);
      ctx.globalAlpha = 1;
      drawLabels();
      if (matchSet) matches.slice(0, 3000).forEach((node) => outline(node, theme.accent, 2));
      if (hover && hover !== root) outline(hover, theme.fg, 1.5);
      if (texturesPending) requestDraw();
      clearTimeout(scanTimer);
      scanTimer = setTimeout(scan, 140);
    }

    // After the view rests: ask for the source of the cells now big enough to
    // show it, and re-derive the focus from what fills the viewport.
    function scan() {
      if (closed || mode !== "2d") return;
      wish.sort((a, b) => b[0] - a[0]);
      loader.want(wish.slice(0, 400).map((item) => item[1]));
      if (focusPinned) return;
      if (atHome) { setFocus(root); return; } // a folder filling the home view is not a zoom
      const cx = (W / 2 - view.tx) / view.k, cy = (H / 2 - view.ty) / view.k;
      let node = root;
      for (;;) {
        const next = (node.children || []).find((child) => (
          cx >= child.x && cx <= child.x + child.w && cy >= child.y && cy <= child.y + child.h));
        if (!next || next.w * view.k * next.h * view.k < W * H * 0.4) break;
        node = next;
      }
      setFocus(node);
    }

    // --- tooltip -------------------------------------------------------
    function showTip(node, x, y) {
      if (!node || node === root) { tip.hidden = true; return; }
      const lines = node.cs === 2 && node.code ? ` · ${node.code.totalLines.toLocaleString()} lines`
        : node.preview && node.preview.width ? ` · ${node.preview.width}×${node.preview.height}` : "";
      const kind = node.type === "file" ? (node.pk === "card" ? fileCategoryOf(node.name) : node.lang || "file") : node.type === "more" ? "folded entries — click to expand" : "folder";
      tip.innerHTML = `${escapeText(node.type === "more" ? `${node.path}/…` : node.path)}<small>${escapeText(`${describeSize(node)} of repository · ${kind}${lines}`)}</small>`;
      tip.hidden = false;
      const tw = tip.offsetWidth, th = tip.offsetHeight;
      tip.style.left = `${Math.max(4, Math.min(W - tw - 4, x + 14))}px`;
      tip.style.top = `${Math.max(4, y + 16 + th > H ? y - th - 10 : y + 16)}px`;
    }

    // --- 2D input ------------------------------------------------------
    const pointers = new Map();
    let drag = null;
    let pinch = 0;

    function localPoint(event) {
      const rect = stage.getBoundingClientRect();
      return { x: event.clientX - rect.left, y: event.clientY - rect.top };
    }

    function click2D(x, y) {
      const wx = (x - view.tx) / view.k, wy = (y - view.ty) / view.k;
      const target = nodeAt(wx, wy, 26);
      if (isStub(target) && canExpand(target)) expandNode(target);
      const chain = chainOf(target);
      const start = Math.min(chain.length - 1, chain.indexOf(focus) + 1);
      let pick = target;
      for (let i = Math.max(1, start); i < chain.length; i += 1) {
        const node = chain[i];
        if (node.w * view.k * node.h * view.k < W * H * 0.5) { pick = node; break; }
      }
      if (pick === root) return;
      goTo(pick);
    }

    canvas.addEventListener("pointerdown", (event) => {
      canvas.setPointerCapture(event.pointerId);
      const point = localPoint(event);
      pointers.set(event.pointerId, point);
      if (pointers.size === 1) drag = { x: point.x, y: point.y, moved: false };
      else if (pointers.size === 2) {
        const [a, b] = [...pointers.values()];
        pinch = Math.hypot(a.x - b.x, a.y - b.y);
        if (drag) drag.moved = true;
      }
      cancelAnimationFrame(animation);
      canvas.classList.add("is-dragging");
    });
    canvas.addEventListener("pointermove", (event) => {
      const point = localPoint(event);
      const previous = pointers.get(event.pointerId);
      if (!previous) {
        const found = nodeAt((point.x - view.tx) / view.k, (point.y - view.ty) / view.k, 5);
        if (found !== hover) { hover = found; requestDraw(); }
        showTip(found, point.x, point.y);
        return;
      }
      pointers.set(event.pointerId, point);
      if (pointers.size === 2) {
        const [a, b] = [...pointers.values()];
        const distance = Math.hypot(a.x - b.x, a.y - b.y);
        if (pinch > 0 && distance > 0) zoomAt((a.x + b.x) / 2, (a.y + b.y) / 2, distance / pinch);
        pinch = distance;
        return;
      }
      if (!drag) return;
      if (!drag.moved && Math.hypot(point.x - drag.x, point.y - drag.y) < 4) return;
      drag.moved = true;
      tip.hidden = true;
      view.tx += point.x - previous.x;
      view.ty += point.y - previous.y;
      clampView();
      atHome = false;
      focusPinned = false;
      requestDraw();
    });
    const release = (event) => {
      const wasDrag = drag;
      const had = pointers.delete(event.pointerId);
      if (pointers.size < 2) pinch = 0;
      if (pointers.size === 0) {
        canvas.classList.remove("is-dragging");
        drag = null;
        if (had && wasDrag && !wasDrag.moved && event.type === "pointerup") {
          const point = localPoint(event);
          click2D(point.x, point.y);
        }
      }
    };
    canvas.addEventListener("pointerup", release);
    canvas.addEventListener("pointercancel", release);
    canvas.addEventListener("pointerleave", () => { if (!pointers.size) { tip.hidden = true; if (hover) { hover = null; requestDraw(); } } });
    canvas.addEventListener("wheel", (event) => {
      event.preventDefault();
      const point = localPoint(event);
      const delta = event.deltaMode === 1 ? event.deltaY * 16 : event.deltaY;
      zoomAt(point.x, point.y, Math.exp(-delta * (event.ctrlKey ? 0.01 : 0.0016)));
    }, { passive: false });
    canvas.addEventListener("dblclick", (event) => {
      const point = localPoint(event);
      const target = nodeAt((point.x - view.tx) / view.k, (point.y - view.ty) / view.k, 8);
      if (target.type === "file" && options.openFile) { close(); options.openFile(target.path); }
    });

    // --- 3D view -------------------------------------------------------
    let three = null;
    let threeLoading = null;

    function create3D(THREE) {
      const S = SCENE_SCALE;
      const renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: "high-performance" });
      renderer.setPixelRatio(dpr);
      renderer.setSize(W, H);
      renderer.setClearColor(new THREE.Color(theme.bg), 1);
      holder3d.appendChild(renderer.domElement);
      const dom = renderer.domElement;
      dom.setAttribute("tabindex", "0");
      dom.setAttribute("aria-label", "3D view of the repository. Folder height follows folder size. Drag to rotate, scroll to zoom, click to fly to a folder or file.");
      dom.dataset.codemapCanvas3d = "1";

      const scene = new THREE.Scene();
      scene.add(new THREE.HemisphereLight(0xffffff, themeName === "dark" ? 0x1a2030 : 0x9aa4b0, 1.5));
      const sun = new THREE.DirectionalLight(0xffffff, 1.7);
      sun.position.set(-0.6, 1, 0.45);
      scene.add(sun);
      const camera = new THREE.PerspectiveCamera(40, W / H, 0.1, 4000);
      const span = Math.max(worldW, WORLD_H) * S;
      const home = { radius: span * 1.15, theta: -0.38, phi: 0.9 };
      const orbit = { target: new THREE.Vector3(0, 0, 0), radius: home.radius, theta: home.theta, phi: home.phi };
      const box = new THREE.BoxGeometry(1, 1, 1).translate(0, 0.5, 0);
      const planeGeometry = new THREE.PlaneGeometry(1, 1);
      const raycaster = new THREE.Raycaster();
      const pointerNdc = new THREE.Vector2();
      const ground = new THREE.Plane(new THREE.Vector3(0, 1, 0), 0);
      const scratch = new THREE.Vector3();
      const scratchBox = new THREE.Box3();
      const frustum = new THREE.Frustum();
      const projection = new THREE.Matrix4();
      const marker = new THREE.LineSegments(
        new THREE.EdgesGeometry(box), new THREE.LineBasicMaterial({ color: new THREE.Color(theme.fg) }));
      marker.visible = false;
      marker.renderOrder = 5;
      scene.add(marker);

      let dirMesh = null, tileMesh = null;
      let dirNodes = [], tileNodes = [], labelDirs = [];
      const planes = new Map();
      const labelPool = [];
      let queued = false;
      let planeTimer = 0;
      let flight = 0;
      let hover3 = null;
      const self = { planesDirty: true };

      const cx = (node) => (node.x + node.w / 2 - worldW / 2) * S;
      const cz = (node) => (node.y + node.h / 2 - WORLD_H / 2) * S;

      function build() {
        elevateTree(root, Math.min(worldW, WORLD_H) * S * 0.2);
        dirNodes = [];
        tileNodes = [];
        eachNode(root, (node) => {
          if (!(node.w > 0) || !(node.h > 0)) return;
          if (node.type === "dir" && node.children && node.children.length) dirNodes.push(node);
          else tileNodes.push(node);
        });
        labelDirs = dirNodes.filter((node) => node !== root).sort((a, b) => b.size - a.size).slice(0, 1500);
        const matrix = new THREE.Matrix4();
        const position = new THREE.Vector3();
        const scale = new THREE.Vector3();
        const rotation = new THREE.Quaternion();
        const color = new THREE.Color();
        const hue = new THREE.Color();
        const surface = new THREE.Color(theme.bg);
        const make = (nodes, inset, tint) => {
          const mesh = new THREE.InstancedMesh(box, new THREE.MeshLambertMaterial(), Math.max(1, nodes.length));
          mesh.count = nodes.length;
          mesh.frustumCulled = false;
          nodes.forEach((node, index) => {
            const gap = Math.min(node.w, node.h) * inset;
            position.set(cx(node), node.base, cz(node));
            scale.set(Math.max(1e-4, (node.w - gap * 2) * S), Math.max(1e-4, node.top - node.base), Math.max(1e-4, (node.h - gap * 2) * S));
            mesh.setMatrixAt(index, matrix.compose(position, rotation, scale));
            mesh.setColorAt(index, tint(node, color));
          });
          mesh.instanceMatrix.needsUpdate = true;
          if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
          scene.add(mesh);
          return mesh;
        };
        dirMesh = make(dirNodes, 0, (node, out) => (node === root
          ? out.set(theme.panel)
          : out.set(node.color).lerp(surface, 0.1 + (node.depth % 2) * 0.16)));
        tileMesh = make(tileNodes, 0.045, (node, out) => (isStub(node)
          ? out.set(theme.gray).lerp(surface, 0.5)
          : out.set(theme.tile).lerp(hue.set(node.color), 0.12)));
      }

      // Planes stay once made, so zooming out and back in never drops,
      // repaints or re-uploads a texture. One goes only when its content
      // changes, or when PLANE_BUDGET_PIXELS forces out the ones seen longest ago.
      let planePixels = 0;
      function dropPlane(node) {
        const record = planes.get(node);
        if (!record) return;
        scene.remove(record.mesh);
        if (!record.shared) record.mesh.material.map.dispose();
        record.mesh.material.dispose();
        planePixels -= record.pixels;
        planes.delete(node);
      }

      function clearMeshes(keepPlanes) {
        [dirMesh, tileMesh].forEach((mesh) => {
          if (!mesh) return;
          scene.remove(mesh);
          mesh.material.dispose();
          mesh.dispose();
        });
        if (!keepPlanes) [...planes.keys()].forEach(dropPlane);
      }

      function applyCamera() {
        const sinPhi = Math.sin(orbit.phi);
        camera.position.set(
          orbit.target.x + orbit.radius * sinPhi * Math.sin(orbit.theta),
          orbit.target.y + orbit.radius * Math.cos(orbit.phi),
          orbit.target.z + orbit.radius * sinPhi * Math.cos(orbit.theta));
        camera.near = Math.max(0.002, orbit.radius * 0.02);
        camera.far = orbit.radius * 40 + span * 4;
        camera.updateProjectionMatrix();
        camera.lookAt(orbit.target);
        camera.updateMatrixWorld();
      }

      function clampOrbit() {
        orbit.phi = Math.max(0.04, Math.min(1.5, orbit.phi));
        orbit.radius = Math.max(0.02, Math.min(span * 4, orbit.radius));
        const limit = span * 0.75;
        orbit.target.x = Math.max(-limit, Math.min(limit, orbit.target.x));
        orbit.target.z = Math.max(-limit, Math.min(limit, orbit.target.z));
      }

      // Pixels one scene unit covers at `distance` from the camera.
      const pixelsPerUnit = (distance) => H / (2 * Math.tan((camera.fov * Math.PI) / 360) * Math.max(distance, 1e-6));

      function texture3(source) {
        const texture = new THREE.CanvasTexture(source);
        texture.colorSpace = THREE.SRGBColorSpace;
        texture.anisotropy = renderer.capabilities.getMaxAnisotropy();
        return texture;
      }

      // Card textures are shared by every file of a type, and outlive planes.
      const cardTextures = new Map();
      function cardTexture(source) {
        let texture = cardTextures.get(source);
        if (!texture) { texture = texture3(source); cardTextures.set(source, texture); }
        return texture;
      }

      const lift = (node) => node.top + Math.max(0.0005, (node.top - node.base) * 0.02);
      const previewKey = (node) => (node.preview ? `preview:${node.rev}` : node.cs === 3 ? "card" : "");

      // Whether a plane no longer shows its node: a preview or card replaced or
      // evicted, or source text evicted or laid out anew (fitCode bumps rev).
      const planeStale = (node, record) => (record.key
        ? record.key !== previewKey(node)
        : !node.code || (fitCode(node) && record.rev !== node.rev));

      // Lays the plane on the tile's top face: a preview fitted to its aspect,
      // code to the text's content box.
      function positionPlane(node, record) {
        const area = record.key ? previewArea(node, record.aspect) : fitCode(node);
        record.mesh.scale.set(area.cw * S, area.ch * S, 1);
        record.mesh.position.set(
          (node.x + area.ox + area.cw / 2 - worldW / 2) * S,
          lift(node),
          (node.y + area.oy + area.ch / 2 - WORLD_H / 2) * S);
      }

      function addPlane(node, record) {
        record.mesh.rotation.x = -Math.PI / 2;
        planes.set(node, record);
        planePixels += record.pixels;
        scene.add(record.mesh);
        positionPlane(node, record);
      }

      // An image, video frame, font specimen or type card on the tile's top face.
      function placePreview(node, source, shared) {
        const key = shared ? "card" : `preview:${node.rev}`;
        let record = planes.get(node);
        if (record && record.key !== key) { dropPlane(node); record = null; }
        if (!record) {
          record = {
            mesh: new THREE.Mesh(planeGeometry, new THREE.MeshBasicMaterial({
              map: shared ? cardTexture(source) : texture3(source), transparent: true,
              polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2,
            })),
            key, shared, aspect: source.width / source.height,
            pixels: shared ? 0 : source.width * source.height, seen: 0,
          };
          addPlane(node, record);
        }
        record.seen = frame;
      }

      // After a rebuild the tiles stand at new heights (and maybe new rects).
      function repositionPlanes() {
        [...planes].forEach(([node, record]) => {
          if (!(node.w > 0) || !(node.h > 0) || planeStale(node, record)) dropPlane(node);
          else positionPlane(node, record);
        });
      }

      // Code planes: the files largest on screen get their source as a texture
      // on the tile's top face, at the level of detail their distance needs.
      // A plane only ever steps up to a sharper level (mipmaps make a sharp
      // texture fine from afar), so zooming out never repaints anything.
      function updatePlanes() {
        projection.multiplyMatrices(camera.projectionMatrix, camera.matrixWorldInverse);
        frustum.setFromProjectionMatrix(projection);
        const ranked = [];
        for (let i = 0; i < tileNodes.length; i += 1) {
          const node = tileNodes[i];
          if (node.type !== "file") continue;
          const x = cx(node), z = cz(node);
          const ppu = pixelsPerUnit(camera.position.distanceTo(scratch.set(x, node.top, z)));
          const area = node.w * S * ppu * node.h * S * ppu;
          if (area < 500) continue;
          scratchBox.min.set(x - node.w * S / 2, node.base, z - node.h * S / 2);
          scratchBox.max.set(x + node.w * S / 2, node.top, z + node.h * S / 2);
          if (!frustum.intersectsBox(scratchBox)) continue;
          ranked.push([area, node, ppu]);
        }
        ranked.sort((a, b) => b[0] - a[0]);
        const chosen = ranked.slice(0, 260);
        loader.want(chosen.filter((item) => item[1].cs === 0).map((item) => item[1]));
        const keep = new Set();
        const deadline = performance.now() + 10;
        let pending = false;
        frame = loader.tick();
        chosen.forEach(([, node, ppu]) => {
          if (node.preview || node.cs === 3) {
            node.seen = frame;
            keep.add(node);
            placePreview(node, node.preview ? node.preview.canvas : cardFor(node), !node.preview);
            return;
          }
          if (node.cs !== 2) return;
          node.seen = frame;
          keep.add(node);
          const code = fitCode(node);
          const want = code.lh * S * ppu * dpr;
          let lod = pickLod(want) || cache.closeupLod(node, want);
          if (lod > LODS[LODS.length - 1] && !cache.size(node, lod)) lod = LODS[LODS.length - 1];
          while (lod > LODS[0] && !cache.size(node, lod)) lod /= 2;
          let record = planes.get(node);
          if (record && (record.key || record.rev !== node.rev)) { dropPlane(node); record = null; }
          if (record) record.seen = frame;
          if (record && record.lod >= lod) return;
          const bitmap = cache.get(node, lod, performance.now() < deadline);
          if (bitmap === undefined) { pending = true; return; }
          if (!bitmap) return;
          const pixels = bitmap.width * bitmap.height;
          if (record) {
            record.mesh.material.map.dispose();
            record.mesh.material.map = texture3(bitmap);
            record.mesh.material.needsUpdate = true;
            planePixels += pixels - record.pixels;
            record.pixels = pixels;
            record.lod = lod;
          } else {
            addPlane(node, {
              mesh: new THREE.Mesh(planeGeometry, new THREE.MeshBasicMaterial({
                map: texture3(bitmap), polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2,
              })),
              lod, rev: node.rev, pixels, seen: frame,
            });
          }
        });
        [...planes].forEach(([node, record]) => { if (!keep.has(node) && planeStale(node, record)) dropPlane(node); });
        if (planePixels > PLANE_BUDGET_PIXELS) {
          [...planes].filter(([node]) => !keep.has(node)).sort((a, b) => a[1].seen - b[1].seen)
            .some(([node]) => { dropPlane(node); return planePixels <= PLANE_BUDGET_PIXELS * 0.8; });
        }
        self.planesDirty = pending;
        return chosen;
      }

      function updateLabels(fileCandidates) {
        const placed = [];
        let used = 0;
        const place = (node, x, z, text) => {
          scratch.set(x, node.top, z).project(camera);
          if (scratch.z < -1 || scratch.z > 1 || Math.abs(scratch.x) > 1.02 || Math.abs(scratch.y) > 1.02) return;
          const px = (scratch.x * 0.5 + 0.5) * W, py = (-scratch.y * 0.5 + 0.5) * H;
          const width = text.length * 6.1 + 10;
          for (let i = 0; i < placed.length; i += 1) {
            const p = placed[i];
            if (px < p[0] + p[2] && px + width > p[0] && py < p[1] + 15 && py + 15 > p[1]) return;
          }
          placed.push([px, py, width]);
          let span3 = labelPool[used];
          if (!span3) { span3 = document.createElement("span"); labelLayer.appendChild(span3); labelPool.push(span3); }
          used += 1;
          if (span3.textContent !== text) span3.textContent = text;
          span3.style.display = "";
          span3.style.transform = `translate(${px.toFixed(1)}px,${py.toFixed(1)}px)`;
        };
        for (let i = 0; i < labelDirs.length && used < 150; i += 1) {
          const node = labelDirs[i];
          const x = cx(node), z = cz(node);
          const ppu = pixelsPerUnit(camera.position.distanceTo(scratch.set(x, node.top, z)));
          if (node.w * S * ppu < 70 || node.h * S * ppu < 26) continue;
          place(node, x - node.w * S * 0.47, z - node.h * S * 0.47, clipLabel(node.name, 170, 10));
        }
        (fileCandidates || []).forEach(([, node, ppu]) => {
          if (used >= 170 || node.w * S * ppu < 120) return;
          place(node, cx(node) - node.w * S * 0.44, cz(node) - node.h * S * 0.47, clipLabel(node.name, 170, 10));
        });
        for (let i = used; i < labelPool.length; i += 1) labelPool[i].style.display = "none";
      }

      let lastFiles = [];
      let passSignature = "";
      const cameraSignature = () => [orbit.radius, orbit.theta, orbit.phi, orbit.target.x, orbit.target.y, orbit.target.z]
        .map((value) => value.toFixed(4)).join("|");
      // Renders on demand. A plane pass follows once the camera rests (or while
      // textures are still waiting for their turn), so an idle view costs nothing.
      function render() {
        queued = false;
        if (closed || mode !== "3d") return;
        applyCamera();
        renderer.render(scene, camera);
        updateLabels(lastFiles);
        if (!self.planesDirty && cameraSignature() === passSignature) return;
        clearTimeout(planeTimer);
        planeTimer = setTimeout(() => {
          if (closed || mode !== "3d") return;
          passSignature = cameraSignature();
          lastFiles = updatePlanes();
          invalidate();
        }, self.planesDirty ? 30 : 160);
      }

      function invalidate() {
        if (queued || closed) return;
        queued = true;
        requestAnimationFrame(render);
      }

      function pick(x, y) {
        pointerNdc.set((x / W) * 2 - 1, -(y / H) * 2 + 1);
        applyCamera();
        raycaster.setFromCamera(pointerNdc, camera);
        const hits = raycaster.intersectObjects([dirMesh, tileMesh], false);
        if (!hits.length) return null;
        const hit = hits[0];
        return (hit.object === dirMesh ? dirNodes : tileNodes)[hit.instanceId] || null;
      }

      function mark(node) {
        if (!node || node === root) { marker.visible = false; return; }
        marker.visible = true;
        marker.position.set(cx(node), node.base, cz(node));
        marker.scale.set(node.w * S * 1.002, (node.top - node.base) * 1.01, node.h * S * 1.002);
      }

      function flyTo(node, instant) {
        cancelAnimationFrame(flight);
        const to = node === root
          ? { x: 0, y: 0, z: 0, radius: home.radius }
          : { x: cx(node), y: node.top, z: cz(node), radius: Math.max(0.05, Math.max(node.w, node.h) * S * (node.type === "file" ? 1.5 : 1.7)) };
        const from = { x: orbit.target.x, y: orbit.target.y, z: orbit.target.z, radius: orbit.radius };
        const reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
        const ms = instant || reduce ? 0 : 600;
        const started = performance.now();
        const step = (now) => {
          if (closed) return;
          const t = ms ? Math.min(1, (now - started) / ms) : 1;
          const e = t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2;
          orbit.target.set(from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e, from.z + (to.z - from.z) * e);
          orbit.radius = from.radius * Math.pow(to.radius / from.radius, e);
          invalidate();
          if (t < 1) flight = requestAnimationFrame(step);
        };
        flight = requestAnimationFrame(step);
      }

      function dolly(factor, x, y) {
        const before = orbit.radius;
        orbit.radius *= factor;
        clampOrbit();
        if (x != null && orbit.radius < before) {
          pointerNdc.set((x / W) * 2 - 1, -(y / H) * 2 + 1);
          raycaster.setFromCamera(pointerNdc, camera);
          ground.constant = -orbit.target.y;
          if (raycaster.ray.intersectPlane(ground, scratch)) {
            orbit.target.lerp(scratch, 1 - orbit.radius / before);
            clampOrbit();
          }
        }
        invalidate();
      }

      // --- 3D input ---
      const active = new Map();
      let gesture = null;
      let pinch3 = 0;
      let hoverAt = 0;
      dom.addEventListener("contextmenu", (event) => event.preventDefault());
      dom.addEventListener("pointerdown", (event) => {
        dom.setPointerCapture(event.pointerId);
        const point = localPoint(event);
        active.set(event.pointerId, point);
        cancelAnimationFrame(flight);
        if (active.size === 1) gesture = { x: point.x, y: point.y, moved: false, pan: event.button !== 0 || event.shiftKey };
        else if (active.size === 2) {
          const [a, b] = [...active.values()];
          pinch3 = Math.hypot(a.x - b.x, a.y - b.y);
          if (gesture) gesture.moved = true;
        }
      });
      const panBy = (dx, dy) => {
        const unit = 1 / pixelsPerUnit(orbit.radius);
        const sin = Math.sin(orbit.theta), cos = Math.cos(orbit.theta);
        orbit.target.x += (-dx * cos - dy * sin) * unit;
        orbit.target.z += (dx * sin - dy * cos) * unit;
        clampOrbit();
      };
      dom.addEventListener("pointermove", (event) => {
        const point = localPoint(event);
        const previous = active.get(event.pointerId);
        if (!previous) {
          const now = performance.now();
          if (now - hoverAt < 50) return;
          hoverAt = now;
          const found = pick(point.x, point.y);
          if (found !== hover3) { hover3 = found; mark(found); invalidate(); }
          showTip(found, point.x, point.y);
          return;
        }
        active.set(event.pointerId, point);
        if (active.size === 2) {
          const [a, b] = [...active.values()];
          const distance = Math.hypot(a.x - b.x, a.y - b.y);
          if (pinch3 > 0 && distance > 0) dolly(pinch3 / distance);
          pinch3 = distance;
          panBy((point.x - previous.x) / 2, (point.y - previous.y) / 2);
          invalidate();
          return;
        }
        if (!gesture) return;
        if (!gesture.moved && Math.hypot(point.x - gesture.x, point.y - gesture.y) < 4) return;
        gesture.moved = true;
        tip.hidden = true;
        const dx = point.x - previous.x, dy = point.y - previous.y;
        if (gesture.pan) panBy(dx, dy);
        else { orbit.theta -= dx * 0.006; orbit.phi -= dy * 0.006; clampOrbit(); }
        invalidate();
      });
      const release3 = (event) => {
        const was = gesture;
        const had = active.delete(event.pointerId);
        if (active.size < 2) pinch3 = 0;
        if (active.size) return;
        gesture = null;
        if (!had || !was || was.moved || event.type !== "pointerup" || event.button !== 0) return;
        const point = localPoint(event);
        const node = pick(point.x, point.y);
        if (!node) return;
        if (isStub(node) && canExpand(node)) { expandNode(node); return; }
        setFocus(node);
        flyTo(node);
      };
      dom.addEventListener("pointerup", release3);
      dom.addEventListener("pointercancel", release3);
      dom.addEventListener("pointerleave", () => { if (!active.size) { tip.hidden = true; if (hover3) { hover3 = null; mark(null); invalidate(); } } });
      dom.addEventListener("wheel", (event) => {
        event.preventDefault();
        const point = localPoint(event);
        const delta = event.deltaMode === 1 ? event.deltaY * 16 : event.deltaY;
        dolly(Math.exp(delta * (event.ctrlKey ? 0.01 : 0.0016)), point.x, point.y);
      }, { passive: false });
      dom.addEventListener("dblclick", (event) => {
        const point = localPoint(event);
        const node = pick(point.x, point.y);
        if (node && node.type === "file" && options.openFile) { close(); options.openFile(node.path); }
      });

      build();
      Object.assign(self, {
        invalidate,
        flyTo,
        dolly,
        orbit,
        rotate(dTheta, dPhi) { orbit.theta += dTheta; orbit.phi += dPhi; clampOrbit(); invalidate(); },
        reset() { orbit.theta = home.theta; orbit.phi = home.phi; flyTo(root); },
        rebuild() { clearMeshes(true); build(); repositionPlanes(); self.planesDirty = true; invalidate(); },
        resize() { renderer.setPixelRatio(dpr); renderer.setSize(W, H); camera.aspect = W / H; invalidate(); },
        stats() { return { dirs: dirNodes.length, tiles: tileNodes.length, planes: planes.size, labels: labelPool.filter((item) => item.style.display !== "none").length, calls: renderer.info.render.calls }; },
        dispose() {
          clearTimeout(planeTimer);
          cancelAnimationFrame(flight);
          clearMeshes();
          cardTextures.forEach((texture) => texture.dispose());
          box.dispose();
          planeGeometry.dispose();
          marker.geometry.dispose();
          marker.material.dispose();
          renderer.dispose();
          if (renderer.forceContextLoss) renderer.forceContextLoss();
          dom.remove();
        },
      });
      return self;
    }

    function setMode(next) {
      if (next === mode || closed) return Promise.resolve();
      el.querySelectorAll("[data-codemap-mode]").forEach((button) => {
        button.setAttribute("aria-pressed", String(button.dataset.codemapMode === next));
      });
      tip.hidden = true;
      if (next === "2d") {
        mode = "2d";
        holder3d.hidden = true;
        labelLayer.hidden = true;
        canvas.hidden = false;
        setHint();
        const target = fitFor(focus, focus === root ? 0.03 : 0.04);
        Object.assign(view, target);
        requestDraw();
        return Promise.resolve();
      }
      showNote("Loading 3D view…");
      threeLoading = threeLoading || import(THREE_URL);
      return threeLoading.then((THREE) => {
        if (closed) return;
        mode = "3d";
        clearTimeout(scanTimer);
        if (!three) three = create3D(THREE);
        canvas.hidden = true;
        holder3d.hidden = false;
        labelLayer.hidden = false;
        showNote("");
        setHint();
        three.resize();
        three.planesDirty = true;
        if (focus !== root) three.flyTo(focus, true);
        three.invalidate();
      }).catch((error) => {
        threeLoading = null;
        mode = "2d";
        el.querySelectorAll("[data-codemap-mode]").forEach((button) => {
          button.setAttribute("aria-pressed", String(button.dataset.codemapMode === "2d"));
        });
        showNote("The 3D view needs WebGL, which this browser could not start.");
        setTimeout(() => showNote(""), 5000);
        if (window.console) console.warn("codemap 3D unavailable", error);
      });
    }

    // --- search --------------------------------------------------------
    function runSearch() {
      const query = search.value.trim().toLowerCase();
      matches = [];
      matchSet = null;
      matchIndex = -1;
      if (query) {
        eachNode(root, (node) => { if (node.type === "file" && node.path.toLowerCase().includes(query)) matches.push(node); });
        matches.sort((a, b) => b.size - a.size);
        matchSet = new Set(matches);
        statusEl.textContent = `${matches.length.toLocaleString()} matching file${matches.length === 1 ? "" : "s"} · Enter to visit`;
      } else statusEl.textContent = "";
      requestDraw();
    }
    search.addEventListener("input", runSearch);
    search.addEventListener("keydown", (event) => {
      if (event.key !== "Enter" || !matches.length) return;
      event.preventDefault();
      matchIndex = (matchIndex + (event.shiftKey ? -1 : 1) + matches.length) % matches.length;
      goTo(matches[matchIndex]);
    });

    // --- chrome --------------------------------------------------------
    function zoomStep(factor) {
      if (mode === "2d") zoomAt(W / 2, H / 2, factor);
      else if (three) three.dolly(1 / factor);
    }

    function reset() {
      setFocus(root);
      if (mode === "2d") { atHome = true; animateView(fitFor(root, 0.03)); }
      else if (three) three.reset();
    }

    el.addEventListener("click", (event) => {
      const modeButton = event.target.closest("[data-codemap-mode]");
      if (modeButton) { setMode(modeButton.dataset.codemapMode); return; }
      const act = event.target.closest("[data-codemap-act]");
      if (!act) return;
      if (act.dataset.codemapAct === "close") close();
      else if (act.dataset.codemapAct === "in") zoomStep(1.6);
      else if (act.dataset.codemapAct === "out") zoomStep(1 / 1.6);
      else reset();
    });

    function onKey(event) {
      if (closed) return;
      const typing = event.target === search;
      if (event.key === "Escape") {
        event.preventDefault();
        event.stopPropagation();
        if (typing && search.value) { search.value = ""; runSearch(); return; }
        if (focus !== root) goTo(focus.parent || root);
        else close();
        return;
      }
      if (typing) return;
      if (event.key === "/") { event.preventDefault(); search.focus(); return; }
      if (event.key === "+" || event.key === "=") zoomStep(1.4);
      else if (event.key === "-" || event.key === "_") zoomStep(1 / 1.4);
      else if (event.key === "0") reset();
      else if (event.key.startsWith("Arrow")) {
        const dx = event.key === "ArrowLeft" ? 1 : event.key === "ArrowRight" ? -1 : 0;
        const dy = event.key === "ArrowUp" ? 1 : event.key === "ArrowDown" ? -1 : 0;
        if (mode === "2d") { view.tx += dx * 80; view.ty += dy * 80; clampView(); atHome = false; focusPinned = false; requestDraw(); }
        else if (three) three.rotate(dx * 0.12, dy * 0.08);
      } else return;
      event.preventDefault();
    }
    document.addEventListener("keydown", onKey, true);

    const resizer = new ResizeObserver(() => {
      if (closed) return;
      const center = { x: (W / 2 - view.tx) / view.k, y: (H / 2 - view.ty) / view.k };
      measureStage();
      if (atHome) Object.assign(view, fitFor(root, 0.03));
      else { view.tx = W / 2 - center.x * view.k; view.ty = H / 2 - center.y * view.k; clampView(); }
      if (three) three.resize();
      requestDraw();
    });

    function close() {
      if (closed) return;
      closed = true;
      cancelAnimationFrame(animation);
      clearTimeout(scanTimer);
      resizer.disconnect();
      document.removeEventListener("keydown", onKey, true);
      loader.close();
      cache.clear();
      if (three) three.dispose();
      el.remove();
      document.body.style.overflow = previousOverflow;
      if (current === handle) current = null;
      if (opener && opener.focus && document.contains(opener)) opener.focus();
      if (options.onClose) options.onClose();
    }

    // --- start ---------------------------------------------------------
    measureStage();
    worldW = Math.round(WORLD_H * Math.max(1, Math.min(2.4, W / H)));
    layoutTree(root, worldW, WORLD_H);
    Object.assign(view, fitFor(root, 0.03));
    setHint();
    renderFocus();
    resizer.observe(stage);
    requestDraw();
    el.querySelector('[data-codemap-act="close"]').focus();

    const findPath = (path) => {
      let found = null;
      eachNode(root, (node) => { if (!found && node.type !== "more" && node.path === path) found = node; });
      return found;
    };
    const handle = {
      close,
      setMode,
      goToPath(path) { const node = path ? findPath(path) : root; if (node) goTo(node); return Boolean(node); },
      // Read-only snapshot for tests and diagnostics.
      state() {
        let files = 0, loadedFiles = 0, previews = 0;
        eachNode(root, (node) => {
          if (node.type !== "file") return;
          files += 1;
          if (node.code) loadedFiles += 1;
          if (node.preview) previews += 1;
        });
        return {
          mode, files, loadedFiles, previews, focus: focus.path, scale: view.k, width: W, height: H,
          three: three ? three.stats() : null,
        };
      },
    };
    current = handle;
    return handle;
  }

  const api = {
    // The open viewer's handle (close, setMode, goToPath, state), or null.
    get current() { return current; },
    buildTree, measureTree, squarify, layoutTree, elevateTree, prepareCode, tokenizeLine, fitCode,
    languageOf, previewKindOf, fileCategoryOf, previewArea, pickLod, createLoader, THREE_URL, open,
  };
  if (typeof module !== "undefined" && module.exports) module.exports = api;
  if (typeof window === "undefined") return;
  window.ForkMeshCodeMap = api;
})();
