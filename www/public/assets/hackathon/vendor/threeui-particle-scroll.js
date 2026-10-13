
/* Canvas UI "Particle Scroll" (React, WebGL) — vanilla port of the standalone ParticleScroll.tsx.
   Shaders, row-progress model, scroll smoothing, lag and intro logic are the authored code (types
   stripped). Two adaptations: (1) the HTML-in-canvas capture (drawElementImage/layoutsubtree, Chrome
   flag only) is replaced by a rasteriser that paints the page's tracked text and images into the
   source canvas at their live positions; (2) scattered grains are tinted with a theme gradient and
   the live DOM blurs in once a block has fully reassembled. */
(function () {
/* once a row is this far into the band it is committed and the floor speed takes over */
const COMMIT = 0.4;
const DEFAULTS = { point: 0.68, band: 260, density: 2, size: 1.25, spread: 220, gravity: 0.35, drift: 0.7, swirl: 60, stagger: 0.55, fade: 0.85, settle: 0.7, hold: 1.15, smoothing: 0.26, tintA: [1.0, 0.3, 0.0], tintB: [1.0, 0.82, 0.2] };
const HASH = `
float hash (vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}`;
const QUAD_VERT = `#version 300 es
precision highp float;
layout(location = 0) in vec2 aPos;
out vec2 vUv;
void main () {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;
const BASE_FRAG = `#version 300 es
precision highp float;
in vec2 vUv;
out vec4 outColor;
uniform sampler2D uContent;
uniform sampler2D uRowTex;
uniform vec2 uRes;
uniform float uDensity;
uniform float uRowCount;
uniform float uStagger;
uniform float uMaxX;
uniform float uCover;
uniform float uScroll;
uniform float uWinStart;
uniform vec3 uBg;
${HASH}
void main () {
  vec2 px = vec2(vUv.x, 1.0 - vUv.y) * uRes;
  vec2 cell = floor(vec2(px.x, px.y + uScroll) / uDensity);
  float h1 = hash(cell);
  float d = h1 * uStagger;
  int row = int(clamp(cell.y - uWinStart, 0.0, uRowCount - 1.0));
  float p = texelFetch(uRowTex, ivec2(row, 0), 0).r;
  float t = clamp((p - d) / max(1.0 - d, 1e-3), 0.0, 1.0);
  float vis = step(0.9995, t) * step(px.x, uMaxX * uRes.x);
  vec4 tex = texture(uContent, vec2(vUv.x, 1.0 - vUv.y));
  outColor = vec4(mix(uBg, tex.rgb, vis * tex.a), max(uCover, vis * tex.a));
}`;
const POINT_VERT = `#version 300 es
precision highp float;
uniform sampler2D uRowTex;
uniform vec2 uRes;
uniform vec2 uGrid;
uniform float uDensity;
uniform float uStagger;
uniform float uSpread;
uniform float uGravity;
uniform float uDrift;
uniform float uSwirl;
uniform float uTime;
uniform float uFade;
uniform float uSize;
uniform float uDpr;
uniform float uMaxX;
uniform float uLag;
uniform float uScroll;
uniform float uWinStart;
out vec2 vCenter;
out float vSize;
out float vAlpha;
out float vLod;
out float vMerge;
out float vHeat;
out float vHash;
${HASH}
void main () {
  float fid = float(gl_VertexID);
  vec2 local = vec2(mod(fid, uGrid.x), floor(fid / uGrid.x));
  vec2 cell = vec2(local.x, local.y + uWinStart);
  float h1 = hash(cell);
  float h2 = hash(cell + vec2(1.7, 9.1));
  float h3 = hash(cell + vec2(5.5, 2.9));
  float h4 = hash(cell + vec2(8.4, 4.2));
  float d = h1 * uStagger;
  vec2 home = vec2(
    (cell.x + 0.5) * uDensity,
    (cell.y + 0.5) * uDensity - uScroll
  );
  int row = int(clamp(local.y, 0.0, uGrid.y - 1.0));
  float p = texelFetch(uRowTex, ivec2(row, 0), 0).r;
  float t = clamp((p - d) / max(1.0 - d, 1e-3), 0.0, 1.0);
  float e = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);   // smootherstep: velocity ramps up (acceleration) then snaps home
  float vis = (1.0 - step(0.9995, t))
    * step(home.x, uMaxX * uRes.x)
    * step(home.y, uRes.y)
    * step(-uDensity, home.y);
  if (vis < 0.5) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    gl_PointSize = 0.0;
    vCenter = vec2(0.0);
    vSize = 0.0;
    vAlpha = 0.0;
    vLod = 0.0;
    vMerge = 0.0;
    vHeat = 0.0;
    vHash = 0.0;
    return;
  }
  vec2 dir = normalize(vec2(h2 - 0.5, h3 - 0.5) + vec2(1e-4, 0.0));
  float reach = 0.08 + 0.92 * pow(h4, 2.4);
  vec2 off = dir * uSpread * reach;
  off.y += uGravity * uSpread * (0.25 + 0.75 * h4);
  vec2 scat = home + off;
  vec2 pos = mix(scat, home, e);
  vec2 perp = vec2(-dir.y, dir.x);
  pos += perp * (h2 - 0.5) * 2.0 * uSwirl * sin(e * 3.14159);
  float tt = uTime * uDrift;
  float amp = (1.0 - e) * (uSpread * 0.05 + 2.5);
  pos += vec2(
    sin(tt * (4.0 + 5.0 * h2) + h3 * 40.0),
    cos(tt * (3.5 + 5.5 * h3) + h2 * 40.0)
  ) * amp;
  pos.y += uLag * (1.0 - e) * (0.5 + 0.5 * h4);
  pos += vec2(h4 - 0.5, h1 - 0.5) * uDensity * 3.0
    * (1.0 - smoothstep(0.5, 0.85, t));
  float grow = smoothstep(0.55, 1.0, e);
  float sizeCss = mix(uSize, uDensity * 1.3, grow);
  vCenter = home;
  vSize = sizeCss;
  vAlpha = mix(uFade, 1.0, e);
  vLod = (1.0 - e) * 1.5;
  vMerge = smoothstep(0.75, 0.97, t);
  vHeat = 1.0 - e;
  vHash = h3;
  gl_Position = vec4(
    pos.x / uRes.x * 2.0 - 1.0,
    1.0 - pos.y / uRes.y * 2.0,
    0.0,
    1.0
  );
  gl_PointSize = max(sizeCss * uDpr, 1.0);
}`;
const POINT_FRAG = `#version 300 es
precision highp float;
uniform sampler2D uContent;
uniform vec2 uRes;
uniform vec3 uTintA;
uniform vec3 uTintB;
in vec2 vCenter;
in float vSize;
in float vAlpha;
in float vLod;
in float vMerge;
in float vHeat;
in float vHash;
out vec4 outColor;
void main () {
  vec2 o = gl_PointCoord - 0.5;
  vec2 uv = clamp((vCenter + o * vSize) / uRes, 0.0, 1.0);
  vec4 tex = textureLod(uContent, uv, vLod);
  float circle = 1.0 - smoothstep(0.25, 0.5, length(o));
  float mask = mix(circle, 1.0, vMerge);
  float a = vAlpha * mask * tex.a;
  if (a < 0.01) discard;
  vec3 tint = mix(uTintA, uTintB, vHash);
  float lum = dot(tex.rgb, vec3(0.299, 0.587, 0.114));
  vec3 rgb = mix(tex.rgb, tint * (0.8 + 0.6 * lum), smoothstep(0.0, 0.9, vHeat));
  outColor = vec4(rgb, a);
}`;

function createParticleScroll(elements, options = {}) {
  const config = { ...DEFAULTS, ...options };
  const { source, content, output, paint, onRows, scrollTarget } = elements;
  const gl = output.getContext("webgl2", { alpha: true, depth: false, stencil: false, antialias: false, premultipliedAlpha: false });
  if (!gl || gl.isContextLost()) return null;
  const sourceCtx = source.getContext("2d");
  const htmlInCanvas = true;   // content is supplied by the page rasteriser
  let contentDirty = false;
  let wake = () => {};
  const requestPaint = () => { try { paint(source, sourceCtx); contentDirty = true; wake(); } catch (e) { console.warn(e); } };

  function compile(type, text) { const shader = gl.createShader(type); gl.shaderSource(shader, text); gl.compileShader(shader); if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) console.error("ParticleScroll shader error:", gl.getShaderInfoLog(shader)); return shader; }
  function link(vertText, fragText) { const vert = compile(gl.VERTEX_SHADER, vertText); const frag = compile(gl.FRAGMENT_SHADER, fragText); const program = gl.createProgram(); gl.attachShader(program, vert); gl.attachShader(program, frag); gl.linkProgram(program); const uniforms = {}; const count = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS); for (let i = 0; i < count; i++) { const info = gl.getActiveUniform(program, i); uniforms[info.name] = gl.getUniformLocation(program, info.name); } return { program, vert, frag, uniforms }; }
  const base = link(QUAD_VERT, BASE_FRAG);
  const points = link(POINT_VERT, POINT_FRAG);
  const quadVao = gl.createVertexArray(); gl.bindVertexArray(quadVao);
  const quad = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, quad); gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW); gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  const pointVao = gl.createVertexArray();
  const contentTexture = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, contentTexture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE, new Uint8Array([0, 0, 0, 0])); gl.generateMipmap(gl.TEXTURE_2D);
  let contentMaxX = 1;
  const rowTex = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, rowTex);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  let rowProgress = new Float32Array(0), rowWindow = new Float32Array(0), rowsAnimating = false, rowsAssembled = false;
  let units = [];   // [{from, to, anchor}] document row ranges that resolve as one block
  let bg = [0, 0, 0];
  function syncCanvasSize() {
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const width = Math.max(1, Math.round(output.clientWidth * dpr)), height = Math.max(1, Math.round(output.clientHeight * dpr));
    if (output.width !== width || output.height !== height) { output.width = width; output.height = height; }
    contentMaxX = Math.min(1, Math.max(0.05, content.clientWidth / Math.max(output.clientWidth, 1)));
    const cssWidth = Math.max(1, Math.round(output.clientWidth)), cssHeight = Math.max(1, Math.round(output.clientHeight));
    if (source.width !== cssWidth * dpr || source.height !== cssHeight * dpr) { source.width = cssWidth * dpr; source.height = cssHeight * dpr; }
    requestPaint();
  }
  const motionQuery = window.matchMedia("(prefers-reduced-motion: reduce)"); let reducedMotion = motionQuery.matches;
  let time = 0, introDone = false, introWait = 0, introReady = false, scrollSmooth = content.scrollTop;
  syncCanvasSize();
  function uploadContent() { if (!contentDirty) return; contentDirty = false; introReady = true; gl.bindTexture(gl.TEXTURE_2D, contentTexture); gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source); gl.generateMipmap(gl.TEXTURE_2D); }
  function rowTargetFor(docRowY) {
    if (reducedMotion || !introDone) return 1;
    const h = Math.max(output.clientHeight, 1), band = Math.max(config.band, 1), max = content.scrollHeight - content.clientHeight;
    let line = Math.min(Math.max(config.point, 0), 1) * h;
    if (max <= 1) line = h + band; else { const endP = Math.min(Math.max((scrollSmooth - (max - h * 0.5)) / (h * 0.5), 0), 1); line += (h + band - line) * endP * endP; }
    const vy = docRowY - scrollSmooth;
    return Math.min(Math.max((line + band - vy) / band, 0), 1);
  }
  function unitAt(i) { for (let u = 0; u < units.length; u++) { const r = units[u]; if (i >= r.from && i <= r.to) return r; } return null; }
  function targetForRow(i, density) { const u = unitAt(i); return rowTargetFor(u ? u.anchor : (i + 0.5) * density); }
  function updateRows(dt, density, winStart, winLen) {
    const docRows = Math.max(1, Math.ceil(content.scrollHeight / density));
    if (rowProgress.length !== docRows) { const next = new Float32Array(docRows); for (let i = 0; i < docRows; i++) next[i] = targetForRow(i, density); rowProgress = next; }
    if (rowWindow.length !== winLen) rowWindow = new Float32Array(winLen);
    rowsAnimating = false; let minP = 1;
    const settle = Math.max(config.settle, 0.05);            // ceiling: the full range never takes less than this
    const hold = Math.max(config.hold || settle, settle);    // floor: a committed row still finishes within this
    for (let i = 0; i < docRows; i++) {
      const target = targetForRow(i, density); let p = rowProgress[i]; const inWin = i >= winStart - 4 && i < winStart + winLen + 4;
      if (reducedMotion || !inWin) { if (p !== target) { rowProgress[i] = target; p = target; } }
      else {
        const cap = dt / settle;
        if (target >= COMMIT) {
          // Past the commit point the row forms at its own pace whether the scroll
          // continues, crawls or stops, so a flick and a slow drag look the same.
          const next = Math.min(1, Math.max(Math.min(target, p + cap), p + dt / hold));
          if (next > p) { p = next; rowsAnimating = true; rowProgress[i] = p; }
        } else if (p < target) {
          const next = Math.min(p + cap, target);
          if (next > p) { p = next; rowsAnimating = true; rowProgress[i] = p; }
        } else if (p > target) {
          const next = Math.max(p - dt / (settle * 0.6), target);
          if (next < p) { p = next; rowsAnimating = true; rowProgress[i] = p; }
        }
      }
      if (inWin && p < minP) minP = p;
    }
    rowsAssembled = minP >= 0.9995;
    rowWindow.fill(1); const from = Math.min(Math.max(winStart, 0), docRows), to = Math.min(winStart + winLen, docRows);
    if (to > from) rowWindow.set(rowProgress.subarray(from, to), from - winStart);
    gl.bindTexture(gl.TEXTURE_2D, rowTex); gl.texImage2D(gl.TEXTURE_2D, 0, gl.R32F, winLen, 1, 0, gl.RED, gl.FLOAT, rowWindow);
    if (onRows) onRows(rowProgress, density, winStart, winLen);
  }
  function render(dt) {
    uploadContent();
    const w = Math.max(output.clientWidth, 1), h = Math.max(output.clientHeight, 1), dpr = output.width / w;
    const density = Math.max(Math.max(config.density, 1), Math.sqrt((w * h) / 800000));
    const scrollTop = content.scrollTop, gridX = Math.ceil(w / density), winStart = Math.floor(scrollTop / density), winLen = Math.ceil(h / density) + 2, stagger = Math.min(Math.max(config.stagger, 0), 0.95);
    updateRows(dt, density, winStart, winLen);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null); gl.viewport(0, 0, output.width, output.height);
    gl.clearColor(0, 0, 0, 0); gl.clear(gl.COLOR_BUFFER_BIT);
    gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, rowTex); gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, contentTexture);
    gl.enable(gl.BLEND); gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    gl.useProgram(base.program); gl.bindVertexArray(quadVao);
    gl.uniform1i(base.uniforms.uContent, 0); gl.uniform1i(base.uniforms.uRowTex, 1); gl.uniform2f(base.uniforms.uRes, w, h); gl.uniform1f(base.uniforms.uDensity, density); gl.uniform1f(base.uniforms.uRowCount, winLen); gl.uniform1f(base.uniforms.uStagger, stagger); gl.uniform1f(base.uniforms.uMaxX, contentMaxX); gl.uniform1f(base.uniforms.uCover, 0); gl.uniform1f(base.uniforms.uScroll, scrollTop); gl.uniform1f(base.uniforms.uWinStart, winStart); gl.uniform3f(base.uniforms.uBg, bg[0], bg[1], bg[2]);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    if (rowsAssembled) { gl.disable(gl.BLEND); return; }
    gl.useProgram(points.program); gl.bindVertexArray(pointVao);
    gl.uniform1i(points.uniforms.uRowTex, 1); gl.uniform2f(points.uniforms.uRes, w, h); gl.uniform2f(points.uniforms.uGrid, gridX, winLen); gl.uniform1f(points.uniforms.uDensity, density); gl.uniform1f(points.uniforms.uStagger, stagger); gl.uniform1f(points.uniforms.uSpread, Math.max(config.spread, 0)); gl.uniform1f(points.uniforms.uGravity, Math.min(Math.max(config.gravity, -1), 1)); gl.uniform1f(points.uniforms.uDrift, Math.max(config.drift, 0)); gl.uniform1f(points.uniforms.uSwirl, Math.max(config.swirl, 0)); gl.uniform1f(points.uniforms.uTime, time); gl.uniform1f(points.uniforms.uFade, Math.min(Math.max(config.fade, 0), 1)); gl.uniform1f(points.uniforms.uSize, Math.max(config.size, 0.5)); gl.uniform1f(points.uniforms.uDpr, dpr); gl.uniform1f(points.uniforms.uMaxX, contentMaxX); gl.uniform1i(points.uniforms.uContent, 0); gl.uniform1f(points.uniforms.uLag, lag); gl.uniform1f(points.uniforms.uScroll, scrollTop); gl.uniform1f(points.uniforms.uWinStart, winStart);
    gl.uniform3f(points.uniforms.uTintA, ...config.tintA); gl.uniform3f(points.uniforms.uTintB, ...config.tintB);
    gl.drawArrays(gl.POINTS, 0, gridX * winLen);
    gl.bindVertexArray(quadVao); gl.disable(gl.BLEND);
  }
  let raf = 0, lastTime = performance.now(), destroyed = false, running = false, visible = true, lag = 0, lastScrollTop = content.scrollTop;
  function frame(now) {
    if (destroyed) return; if (!visible) { running = false; return; }
    const delta = Math.min((now - lastTime) / 1000, 1 / 30); lastTime = now; time += delta;
    const scrollTop = content.scrollTop; lag += scrollTop - lastScrollTop; lastScrollTop = scrollTop; lag *= Math.exp(-delta / 0.22); lag = Math.min(Math.max(lag, -400), 400); if (reducedMotion || Math.abs(lag) < 0.1) lag = 0;
    if (!introDone) { if (reducedMotion) introDone = true; else if (introReady) { introWait += delta; if (introWait >= 1) introDone = true; } }
    const tau = config.smoothing, k = reducedMotion || tau <= 0 ? 1 : 1 - Math.exp(-delta / Math.max(tau, 1e-4));
    scrollSmooth += (scrollTop - scrollSmooth) * k; if (Math.abs(scrollTop - scrollSmooth) < 0.5) scrollSmooth = scrollTop;
    render(delta);
    if (!contentDirty && scrollSmooth === scrollTop && !rowsAnimating && rowsAssembled && introDone && lag === 0) { running = false; return; }
    raf = requestAnimationFrame(frame);
  }
  function start() { if (destroyed || running || !visible) return; running = true; lastTime = performance.now(); raf = requestAnimationFrame(frame); }
  wake = start; start();
  function onScroll() { requestPaint(); start(); }
  (scrollTarget || content).addEventListener("scroll", onScroll, { passive: true });
  function onMotionChange() { reducedMotion = motionQuery.matches; start(); }
  motionQuery.addEventListener("change", onMotionChange);
  const observer = new ResizeObserver(() => { syncCanvasSize(); start(); }); observer.observe(output);
  return {
    setOptions(next) { if (!Object.entries(next).some(([key, value]) => config[key] !== value)) return; Object.assign(config, next); start(); },
    resize() { syncCanvasSize(); start(); },
    setUnits(next) { units = next || []; start(); },
    repaint() { requestPaint(); start(); },
    destroy() { destroyed = true; cancelAnimationFrame(raf); (scrollTarget || content).removeEventListener("scroll", onScroll); observer.disconnect(); motionQuery.removeEventListener("change", onMotionChange); gl.deleteTexture(contentTexture); gl.deleteTexture(rowTex); gl.deleteProgram(base.program); gl.deleteProgram(points.program); gl.deleteShader(base.vert); gl.deleteShader(base.frag); gl.deleteShader(points.vert); gl.deleteShader(points.frag); gl.deleteBuffer(quad); gl.deleteVertexArray(quadVao); gl.deleteVertexArray(pointVao); },
  };
}

/* ---- page rasteriser: paints tracked text and images at their live viewport positions ---- */
function rasterise(tracked, source, ctx) {
  const dpr = source.width / Math.max(1, source.clientWidth || window.innerWidth);
  ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.clearRect(0, 0, source.width, source.height); ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const vh = window.innerHeight, margin = 600;
  for (const t of tracked) {
    if (t.assembled) continue;
    const box = t.el.getBoundingClientRect(); if (box.bottom < -margin || box.top > vh + margin) continue;
    t.el.classList.add('ps-hidden');
    paintElement(t.el, ctx);
  }
}
function paintElement(root, ctx) {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT);
  let node = root;
  const paintBox = (el) => { const cs = getComputedStyle(el); const bg = cs.backgroundColor; if (bg && bg !== 'rgba(0, 0, 0, 0)' && bg !== 'transparent') { const r = el.getBoundingClientRect(); ctx.fillStyle = bg; ctx.fillRect(r.left, r.top, r.width, r.height); } };
  const paintImg = (img) => { if (!img.complete || !img.naturalWidth) return; const r = img.getBoundingClientRect(); const fit = getComputedStyle(img).objectFit; if (fit === 'cover') { const s = Math.max(r.width / img.naturalWidth, r.height / img.naturalHeight); const sw = r.width / s, sh = r.height / s; ctx.drawImage(img, (img.naturalWidth - sw) / 2, (img.naturalHeight - sh) / 2, sw, sh, r.left, r.top, r.width, r.height); } else ctx.drawImage(img, r.left, r.top, r.width, r.height); };
  const paintText = (tn) => { const text = tn.nodeValue; if (!text.trim()) return; const parent = tn.parentElement; const cs = getComputedStyle(parent); ctx.font = `${cs.fontStyle} ${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`; ctx.fillStyle = cs.color; ctx.textBaseline = 'alphabetic'; try { ctx.letterSpacing = cs.letterSpacing === 'normal' ? '0px' : cs.letterSpacing; } catch (e) {}
    const re = /\S+/g; let m; const range = document.createRange(); const fs = parseFloat(cs.fontSize);
    const met = ctx.measureText('Hg'); const asc = met.fontBoundingBoxAscent || fs * 0.8, desc = met.fontBoundingBoxDescent || fs * 0.2;
    while ((m = re.exec(text))) { range.setStart(tn, m.index); range.setEnd(tn, m.index + m[0].length); const rects = range.getClientRects(); if (!rects.length) continue; const r = rects[0]; if (r.width === 0) continue; ctx.fillText(m[0], r.left, r.top + (r.height - (asc + desc)) / 2 + asc); } };
  if (root.tagName === 'IMG') { paintImg(root); return; }
  paintBox(root);
  while ((node = walker.nextNode())) {
    if (node.nodeType === 3) paintText(node);
    else if (node.tagName === 'IMG') paintImg(node);
    else if (node.tagName !== 'SVG' && node.tagName !== 'svg') paintBox(node);
  }
}

function mountParticleScroll(selectorList, options = {}) {
  const output = document.createElement('canvas'); output.id = 'ps-output'; output.setAttribute('aria-hidden', 'true'); document.body.appendChild(output);
  const source = document.createElement('canvas');
  const tracked = [...document.querySelectorAll(selectorList)].map(el => ({ el, assembled: false }));
  const scroller = document.scrollingElement || document.documentElement; let inst = null;
  const content = { get scrollTop() { return scroller.scrollTop; }, get scrollHeight() { return scroller.scrollHeight; }, get clientHeight() { return window.innerHeight; }, get clientWidth() { return window.innerWidth; } };
  const instance = createParticleScroll({ source, content, output, scrollTarget: window, paint: (src, ctx) => rasterise(tracked, src, ctx), onRows(rows, density) {
    const sy = scroller.scrollTop; let changed = false;
    for (const t of tracked) {
      const r = t.el.getBoundingClientRect(); const top = Math.max(0, Math.floor((r.top + sy) / density)), bottom = Math.min(rows.length - 1, Math.ceil((r.bottom + sy) / density));
      let min = 1; for (let i = top; i <= bottom; i++) if (rows[i] < min) min = rows[i];
      const done = min >= 0.9995;
      if (done && !t.assembled) { t.assembled = true; t.el.classList.remove('ps-hidden'); t.el.classList.add('ps-in'); changed = true; }
      else if (!done && t.assembled && min < 0.6) { t.assembled = false; t.el.classList.remove('ps-in'); changed = true; }
    }
    if (changed && inst) inst.repaint();
  } }, options);
  inst = instance;
  if (!instance) { output.remove(); return null; }
  // Each tracked block is one unit: every row it spans shares a single progress value, so the
  // block dissolves and re-forms as a whole instead of being wiped by a hard horizontal edge.
  const density = Math.max(1, options.density || DEFAULTS.density);
  const syncUnits = () => {
    const sy = scroller.scrollTop;
    instance.setUnits(tracked.map(t => {
      const r = t.el.getBoundingClientRect();
      const from = Math.max(0, Math.floor((r.top + sy) / density) - 1);
      const to = Math.max(from, Math.ceil((r.bottom + sy) / density) + 1);
      return { from, to, anchor: r.top + sy + r.height * 0.62 };
    }));
  };
  syncUnits();
  const repaint = () => { syncUnits(); instance.repaint(); };
  window.addEventListener('resize', repaint); document.fonts?.ready.then(repaint); document.querySelectorAll('img').forEach(i => i.addEventListener('load', repaint));
  new ResizeObserver(syncUnits).observe(document.body);
  return instance;
}
window.ParticleScroll = { createParticleScroll, mountParticleScroll, DEFAULTS };
})();

