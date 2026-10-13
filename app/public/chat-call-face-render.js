import { extendLandmarks, faceFrame } from "./chat-call-face-geometry.js";

const LOW_WIDTH = 160;
const TONE_SIGMA = 0.22;
const WIDE_SIGMA = 0.35;
const WIDE_PASSES = 3;
const HIDE_SIGMA = 4;
const HIDE_PASSES = 4;
const GAUSS = [0.2270, 0.1945, 0.1216, 0.0540, 0.0162];

const QUAD_VERTEX = `
attribute vec2 aPos;
uniform float uFlip;
varying vec2 vUv;
void main() {
  vUv = vec2(aPos.x * 0.5 + 0.5, uFlip > 0.5 ? 0.5 - aPos.y * 0.5 : aPos.y * 0.5 + 0.5);
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const WARP_VERTEX = `
attribute vec2 aPos;
attribute vec2 aUv;
attribute float aAlpha;
uniform vec2 uSize;
varying vec2 vUv;
varying float vAlpha;
void main() {
  vUv = aUv;
  vAlpha = aAlpha;
  gl_Position = vec4(aPos / uSize * 2.0 - 1.0, 0.0, 1.0);
}`;

const WARP_FRAGMENT = `
precision mediump float;
uniform sampler2D uPhoto;
varying vec2 vUv;
varying float vAlpha;
void main() {
  gl_FragColor = vec4(texture2D(uPhoto, vUv).rgb, vAlpha);
}`;

const REDUCE_FRAGMENT = `
precision mediump float;
uniform sampler2D uCam;
uniform sampler2D uWarp;
uniform vec2 uStep;
uniform float uMode;
varying vec2 vUv;
vec4 tap(vec2 uv) {
  vec4 w = texture2D(uWarp, uv);
  vec3 color = (uMode < 0.5 || uMode > 1.5) ? texture2D(uCam, uv).rgb : w.rgb;
  float a = uMode > 1.5 ? 1.0 : w.a;
  return vec4(color * a, a);
}
void main() {
  gl_FragColor = 0.25 * (tap(vUv + vec2(-uStep.x, -uStep.y)) + tap(vUv + vec2(uStep.x, -uStep.y)) +
    tap(vUv + vec2(-uStep.x, uStep.y)) + tap(vUv + vec2(uStep.x, uStep.y)));
}`;

const BLUR_FRAGMENT = `
precision mediump float;
uniform sampler2D uSource;
uniform vec2 uStep;
varying vec2 vUv;
void main() {
  vec4 sum = texture2D(uSource, vUv) * ${GAUSS[0].toFixed(4)};
  ${GAUSS.slice(1).map((weight, i) => `sum += (texture2D(uSource, vUv + uStep * ${(i + 1).toFixed(1)}) + texture2D(uSource, vUv - uStep * ${(i + 1).toFixed(1)})) * ${weight.toFixed(4)};`).join("\n  ")}
  gl_FragColor = sum;
}`;

const COMPOSITE_FRAGMENT = `
precision mediump float;
uniform sampler2D uCam;
uniform sampler2D uWarp;
uniform sampler2D uLowCam;
uniform sampler2D uLowFace;
uniform sampler2D uLowPlain;
uniform sampler2D uWideCam;
uniform sampler2D uWideFace;
uniform float uPresence;
uniform float uHide;
varying vec2 vUv;
void main() {
  vec3 cam = texture2D(uCam, vUv).rgb;
  vec4 warp = texture2D(uWarp, vUv);
  vec4 lowCam = texture2D(uLowCam, vUv);
  vec4 lowFace = texture2D(uLowFace, vUv);
  vec4 wideCam = texture2D(uWideCam, vUv);
  vec4 wideFace = texture2D(uWideFace, vUv);
  const vec3 luma = vec3(0.299, 0.587, 0.114);
  vec3 tint = wideFace.a > 0.01 ? (wideCam.rgb + 0.004) / (wideFace.rgb + 0.004) : vec3(1.0);
  float near = (dot(lowCam.rgb, luma) + 0.004) / (dot(lowFace.rgb, luma) + 0.004);
  float far = (dot(wideCam.rgb, luma) + 0.004) / (dot(wideFace.rgb, luma) + 0.004);
  float shade = lowFace.a > 0.01 ? clamp(near / far, 0.65, 1.5) : 1.0;
  vec3 face = clamp(warp.rgb * clamp(tint, 0.35, 2.8) * shade, 0.0, 1.0);
  vec3 color = mix(cam, face, warp.a * uPresence);
  vec3 hidden = texture2D(uLowPlain, vUv).rgb;
  gl_FragColor = vec4(mix(color, hidden, uHide), 1.0);
}`;

function compile(gl, vertex, fragment) {
  const program = gl.createProgram();
  for (const [type, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]]) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      throw new Error(gl.getShaderInfoLog(shader) || "shader failed");
    }
    gl.attachShader(program, shader);
  }
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
    throw new Error(gl.getProgramInfoLog(program) || "program failed");
  }
  const uniforms = {};
  const count = gl.getProgramParameter(program, gl.ACTIVE_UNIFORMS);
  for (let i = 0; i < count; i += 1) {
    const info = gl.getActiveUniform(program, i);
    uniforms[info.name] = gl.getUniformLocation(program, info.name);
  }
  return { program, uniforms };
}

function texture(gl, filter = gl.LINEAR) {
  const handle = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, handle);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  return handle;
}

function target(gl, width, height, type) {
  const handle = texture(gl);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, type, null);
  const framebuffer = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, handle, 0);
  const ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  return ok ? { texture: handle, framebuffer, width, height } : null;
}

export function faceWebGL(canvas) {
  try {
    return canvas?.getContext?.("webgl", {
      alpha: false,
      antialias: false,
      depth: false,
      premultipliedAlpha: false,
      preserveDrawingBuffer: true,
    }) || null;
  } catch (_) {
    return null;
  }
}

export function warpVertices(mesh, sourcePoints, livePoints, photoSize, look = {}) {
  const forehead = look.forehead;
  const brows = look.brows ?? 1;
  const source = extendLandmarks(sourcePoints, forehead).points;
  const live = extendLandmarks(livePoints, forehead).points;
  const out = new Float32Array(mesh.triangles.length * 15);
  let at = 0;
  for (const triangle of mesh.triangles) {
    for (const v of triangle.v) {
      out[at++] = live[v][0];
      out[at++] = live[v][1];
      out[at++] = source[v][0] / photoSize;
      out[at++] = source[v][1] / photoSize;
      out[at++] = triangle.mouth ? 0 : v >= 17 && v <= 26 ? brows : mesh.alpha[v];
    }
  }
  return out;
}

export function createFaceRenderer(canvas) {
  const gl = faceWebGL(canvas);
  if (!gl) throw new Error("WebGL is unavailable");
  const half = gl.getExtension("OES_texture_half_float");
  const halfFloat = half && gl.getExtension("OES_texture_half_float_linear") &&
    gl.getExtension("EXT_color_buffer_half_float") ? half.HALF_FLOAT_OES : null;
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const meshBuffer = gl.createBuffer();
  const warp = compile(gl, WARP_VERTEX, WARP_FRAGMENT);
  const reduce = compile(gl, QUAD_VERTEX, REDUCE_FRAGMENT);
  const blur = compile(gl, QUAD_VERTEX, BLUR_FRAGMENT);
  const composite = compile(gl, QUAD_VERTEX, COMPOSITE_FRAGMENT);
  const camera = texture(gl);
  const photo = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  let width = 0;
  let height = 0;
  let targets = null;
  let face = null;
  let lost = false;

  function resize(w, h) {
    if (w === width && h === height && targets) return;
    width = w;
    height = h;
    canvas.width = w;
    canvas.height = h;
    const lowWidth = Math.min(LOW_WIDTH, w);
    const lowHeight = Math.max(1, Math.round((lowWidth * h) / w));
    const make = (tw, th) => target(gl, tw, th, halfFloat || gl.UNSIGNED_BYTE) ||
      target(gl, tw, th, gl.UNSIGNED_BYTE);
    targets = {
      warp: target(gl, w, h, gl.UNSIGNED_BYTE),
      cam: make(lowWidth, lowHeight),
      face: make(lowWidth, lowHeight),
      plain: make(lowWidth, lowHeight),
      wideCam: make(lowWidth, lowHeight),
      wideFace: make(lowWidth, lowHeight),
      spare: make(lowWidth, lowHeight),
    };
  }

  function drawQuad(program, flip = 0) {
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    const at = gl.getAttribLocation(program.program, "aPos");
    gl.enableVertexAttribArray(at);
    gl.vertexAttribPointer(at, 2, gl.FLOAT, false, 0, 0);
    if (program.uniforms.uFlip) gl.uniform1f(program.uniforms.uFlip, flip);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.disableVertexAttribArray(at);
  }

  function bindTextures(program, entries) {
    entries.forEach(([name, handle], unit) => {
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_2D, handle);
      gl.uniform1i(program.uniforms[name], unit);
    });
  }

  function into(out) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, out ? out.framebuffer : null);
    gl.viewport(0, 0, out ? out.width : width, out ? out.height : height);
  }

  function reduceInto(out, mode) {
    into(out);
    gl.useProgram(reduce.program);
    bindTextures(reduce, [["uCam", camera], ["uWarp", targets.warp.texture]]);
    gl.uniform2f(reduce.uniforms.uStep, 0.25 * (width / out.width) / width, 0.25 * (height / out.height) / height);
    gl.uniform1f(reduce.uniforms.uMode, mode);
    drawQuad(reduce);
  }

  function blurPass(from, to, dx, dy) {
    into(to);
    gl.useProgram(blur.program);
    bindTextures(blur, [["uSource", from.texture]]);
    gl.uniform2f(blur.uniforms.uStep, dx, dy);
    drawQuad(blur);
  }

  function blurInto(out, sigma) {
    const step = Math.max(0.5, sigma / 2);
    blurPass(out, targets.spare, step / out.width, 0);
    blurPass(targets.spare, out, 0, step / out.height);
  }

  return {
    gl,
    setPhoto(image, mesh, points, size, look) {
      gl.bindTexture(gl.TEXTURE_2D, photo);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image);
      gl.generateMipmap(gl.TEXTURE_2D);
      face = { mesh, points, size, look };
    },
    clearPhoto() {
      face = null;
    },
    draw(frame, w, h, live, { presence = 1, hide = 0 } = {}) {
      if (gl.isContextLost()) {
        lost = true;
        return false;
      }
      resize(w, h);
      gl.disable(gl.BLEND);
      gl.disable(gl.DEPTH_TEST);
      gl.bindTexture(gl.TEXTURE_2D, camera);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
      into(targets.warp);
      gl.clearColor(0, 0, 0, 0);
      gl.clear(gl.COLOR_BUFFER_BIT);
      const showFace = face && live && presence > 0.001;
      if (showFace) {
        const vertices = warpVertices(face.mesh, face.points, live, face.size, face.look);
        gl.useProgram(warp.program);
        gl.bindBuffer(gl.ARRAY_BUFFER, meshBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
        const attrs = [["aPos", 2, 0], ["aUv", 2, 8], ["aAlpha", 1, 16]].map(([name, size, offset]) => {
          const at = gl.getAttribLocation(warp.program, name);
          gl.enableVertexAttribArray(at);
          gl.vertexAttribPointer(at, size, gl.FLOAT, false, 20, offset);
          return at;
        });
        bindTextures(warp, [["uPhoto", photo]]);
        gl.uniform2f(warp.uniforms.uSize, w, h);
        gl.drawArrays(gl.TRIANGLES, 0, vertices.length / 5);
        for (const at of attrs) gl.disableVertexAttribArray(at);
        const span = faceFrame(live).span;
        const sigma = (TONE_SIGMA * span * targets.cam.width) / w;
        reduceInto(targets.cam, 0);
        blurInto(targets.cam, sigma);
        reduceInto(targets.face, 1);
        blurInto(targets.face, sigma);
        const wide = (WIDE_SIGMA * span * targets.cam.width) / w;
        reduceInto(targets.wideCam, 0);
        reduceInto(targets.wideFace, 1);
        for (let pass = 0; pass < WIDE_PASSES; pass += 1) {
          blurInto(targets.wideCam, wide);
          blurInto(targets.wideFace, wide);
        }
      }
      if (hide > 0.001) {
        reduceInto(targets.plain, 2);
        for (let pass = 0; pass < HIDE_PASSES; pass += 1) blurInto(targets.plain, HIDE_SIGMA);
      }
      into(null);
      gl.useProgram(composite.program);
      bindTextures(composite, [
        ["uCam", camera],
        ["uWarp", targets.warp.texture],
        ["uLowCam", targets.cam.texture],
        ["uLowFace", targets.face.texture],
        ["uLowPlain", targets.plain.texture],
        ["uWideCam", targets.wideCam.texture],
        ["uWideFace", targets.wideFace.texture],
      ]);
      gl.uniform1f(composite.uniforms.uPresence, showFace ? presence : 0);
      gl.uniform1f(composite.uniforms.uHide, hide);
      drawQuad(composite, 1);
      return true;
    },
    get lost() {
      return lost || gl.isContextLost();
    },
    dispose() {
      gl.getExtension("WEBGL_lose_context")?.loseContext();
    },
  };
}
