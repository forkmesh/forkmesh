
/* ThreeUI <TerrainPlumeCanvas variant="…"> ported to vanilla JS for this single-file page.
   The GLSL in terrainPlumeShaders.js is carried over verbatim (VERTEX_SHADER / FRAGMENT_SHADER
   strings and the host preambles), as is the raw-WebGL host in TerrainPlumeCanvas.tsx: context
   options, program creation, interleaved quad, separate blend func, DPR<=2 resize, the
   IntersectionObserver/visibility/reduced-motion playback gate, context-loss handling and the
   plume/​filter style maths.

   The alpine range in TerrainPlumeEngravedTerrain.jsx is kept byte-for-byte upstream for
   provenance, so — exactly as TerrainPlumeSceneTerrain.jsx does for river / desert / bridge-city —
   this file authors an additional scene against the same hidden-line camera: a retro sunset drive.
   Constant-depth grid rows are drawn near to far and clipped against the silhouette raised by
   everything already on the canvas, props are cut into that same silhouette at their own depth,
   and a dithered pixel wipe uncovers the block the first time it scrolls into view. */
(function () {
'use strict';

/* ---------------------------------------------------------------- shaders --- */
/* Exact GLSL authored in promptui commit c35c24f0e92e80fbfd98f81595bd024e69aa31b4. */
const VERTEX_SHADER = `
  varying vec2 vUv;

  void main() {
    vUv = uv;
    gl_Position = vec4(position, 1.0);
  }
`;

const FRAGMENT_SHADER = `
  uniform float u_time;
  uniform vec2 u_resolution;
  varying vec2 vUv;

  float hash(float n) {
    return fract(sin(n) * 10000.0);
  }

  float noise(float x) {
    float i = floor(x);
    float f = fract(x);
    float u = f * f * (3.0 - 2.0 * f);
    return mix(hash(i), hash(i + 1.0), u);
  }

  void main() {
    vec2 st = gl_FragCoord.xy / u_resolution.xy;

    float wave1 = sin(st.x * 3.0 + u_time * 0.42) * 0.1;
    float wave2 = sin(st.x * 5.0 - u_time * 0.28) * 0.05;
    float wave3 = sin(st.x * 9.0 + u_time * 0.14) * 0.025;
    float combinedWave = wave1 + wave2 + wave3;

    float baseIntensity = smoothstep(0.48, -0.1, st.y + combinedWave);
    float variation = noise(st.x * 2.2 + u_time * 0.08) * 0.52 + 0.48;
    float plume = pow(baseIntensity * variation * 1.35, 1.46);

    float centerFalloff = smoothstep(1.16, 0.34, length((st - vec2(0.5, 0.04)) * vec2(0.86, 1.0)));
    float sideFalloff = smoothstep(0.92, 0.08, abs(st.x - 0.5));
    float scan = sin((st.y * 640.0) + u_time * 12.0) * 0.015;
    float grain = noise(st.x * 70.0 + u_time * 0.18) * 0.025;

    float light = plume * centerFalloff * sideFalloff;
    vec3 color = vec3(0.0);
    color += vec3(0.78) * light * 0.52;
    color += vec3(0.18) * pow(light, 0.72) * 0.24;
    color += vec3(scan + grain) * light;

    float alpha = clamp(light * 0.72, 0.0, 0.78);
    gl_FragColor = vec4(color, alpha);
  }
`;

/* The source ShaderMaterial injected these declarations around the authored GLSL. */
const VERTEX_HOST_PREAMBLE = `precision highp float;
attribute vec3 position;
attribute vec2 uv;
`;
const FRAGMENT_HOST_PREAMBLE = `precision highp float;
`;

/* The authored plume is monochrome, so each scene is tinted through the plume
   canvas only: sepia() gives the grey light a hue for hue-rotate() to place,
   and the engraved scene above it keeps its own ink. */
const SCENE_PLUME_TINT = {
  alpine: undefined,
  river: "sepia(0.6) hue-rotate(148deg) saturate(1.75) brightness(1.62)",
  desert: "sepia(0.8) hue-rotate(-18deg) saturate(2.1) brightness(1.6)",
  "bridge-city": "sepia(0.58) hue-rotate(176deg) saturate(2.3) brightness(1.7)",
  "sunset-drive": "sepia(0.92) hue-rotate(-14deg) saturate(3.1) brightness(1.34)",
};

const TERRAIN_PLUME_DEFAULTS = {
  speed: 1, size: 1, thickness: 1, strength: 1, softness: 0,
  opacity: 1, hue: 0, saturation: 1, brightness: 1,
};

function clamp(value, minimum, maximum) { return Math.min(maximum, Math.max(minimum, value)); }

function compileShader(gl, type, source) {
  const shader = gl.createShader(type);
  if (!shader) throw new Error("Unable to create Terrain Plume shader");
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
    const log = gl.getShaderInfoLog(shader);
    gl.deleteShader(shader);
    throw new Error(`Terrain Plume shader failed to compile: ${log}`);
  }
  return shader;
}

function createProgram(gl) {
  const program = gl.createProgram();
  if (!program) throw new Error("Unable to create Terrain Plume program");
  const vertex = compileShader(gl, gl.VERTEX_SHADER, VERTEX_HOST_PREAMBLE + VERTEX_SHADER);
  const fragment = compileShader(gl, gl.FRAGMENT_SHADER, FRAGMENT_HOST_PREAMBLE + FRAGMENT_SHADER);
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.linkProgram(program);
  gl.deleteShader(vertex);
  gl.deleteShader(fragment);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    const log = gl.getProgramInfoLog(program);
    gl.deleteProgram(program);
    throw new Error(`Terrain Plume program failed to link: ${log}`);
  }
  return program;
}

/* ---------------------------------------------------------------- noise ---- */
function ihash(x, y) {
  let h = Math.imul(x | 0, 374761393) ^ Math.imul(y | 0, 668265263);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}

function vnoise(x, y) {
  const xi = Math.floor(x);
  const yi = Math.floor(y);
  const xf = x - xi;
  const yf = y - yi;
  const u = xf * xf * (3 - 2 * xf);
  const v = yf * yf * (3 - 2 * yf);
  const a = ihash(xi, yi);
  const b = ihash(xi + 1, yi);
  const c = ihash(xi, yi + 1);
  const d = ihash(xi + 1, yi + 1);
  return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}

function mulberry(seed) {
  let a = seed;
  return function next() {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const smooth = (a, b, input) => {
  let t = (input - a) / (b - a);
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t * t * (3 - 2 * t);
};

/* ------------------------------------------------------ sunset drive scene -- */
/* A dead-flat plane so the constant-depth rows read as the grid itself, a road
   corridor cut out of it, and the props that make it a drive: light standards on
   both shoulders and reflector posts between them. The lane markings scroll with
   the same phase clock as the grid, so everything moves at one speed. */
const DRIVE = {
  ink: "255, 222, 186",
  camY: 0.62,
  zNear: 1.55,
  zFar: 15.5,
  gridZ: 0.78,      /* world spacing of the transverse grid lines */
  gridX: 0.9,       /* world spacing of the longitudinal grid lines */
  halfX: 22,        /* grid extent either side of the road */
  laneHalf: 1.12,   /* road half-width */
  dashLen: 0.7,
  dashGap: 0.86,
  poleZ: 4.6,       /* world spacing of the light standards */
  postZ: 1.3,       /* world spacing of the reflector posts */
  speed: 2.35,      /* world units per second */
  gantryZ: 9.4,     /* world spacing of the overhead sign gantries */
  gantryNear: 5.6,  /* closer than this a gantry fills the frame, so it is dropped */
  poleNear: 2.9,
  stars: 150,
  sunR: 0.2,        /* sun radius, fraction of band height */
  sunSlats: 8,
};

function REVEAL_defaults() { return { CELL: 7, BAND: 0.2, SOFT: 0.055 }; }

/* Engraved sunset drive, rendered into a band pinned to the bottom of the mount. */
function createDriveTerrain(root, tcv, rcv, options) {
  /* The scene is drawn onto an offscreen plate and then laid down through a tape
     pass: per-band horizontal wobble, a head-switching skew at the bottom edge and
     a colour fringe. The page's own VHS layer is additive only, so it can put grain
     and scanlines over the footer but cannot warp what is under it. */
  const work = document.createElement("canvas");
  const tx = work.getContext("2d");
  const vx = tcv.getContext("2d");
  const rx = rcv.getContext("2d");
  if (!tx || !vx || !rx) return null;

  const REPLAY = true;
  const PARALLAX = true;
  const DEFAULT_BG = "#030304";
  const INK = DRIVE.ink;
  const reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const BG = getComputedStyle(root).getPropertyValue("--fe-bg").trim() || DEFAULT_BG;

  /* --------------------------------------------------------- camera ----- */
  const P = {
    fov: 50,
    camY: DRIVE.camY,
    horizon: 0.575,   /* principal point — no pitch, so verticals stay vertical */
    zNear: DRIVE.zNear,
    zFar: DRIVE.zFar,
    spanPad: 1.26,
  };

  let DPR = 1, W = 0, H = 0, RW = 0, RH = 0, CW = 0, CH = 0;
  let camX = 0, camY = P.camY, tgtX = 0, tgtY = P.camY;
  let F = 1000, OX = 0, OY = 0, phase = 0;

  let envY = new Float32Array(1);   /* upper silhouette raised by the props, per half-device column */
  let envD = new Float32Array(1);   /* depth (0 far … 1 near) of that silhouette */
  let EW = 1;
  let horizonY = 0;
  let poles = [], posts = [], stars = [], gantries = [];

  /* the envelope is kept at half device resolution: it is the hottest loop in
     the renderer and a half-pixel of hidden-line jitter is invisible */
  function rasterInto(px, py, n, depth) {
    for (let j = 0; j < n - 1; j += 1) {
      let x0 = px[j] * 0.5, y0 = py[j], x1 = px[j + 1] * 0.5, y1 = py[j + 1];
      if (x1 < x0) { let t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
      let c0 = Math.ceil(x0), c1 = Math.floor(x1);
      if (c1 < 0 || c0 >= EW || c1 < c0) continue;
      if (c0 < 0) c0 = 0;
      if (c1 > EW - 1) c1 = EW - 1;
      const dx = x1 - x0, dy = y1 - y0;
      if (dx < 1e-6) {
        const y = y0 < y1 ? y0 : y1;
        if (y < envY[c0]) { envY[c0] = y; envD[c0] = depth; }
        continue;
      }
      for (let c = c0; c <= c1; c += 1) {
        const y = y0 + dy * ((c - x0) / dx);
        if (y < envY[c]) { envY[c] = y; envD[c] = depth; }
      }
    }
  }

  const segX = new Float32Array(2), segY = new Float32Array(2);
  function rasterPoly(pts, depth) {
    for (let i = 0; i < pts.length - 1; i += 1) {
      segX[0] = pts[i][0]; segY[0] = pts[i][1];
      segX[1] = pts[i + 1][0]; segY[1] = pts[i + 1][1];
      rasterInto(segX, segY, 2, depth);
    }
  }

  const envAt = (x) => { const c = (x * 0.5) | 0; return c < 0 || c >= EW ? 1e9 : envY[c]; };

  /* the region still above every silhouette drawn so far, narrowed to the
     columns a prop actually covers so the clip stays cheap */
  function clipAbove(x0, x1) {
    const c0 = Math.max(0, Math.floor(x0 / 2) - 1);
    const c1 = Math.min(EW - 1, Math.ceil(x1 / 2) + 1);
    tx.save();
    tx.beginPath();
    tx.moveTo(c0 * 2, -H);
    tx.lineTo(c1 * 2, -H);
    for (let c = c1; c >= c0; c -= 1) { const y = envY[c]; tx.lineTo(c * 2, y > H + 4 ? H + 4 : y); }
    tx.closePath();
    tx.clip();
  }

  function tracePoly(pts, close) {
    tx.beginPath();
    tx.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length; i += 1) tx.lineTo(pts[i][0], pts[i][1]);
    if (close) tx.closePath();
  }
  function solid(pts, alpha) {
    tracePoly(pts, true);
    tx.fillStyle = BG; tx.fill();
    tx.strokeStyle = `rgba(${INK},${alpha.toFixed(3)})`; tx.stroke();
  }
  function line(pts, alpha, width) {
    tx.lineWidth = width;
    tracePoly(pts, false);
    tx.strokeStyle = `rgba(${INK},${alpha.toFixed(3)})`; tx.stroke();
  }

  /* screen projection of a point on the road plane */
  const projX = (x, invZ) => OX + (x - camX) * invZ;
  const projY = (y, invZ) => OY - (y - camY) * invZ;

  /* ------------------------------------------------- pixel dissolve ----- */
  const { CELL, BAND, SOFT } = REVEAL_defaults();
  let dither = null, dCols = 0, dRows = 0, dCell = 7;

  function buildDither() {
    dCell = Math.max(4, Math.round(CELL * DPR));
    dCols = Math.ceil(RW / dCell) + 1;
    dRows = Math.ceil(RH / dCell) + 1;
    dither = new Float32Array(dCols * dRows);
    for (let y = 0; y < dRows; y += 1) {
      for (let x = 0; x < dCols; x += 1) {
        const clump = vnoise(x * 0.115, y * 0.115);   /* organic tendrils */
        const fine = vnoise(x * 0.46 + 31.5, y * 0.46 + 9.1);
        const white = ihash(x + 977, y + 311);
        dither[y * dCols + x] = 0.42 * clump + 0.24 * fine + 0.34 * white;
      }
    }
  }

  /* the standards and posts are laid out on a fixed world lattice and simply
     slide through the phase offset, so nothing pops as the drive advances */
  function buildScene() {
    const rnd = mulberry(20260902);
    poles = []; posts = [];
    const lanes = DRIVE.laneHalf + 0.5;
    for (let i = 0; i < 26; i += 1) {
      poles.push({ k: i, side: i % 2 === 0 ? -1 : 1, x: lanes + 0.62 + rnd() * 0.12, h: 0.94 + rnd() * 0.1, arm: 0.46 + rnd() * 0.1 });
    }
    for (let i = 0; i < 64; i += 1) posts.push({ k: i, side: i % 2 === 0 ? -1 : 1, x: lanes, h: 0.115 + rnd() * 0.03 });
    /* the sky is fixed to the canvas rather than to the world: at this focal
       length real parallax on a star field is invisible and only costs a redraw */
    gantries = [];
    for (let i = 0; i < 4; i += 1) gantries.push({ k: i, h: 1.26, sign: 0.3 + rnd() * 0.06 });
    stars = [];
    for (let i = 0; i < DRIVE.stars; i += 1) {
      const y = Math.pow(rnd(), 1.6);                      /* denser toward the top */
      stars.push({ x: rnd(), y, a: 0.10 + Math.pow(rnd(), 2.2) * 0.5, tw: rnd() * 6.283 });
    }
  }

  function resize() {
    DPR = Math.min(2, window.devicePixelRatio || 1);
    const rootRect = root.getBoundingClientRect();
    CW = Math.max(1, Math.round(rootRect.width));
    CH = Math.max(1, Math.round(rootRect.height));

    /* the band is sized in CSS (--fe-band), which also reserves the matching
       padding under the copy — read it back rather than driving layout here */
    const bandCss = Math.max(1, Math.round(tcv.getBoundingClientRect().height));

    W = Math.round(CW * DPR); H = Math.round(bandCss * DPR);
    tcv.width = W; tcv.height = H;
    work.width = W; work.height = H;
    RW = Math.round(CW * DPR); RH = Math.round(CH * DPR);
    rcv.width = RW; rcv.height = RH;

    F = (H * 0.5) / Math.tan((P.fov * Math.PI) / 360);
    OX = W * 0.5; OY = H * P.horizon;
    horizonY = OY + camY * (F / P.zFar);

    EW = (W >> 1) + 1;
    envY = new Float32Array(EW);
    envD = new Float32Array(EW);
    buildScene();
    buildDither();
  }

  /* --------------------------------------------------------- the sky ---- */
  /* Stars and the haze bars that sit on the horizon. Drawn after the ground so
     they land at the back of the scene, and clipped against the silhouette that
     everything in front of them has already raised. */
  function drawSky() {
    clipAbove(-8, W + 8);
    const top = horizonY;
    for (let i = 0; i < stars.length; i += 1) {
      const st = stars[i];
      const y = st.y * top;
      if (y > top - 2) continue;
      const tw = 0.72 + 0.28 * Math.sin(phase * 1.7 + st.tw);
      const a = st.a * tw * (0.35 + 0.65 * (1 - st.y));    /* fade into the horizon glow */
      const sPx = a > 0.42 ? Math.max(1, DPR * 1.3) : Math.max(1, DPR * 0.9);
      tx.fillStyle = `rgba(${INK},${a.toFixed(3)})`;
      tx.fillRect(st.x * W, y, sPx, sPx);
    }
    /* haze bars: the flat stratus that makes a synthetic sunset read as one */
    tx.lineWidth = Math.max(1, DPR * 1.1);
    for (let i = 0; i < 5; i += 1) {
      const y = top - (8 + i * 13) * DPR;
      if (y < 0) break;
      const half = W * (0.42 - i * 0.055);
      const cx = OX - W * 0.1 + Math.sin(phase * 0.04 + i) * W * 0.02;
      const g = tx.createLinearGradient(cx - half, 0, cx + half, 0);
      g.addColorStop(0, `rgba(${INK},0)`);
      g.addColorStop(0.5, `rgba(${INK},${(0.13 - i * 0.02).toFixed(3)})`);
      g.addColorStop(1, `rgba(${INK},0)`);
      tx.strokeStyle = g;
      tx.beginPath(); tx.moveTo(cx - half, y); tx.lineTo(cx + half, y); tx.stroke();
    }
    tx.restore();
  }

  /* --------------------------------------------------------- the sun ---- */
  /* Drawn last, so it sits at the back of the scene, and clipped against the
     silhouette everything in front of it has already raised. */
  function drawSun() {
    const r = H * DRIVE.sunR;
    const cx = OX - W * 0.185, cy = horizonY - r * 0.13;
    clipAbove(cx - r - 8, cx + r + 8);

    /* it sets behind the ground rather than over it: nothing of the disc, its rim or
       its halo is drawn below the horizon line */
    tx.beginPath(); tx.rect(0, 0, W, horizonY); tx.clip();

    const halo = tx.createRadialGradient(cx, cy, r * 0.7, cx, cy, r * 2.5);
    halo.addColorStop(0, `rgba(${INK},0.09)`);
    halo.addColorStop(0.45, `rgba(${INK},0.035)`);
    halo.addColorStop(1, `rgba(${INK},0)`);
    tx.fillStyle = halo;
    tx.fillRect(cx - r * 2.5, cy - r * 2.5, r * 5, r * 5);

    tx.lineWidth = Math.max(1, DPR * 0.9);
    /* the disc itself: an opaque body so the plume does not print through it,
       then the rim, then the slats that cut it into a retro sun */
    tx.beginPath(); tx.arc(cx, cy, r, 0, Math.PI * 2);
    tx.fillStyle = BG; tx.fill();
    tx.strokeStyle = `rgba(${INK},0.34)`; tx.stroke();

    const grad = tx.createLinearGradient(0, cy - r, 0, cy + r);
    grad.addColorStop(0, `rgba(${INK},0.09)`);
    grad.addColorStop(0.52, `rgba(${INK},0.26)`);
    grad.addColorStop(1, `rgba(${INK},0.52)`);
    tx.save();
    tx.beginPath(); tx.arc(cx, cy, r * 0.995, 0, Math.PI * 2); tx.clip();
    tx.fillStyle = grad; tx.fillRect(cx - r, cy - r, r * 2, r * 2);
    /* slats widen toward the bottom of the disc — the classic cut */
    tx.globalCompositeOperation = "destination-out";
    const n = DRIVE.sunSlats;
    for (let i = 0; i < n; i += 1) {
      const t = i / (n - 1);
      const y = cy - r * 0.12 + t * r * 1.06;
      const h = r * (0.012 + 0.085 * t * t);
      tx.fillRect(cx - r, y, r * 2, h);
    }
    tx.globalCompositeOperation = "source-over";
    tx.restore();

    tx.restore();   /* clipAbove */
  }

  /* ------------------------------------------ hidden-line drive scene --- */
  function renderTerrain() {
    tx.setTransform(1, 0, 0, 1, 0, 0);
    tx.clearRect(0, 0, W, H);
    envY.fill(1e9);
    envD.fill(0);

    tx.lineCap = "round";
    tx.lineJoin = "round";

    const zSpan = P.zFar - P.zNear;
    const cue = (z) => Math.pow(Math.max(0, (P.zFar - z) / zSpan), 0.9);
    const off = phase % DRIVE.gridZ;

    /* the depth slots the whole scene is ordered by: one per transverse grid
       line, near to far, with the props that live in each slot drawn first */
    const slots = [];
    for (let z = P.zNear + (DRIVE.gridZ - off); z < P.zFar; z += DRIVE.gridZ) slots.push(z);

    /* --- props, keyed to the same phase so the lattice slides as one --- */
    const propAt = [];
    const overhead = [];   /* the gantries: floating geometry, so they are painted last */
    for (let i = 0; i < poles.length; i += 1) {
      const p = poles[i];
      const z = P.zNear + ((p.k * DRIVE.poleZ - phase) % (poles.length * DRIVE.poleZ) + poles.length * DRIVE.poleZ) % (poles.length * DRIVE.poleZ);
      if (z < P.zFar) propAt.push({ z, kind: "pole", p });
    }
    for (let i = 0; i < posts.length; i += 1) {
      const p = posts[i];
      const span = posts.length * DRIVE.postZ;
      const z = P.zNear + (((p.k * DRIVE.postZ - phase) % span) + span) % span;
      if (z < P.zFar) propAt.push({ z, kind: "post", p });
    }
    for (let i = 0; i < gantries.length; i += 1) {
      const g = gantries[i], span = gantries.length * DRIVE.gantryZ;
      const z = P.zNear + (((g.k * DRIVE.gantryZ - phase) % span) + span) % span;
      if (z < P.zFar) overhead.push({ z, p: g });
    }
    propAt.sort((a, b) => a.z - b.z);
    overhead.sort((a, b) => b.z - a.z);

    let pi = 0;
    let prevZ = P.zNear;
    const lonMax = Math.ceil(DRIVE.halfX / DRIVE.gridX);

    for (let s = 0; s < slots.length; s += 1) {
      const z = slots[s];
      while (pi < propAt.length && propAt[pi].z < z) { drawProp(propAt[pi], cue(propAt[pi].z)); pi += 1; }

      const invZ = F / z, invP = F / prevZ;
      const sy = projY(0, invZ);
      const d = cue(z);

      /* longitudinal lines, segment by segment between the two depths, so each
         cell of the grid is clipped by whatever prop stands in front of it */
      tx.lineWidth = Math.max(1, DPR * 0.7);
      tx.beginPath();
      for (let k = -lonMax; k <= lonMax; k += 1) {
        const x = k * DRIVE.gridX;
        if (Math.abs(x) < DRIVE.laneHalf) continue;   /* the road is not gridded */
        const ax = projX(x, invP), ay = projY(0, invP);
        const bx = projX(x, invZ), by = sy;
        if ((ax < -40 && bx < -40) || (ax > W + 40 && bx > W + 40)) continue;
        if (ay < envAt(ax) - 0.5 && by < envAt(bx) - 0.5) { tx.moveTo(ax, ay); tx.lineTo(bx, by); }
      }
      tx.strokeStyle = `rgba(${INK},${(0.03 + 0.42 * d).toFixed(3)})`;
      tx.stroke();

      /* the transverse line: one run per visible stretch, bisected at the edges
         so a run ends on the true silhouette rather than on a sample */
      const span = (W * 0.5 / F) * z * P.spanPad;
      const nS = 84;
      tx.beginPath();
      let open = false, prevVis = false, prevX = 0;
      for (let j = 0; j < nS; j += 1) {
        const wx = camX - span + (2 * span * j) / (nS - 1);
        const sx = projX(wx, invZ);
        const vis = sy < envAt(sx) - 0.5;
        if (vis) {
          if (!open) {
            if (j > 0 && !prevVis) {
              let ax = prevX, bx = sx;
              for (let k = 0; k < 3; k += 1) { const mx = (ax + bx) * 0.5; if (sy < envAt(mx) - 0.5) bx = mx; else ax = mx; }
              tx.moveTo(bx, sy);
            } else tx.moveTo(sx, sy);
            open = true;
          } else tx.lineTo(sx, sy);
        } else if (open) {
          let ax = prevX, bx = sx;
          for (let k = 0; k < 3; k += 1) { const mx = (ax + bx) * 0.5; if (sy < envAt(mx) - 0.5) ax = mx; else bx = mx; }
          tx.lineTo(ax, sy);
          open = false;
        }
        prevVis = vis; prevX = sx;
      }
      tx.lineWidth = Math.max(1, DPR * 0.8);
      tx.strokeStyle = `rgba(${INK},${(0.05 + 0.62 * d).toFixed(3)})`;
      tx.stroke();
      prevZ = z;
    }
    while (pi < propAt.length) { drawProp(propAt[pi], cue(propAt[pi].z)); pi += 1; }

    drawRoad();
    drawSky();
    drawSun();
    /* An overhead structure cannot be expressed in a min-y envelope: rastering the
       beam would raise the silhouette across the whole span and black out the road
       beneath it. So the gantries are painted after the ground, far to near, each
       clipped against the silhouette the ground and the standards already raised —
       and after the sky, so a sign always reads in front of the moon. */
    for (let i = 0; i < overhead.length; i += 1) drawGantry(overhead[i].p, overhead[i].z, cue(overhead[i].z));

    /* the road plane's own silhouette is the far edge of the grid; the props
       stand above it, banded by depth so nothing draws a hard skyline */
    const BANDS = 7;
    tx.lineWidth = Math.max(1, DPR * 0.85);
    for (let b = 0; b < BANDS; b += 1) {
      tx.beginPath();
      let started = false;
      for (let c = 0; c < EW; c += 1) {
        const y = envY[c];
        if (y > H + 2) { started = false; continue; }
        const bb = Math.min(BANDS - 1, ((1 - envD[c]) * BANDS) | 0);
        if (bb !== b) { started = false; continue; }
        if (!started) { tx.moveTo(c * 2, y); started = true; } else tx.lineTo(c * 2, y);
      }
      const a = 0.045 + 0.5 * Math.pow(1 - b / (BANDS - 1), 1.35);
      tx.strokeStyle = `rgba(${INK},${a.toFixed(3)})`;
      tx.stroke();
    }
    line([[0, horizonY], [W, horizonY]], 0.5, Math.max(1, DPR * 0.9));

    /* opaque body behind every stroke, so the plume is occluded by the ground */
    tx.globalCompositeOperation = "destination-over";
    tx.beginPath();
    tx.moveTo(-2, H + 2);
    for (let c = 0; c < EW; c += 1) {
      const e = envY[c];
      const y = e < horizonY ? e : horizonY;
      tx.lineTo(c * 2, y > H + 2 ? H + 2 : y);
    }
    tx.lineTo(W + 2, H + 2);
    tx.closePath();
    tx.fillStyle = BG;
    tx.fill();
    tx.globalCompositeOperation = "source-over";

    tapePass();
  }

  /* ---------------------------------------------------------- tape pass --- */
  const TAPE = { bandPx: 11, wobble: 1.5, tear: 0.055, tearSkew: 20, fringe: 0.3 };
  function tapePass() {
    vx.setTransform(1, 0, 0, 1, 0, 0);
    vx.globalCompositeOperation = "source-over";
    vx.globalAlpha = 1;
    vx.clearRect(0, 0, W, H);   /* the sky is transparent here: the plume sits behind it */

    const t = phase * 0.9;
    const bh = Math.max(2, Math.round(TAPE.bandPx * DPR));
    const bands = Math.ceil(H / bh);
    const tearAt = 1 - TAPE.tear;
    for (let i = 0; i < bands; i += 1) {
      const y = i * bh;
      const v = (y + bh * 0.5) / H;
      /* two slow beats plus a per-band flutter, so no row sits perfectly still */
      let dx = Math.sin(v * 26.0 + t * 2.1) * 0.8 + Math.sin(v * 6.1 - t * 1.3) * 1.5;
      dx += (ihash(i, Math.floor(t * 7)) - 0.5) * 2.0;
      dx *= TAPE.wobble * DPR;
      if (v > tearAt) dx += ((v - tearAt) / TAPE.tear) * TAPE.tearSkew * DPR;
      const h = Math.min(bh + 1, H - y);
      vx.drawImage(work, 0, y, W, h, dx, y, W, h);
    }

    /* colour fringe: the plate laid twice more, warm one way and cool the other */
    if (TAPE.fringe > 0 && typeof vx.filter === "string") {
      const off = 1.6 * DPR;
      vx.globalCompositeOperation = "lighter";
      vx.globalAlpha = TAPE.fringe;
      vx.filter = "sepia(1) saturate(7) hue-rotate(-32deg) brightness(0.62)";
      vx.drawImage(work, off, 0);
      vx.filter = "sepia(1) saturate(6) hue-rotate(150deg) brightness(0.52)";
      vx.drawImage(work, -off, 0);
      vx.filter = "none";
      vx.globalAlpha = 1;
      vx.globalCompositeOperation = "source-over";
    }
  }

  /* lane edges and the dashed centre line, both scrolling on the phase clock */
  function drawRoad() {
    const iN = F / P.zNear, iFar = F / (P.zFar * 0.86);
    for (let side = -1; side <= 1; side += 2) {
      const x = side * DRIVE.laneHalf;
      line([[projX(x, iN), projY(0, iN)], [projX(x, iFar), projY(0, iFar)]], 0.62, Math.max(1, DPR * 1.0));
    }
    const period = DRIVE.dashLen + DRIVE.dashGap;
    const off = phase % period;
    tx.fillStyle = `rgba(${INK},0.5)`;
    for (let z = P.zNear - off; z < P.zFar * 0.8; z += period) {
      const z0 = Math.max(P.zNear, z), z1 = z + DRIVE.dashLen;
      if (z1 <= P.zNear) continue;
      const i0 = F / z0, i1 = F / z1;
      const w0 = 0.055 * i0, w1 = 0.055 * i1;
      const y0 = projY(0, i0), y1 = projY(0, i1);
      const cx0 = projX(0, i0), cx1 = projX(0, i1);
      if (y1 > envAt(cx1) - 0.5 && y0 > envAt(cx0) - 0.5) continue;
      tx.beginPath();
      tx.moveTo(cx0 - w0, y0); tx.lineTo(cx0 + w0, y0);
      tx.lineTo(cx1 + w1, y1); tx.lineTo(cx1 - w1, y1);
      tx.closePath(); tx.fill();
    }
  }

  /* an overhead sign gantry: two legs, a truss beam and a blank panel, all cut
     into the same silhouette so the grid and the sky behind them are occluded */
  function drawGantry(g, z, cue) {
    if (z < DRIVE.gantryNear) return;
    const invZ = F / z;
    const outer = DRIVE.laneHalf + 0.72;
    const top = projY(g.h, invZ), road = projY(0, invZ);
    const lw = Math.max(0.8, 0.021 * invZ);
    const beamH = Math.max(1.3, 0.062 * invZ);
    if (top > H + 6 || road < -40) return;
    const lx = projX(-outer, invZ), rx2 = projX(outer, invZ);
    if (rx2 < -60 || lx > W + 60) return;
    clipAbove(Math.min(lx, 0) - 8, Math.max(rx2, W) + 8);
    tx.lineWidth = Math.max(1, DPR * 0.55);
    const legs = [];
    for (const bx of [lx, rx2]) {
      const leg = [[bx - lw, road], [bx + lw, road], [bx + lw, top], [bx - lw, top]];
      solid(leg, 0.26 + 0.26 * cue);
      legs.push(leg);
    }
    const beam = [[lx - lw, top], [rx2 + lw, top], [rx2 + lw, top - beamH], [lx - lw, top - beamH]];
    solid(beam, 0.3 + 0.3 * cue);
    /* the diagonal truss inside the beam, only once it is worth the strokes */
    if (rx2 - lx > 46) {
      const n = Math.max(4, Math.round((rx2 - lx) / 26));
      for (let i = 0; i < n; i += 1) {
        const a = lx + ((rx2 - lx) * i) / n, b = lx + ((rx2 - lx) * (i + 1)) / n;
        line(i % 2 ? [[a, top], [b, top - beamH]] : [[a, top - beamH], [b, top]], 0.14 + 0.12 * cue, Math.max(1, DPR * 0.35));
      }
    }
    /* the sign panel hanging under the beam, over the carriageway */
    const sw = (DRIVE.laneHalf * 0.58) * invZ, sh = g.sign * 0.62 * invZ;
    const cxp = projX(0, invZ), sy0 = top + beamH * 0.2;
    if (sh > 4) {
      const panel = [[cxp - sw, sy0], [cxp + sw, sy0], [cxp + sw, sy0 + sh], [cxp - sw, sy0 + sh]];
      solid(panel, 0.3 + 0.26 * cue);
      tx.fillStyle = `rgba(${INK},${(0.16 + 0.2 * cue).toFixed(3)})`;
      for (let r = 0; r < 2; r += 1) {
        const bh = sh * 0.17, by = sy0 + sh * (0.24 + r * 0.36);
        tx.fillRect(cxp - sw * 0.68, by, sw * (r ? 0.86 : 1.32), bh);
      }
    }
    tx.restore();   /* clipAbove */
  }

  function drawProp(item, cue) {
    const p = item.p, z = item.z, invZ = F / z;
    const baseX = p.side * p.x;
    const sx = projX(baseX, invZ), sy = projY(0, invZ);
    if (sx < -140 || sx > W + 140) return;

    if (item.kind === "post") {
      const hpx = p.h * invZ;
      if (hpx < 1.6 || sy - hpx > H + 6) return;
      if (sy < envAt(sx) - 3) return;
      const w = Math.max(0.8, 0.014 * invZ);
      const pts = [[sx - w, sy], [sx + w, sy], [sx + w, sy - hpx], [sx - w, sy - hpx]];
      tx.lineWidth = Math.max(1, DPR * 0.5);
      solid(pts, 0.24 + 0.3 * cue);
      tx.fillStyle = `rgba(${INK},${(0.5 + 0.4 * cue).toFixed(3)})`;
      tx.fillRect(sx - w, sy - hpx, w * 2, Math.max(1, hpx * 0.22));
      rasterPoly(pts.concat([pts[0]]), cue);
      return;
    }

    /* light standard: mast, curved arm reaching over the road, lamp head */
    if (z < DRIVE.poleNear) return;
    const hpx = p.h * invZ;
    if (hpx < 5 || sy - hpx > H + 6) return;
    const w = Math.max(0.8, 0.016 * invZ);
    const armPx = p.arm * invZ * -p.side;
    const top = sy - hpx;
    const mast = [[sx - w, sy], [sx + w, sy], [sx + w, top], [sx - w, top]];
    tx.lineWidth = Math.max(1, DPR * 0.55);
    solid(mast, 0.22 + 0.24 * cue);
    const arm = [[sx, top + w], [sx + armPx * 0.55, top - hpx * 0.055], [sx + armPx, top - hpx * 0.075]];
    line(arm, 0.32 + 0.26 * cue, Math.max(1, DPR * 0.6));
    const lx = sx + armPx, ly = top - hpx * 0.075;
    const lw = Math.max(1.4, 0.05 * invZ), lh = Math.max(0.8, 0.018 * invZ);
    tx.fillStyle = `rgba(${INK},${(0.62 + 0.3 * cue).toFixed(3)})`;
    tx.fillRect(lx - lw, ly, lw * 2, lh);
    /* the lamp throws a short cone of light down onto the road surface */
    const cone = tx.createLinearGradient(0, ly, 0, sy);
    cone.addColorStop(0, `rgba(${INK},${(0.10 * cue).toFixed(3)})`);
    cone.addColorStop(1, `rgba(${INK},0)`);
    tx.fillStyle = cone;
    tx.beginPath();
    tx.moveTo(lx - lw, ly + lh);
    tx.lineTo(lx + lw, ly + lh);
    tx.lineTo(lx + lw * 3.2, sy);
    tx.lineTo(lx - lw * 3.2, sy);
    tx.closePath(); tx.fill();
    rasterPoly(mast.concat([mast[0]]), cue);
    rasterPoly([[lx - lw, ly], [lx + lw, ly]], cue);
  }

  function drawReveal(p) {
    rx.setTransform(1, 0, 0, 1, 0, 0);
    rx.globalCompositeOperation = "source-over";
    rx.globalAlpha = 1;
    rx.fillStyle = BG;
    rx.fillRect(0, 0, RW, RH);
    if (p <= 0) return;

    const edge = p * (1 + BAND + SOFT);
    rx.globalCompositeOperation = "destination-out";

    /* everything well behind the front is simply gone — one rect instead of 20k */
    const solidPx = (edge - BAND - SOFT) * RW;
    if (solidPx > 0) rx.fillRect(0, 0, Math.min(RW, solidPx), RH);

    const cFrom = Math.max(0, Math.floor(solidPx / dCell));
    const cTo = Math.min(dCols - 1, Math.ceil((edge * RW) / dCell));
    for (let cy = 0; cy < dRows; cy += 1) {
      const row = cy * dCols;
      for (let cx = cFrom; cx <= cTo; cx += 1) {
        const t = (cx * dCell + dCell * 0.5) / RW;
        let a = (edge - BAND * dither[row + cx] - t) / SOFT;
        if (a <= 0) continue;
        if (a > 1) a = 1;
        else a = Math.ceil(a * 3) / 3;   /* quantised → dithered edge */
        rx.globalAlpha = a;
        rx.fillRect(cx * dCell, cy * dCell, dCell + 0.5, dCell + 0.5);
      }
    }
    rx.globalAlpha = 1;
    rx.globalCompositeOperation = "source-over";
  }

  /* ------------------------------------------------------------ loop ---- */
  const DUR = 2050;
  let revealT = 0, playing = false, done = false, armed = true, inView = false;
  let last = 0, drift = 0, lastFrameAt = 0, rafId = 0;
  const easeOut = (t) => 1 - Math.pow(1 - t, 2.6);

  const DRIFT_HOLD = PARALLAX ? 6000 : 0;
  let idleT = 0, driftAmp = PARALLAX ? 1 : 0;

  function frame(now) {
    const dt = last ? Math.min(48, now - last) : 16;
    last = now;

    if (inView) {
      if (now - lastFrameAt >= 15.5) {   /* cap terrain work at ~60fps */
        lastFrameAt = now;
        idleT += dt;
        const want = idleT > DRIFT_HOLD ? 0 : 1;
        driftAmp += (want - driftAmp) * 0.02;
        if (driftAmp > 0.002) drift += dt * 0.00016;

        const dx = Math.sin(drift) * 0.055 * driftAmp;
        const dy = Math.cos(drift * 0.73) * 0.014 * driftAmp;
        camX += (tgtX + dx - camX) * 0.055;
        camY += (tgtY + dy - camY) * 0.055;

        /* the drive never comes to rest, so unlike the still scenes the phase
           runs off wall-clock time and the frame is always worth drawing */
        phase += (dt / 1000) * DRIVE.speed;
        horizonY = OY + camY * (F / P.zFar);
        renderTerrain();
      }

      if (playing) {
        revealT += dt;
        const p = Math.min(1, revealT / DUR);
        drawReveal(easeOut(p));
        if (p >= 1) { playing = false; done = true; rcv.style.display = "none"; }
      }
    }

    rafId = requestAnimationFrame(frame);
  }

  function start() {
    if (!armed) return;
    armed = false; done = false; playing = true; revealT = 0;
    rcv.style.display = ""; drawReveal(0);
  }

  /* ----------------------------------------------------------- input ---- */
  const onPointerMove = (event) => {
    const nx = (event.clientX / window.innerWidth) * 2 - 1;
    const ny = (event.clientY / window.innerHeight) * 2 - 1;
    tgtX = -nx * 0.34;
    tgtY = P.camY + ny * 0.052;
    idleT = 0;   /* wake the ambient drift back up */
  };

  let resizeTimer = 0;
  const onResize = () => {
    window.clearTimeout(resizeTimer);
    resizeTimer = window.setTimeout(() => {
      resize(); renderTerrain();
      if (!done) drawReveal(playing ? easeOut(Math.min(1, revealT / DUR)) : 0);
    }, 120);
  };

  /* ------------------------------------------------------------ boot ---- */
  resize();
  let viewObserver = null, startObserver = null;

  if (reduced) {
    renderTerrain();
    rcv.style.display = "none";
  } else {
    if (PARALLAX) window.addEventListener("pointermove", onPointerMove, { passive: true });
    drawReveal(0);
    renderTerrain();

    viewObserver = new IntersectionObserver((entries) => {
      for (const entry of entries) {
        inView = entry.isIntersecting;
        if (!entry.isIntersecting && REPLAY) {
          armed = true; playing = false; done = false; revealT = 0;
          rcv.style.display = ""; drawReveal(0);
        }
      }
    }, { threshold: 0 });
    viewObserver.observe(root);

    startObserver = new IntersectionObserver((entries) => {
      for (const entry of entries) if (entry.isIntersecting) start();
    }, { threshold: 0, rootMargin: "0px 0px -33% 0px" });
    startObserver.observe(root);

    rafId = requestAnimationFrame(frame);
  }

  const boxObserver = new ResizeObserver(onResize);
  boxObserver.observe(root);
  window.addEventListener("resize", onResize);

  return function destroy() {
    window.clearTimeout(resizeTimer);
    boxObserver.disconnect();
    window.removeEventListener("resize", onResize);
    window.removeEventListener("pointermove", onPointerMove);
    if (rafId) cancelAnimationFrame(rafId);
    if (viewObserver) viewObserver.disconnect();
    if (startObserver) startObserver.disconnect();
  };
}

/* ------------------------------------------------------ the plume host ---- */
function mountTerrainPlume(host, props) {
  const o = Object.assign({}, TERRAIN_PLUME_DEFAULTS, props || {});
  const variant = o.variant || "alpine";
  const options = {
    speed: clamp(o.speed, 0, 3),
    size: clamp(o.size, 0.55, 1.8),
    thickness: clamp(o.thickness, 0.35, 2),
    strength: clamp(o.strength, 0, 2),
    softness: clamp(o.softness, 0, 12),
    opacity: clamp(o.opacity, 0.05, 1),
    hue: clamp(o.hue, -180, 180),
    saturation: clamp(o.saturation, 0, 2),
    brightness: clamp(o.brightness, 0.35, 1.65),
  };
  const scene = variant in SCENE_PLUME_TINT ? variant : "alpine";

  const mount = document.createElement("div");
  mount.className = ["terrain-plume-component", o.className].filter(Boolean).join(" ");
  mount.dataset.terrainScene = scene;
  mount.setAttribute("aria-hidden", "true");
  const filter = options.hue === 0 && options.saturation === 1 && options.brightness === 1
    ? "" : `hue-rotate(${options.hue}deg) saturate(${options.saturation}) brightness(${options.brightness})`;
  mount.style.opacity = String(options.opacity);
  if (filter) mount.style.filter = filter;

  const canvas = document.createElement("canvas");
  canvas.className = "terrain-plume-canvas";
  const plumeScaleX = options.size * options.thickness;
  const plumeFilters = [
    SCENE_PLUME_TINT[scene] || "",
    options.strength > 1 ? `brightness(${options.strength})` : "",
    options.softness > 0 ? `blur(${options.softness}px)` : "",
  ].filter(Boolean).join(" ");
  canvas.style.opacity = String(Math.min(options.strength, 1));
  if (!(plumeScaleX === 1 && options.size === 1)) canvas.style.transform = `scale3d(${plumeScaleX}, ${options.size}, 1)`;
  canvas.style.transformOrigin = "50% 100%";
  if (plumeFilters) canvas.style.filter = plumeFilters;
  mount.appendChild(canvas);

  const terrain = document.createElement("canvas");
  terrain.className = "footer-engraved-terrain";
  terrain.setAttribute("aria-hidden", "true");
  const reveal = document.createElement("canvas");
  reveal.className = "footer-engraved-reveal";
  reveal.setAttribute("aria-hidden", "true");
  const grain = document.createElement("div");
  grain.className = "footer-engraved-grain";
  grain.setAttribute("aria-hidden", "true");
  mount.appendChild(terrain); mount.appendChild(reveal); mount.appendChild(grain);
  host.appendChild(mount);

  /* ---- raw WebGL plume ---- */
  const gl = canvas.getContext("webgl", { alpha: true, antialias: true, depth: false, stencil: false, powerPreference: "low-power" });
  let disposeTerrain = null;
  if (!gl) {
    mount.dataset.webglState = "unavailable";
  } else {
    let program;
    try { program = createProgram(gl); }
    catch (error) { mount.dataset.webglState = "error"; console.warn(error); program = null; }

    if (program) {
      const vertices = new Float32Array([
        -1, -1, 0, 0, 0,
         1, -1, 0, 1, 0,
        -1,  1, 0, 0, 1,
        -1,  1, 0, 0, 1,
         1, -1, 0, 1, 0,
         1,  1, 0, 1, 1,
      ]);
      const buffer = gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
      gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.STATIC_DRAW);
      const position = gl.getAttribLocation(program, "position");
      const uv = gl.getAttribLocation(program, "uv");
      gl.enableVertexAttribArray(position);
      gl.vertexAttribPointer(position, 3, gl.FLOAT, false, 20, 0);
      gl.enableVertexAttribArray(uv);
      gl.vertexAttribPointer(uv, 2, gl.FLOAT, false, 20, 12);

      const timeUniform = gl.getUniformLocation(program, "u_time");
      const resolutionUniform = gl.getUniformLocation(program, "u_resolution");
      const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
      const startedAt = performance.now();
      let rafId = 0, isVisible = true;

      gl.useProgram(program);
      gl.enable(gl.BLEND);
      gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      gl.clearColor(0, 0, 0, 0);

      const resize = () => {
        const rect = mount.getBoundingClientRect();
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        const width = Math.max(1, Math.round(rect.width * dpr));
        const height = Math.max(1, Math.round(rect.height * dpr));
        if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
        gl.viewport(0, 0, width, height);
        gl.uniform2f(resolutionUniform, width, height);
      };
      const render = (now = performance.now()) => {
        resize();
        gl.clear(gl.COLOR_BUFFER_BIT);
        gl.uniform1f(timeUniform, ((now - startedAt) / 1000) * options.speed);
        gl.drawArrays(gl.TRIANGLES, 0, 6);
      };
      const stop = () => { if (rafId) cancelAnimationFrame(rafId); rafId = 0; canvas.dataset.animationActive = "false"; };
      const tick = (now) => { render(now); rafId = requestAnimationFrame(tick); };
      const syncPlayback = () => {
        const shouldAnimate = isVisible && !document.hidden && !reducedMotion.matches;
        if (!shouldAnimate) { stop(); render(); return; }
        if (rafId) return;
        canvas.dataset.animationActive = "true";
        rafId = requestAnimationFrame(tick);
      };
      const visibilityObserver = new IntersectionObserver((entries) => {
        isVisible = entries.some((entry) => entry.isIntersecting);
        syncPlayback();
      }, { threshold: 0.08 });
      const resizeObserver = new ResizeObserver(() => render());
      canvas.addEventListener("webglcontextlost", (event) => { event.preventDefault(); stop(); mount.dataset.webglState = "lost"; });
      const rect = mount.getBoundingClientRect();
      isVisible = rect.bottom > 0 && rect.top < window.innerHeight;
      visibilityObserver.observe(mount);
      resizeObserver.observe(mount);
      document.addEventListener("visibilitychange", syncPlayback);
      reducedMotion.addEventListener && reducedMotion.addEventListener("change", syncPlayback);
      canvas.dataset.animationActive = "false";
      render();
      syncPlayback();
    }
  }

  /* the engraved scene above the plume */
  disposeTerrain = createDriveTerrain(mount, terrain, reveal, options);
  return { mount, destroy() { if (disposeTerrain) disposeTerrain(); mount.remove(); } };
}

window.ThreeUITerrainPlume = { mountTerrainPlume, TERRAIN_PLUME_DEFAULTS, SCENE_PLUME_TINT, VERTEX_SHADER, FRAGMENT_SHADER };
})();

