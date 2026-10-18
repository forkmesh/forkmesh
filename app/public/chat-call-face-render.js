import { centroid, faceFrame, liveHead, projectPose, vertexDepths } from "./chat-call-face-geometry.js";
import { createFaceMesh } from "./chat-call-face-mesh.js";
import { createHairMatte, createMatte, floatTargets, MATTE_SAMPLE } from "./chat-call-segment.js";
import { OUTFIT_NECK, OUTFIT_SIZE, OUTFIT_UNIT } from "./chat-call-outfits.js";

const LOW_WIDTH = 160;
const FILL_WIDTH = 192;
const FILL_LEVELS = 5;
const TONE_SIGMA = 0.22;
const WIDE_SIGMA = 0.35;
const WIDE_PASSES = 3;
const HIDE_SIGMA = 4;
const HIDE_PASSES = 4;
const GAUSS = [0.2270, 0.1945, 0.1216, 0.0540, 0.0162];
const MIN_UNITS = 14;
const HALO_STRENGTH = 0.25;
const HAIR_SIDE = 6.4;
const HAIR_CENTER = [0, 0.5, -0.3];
const DEPTH_SCALE = 0.5;
const EYE_UV_HALF = [0.62, 0.36];
const LOOK_GAIN = 0.55;
const PHOTO_SOFTEN = 0.35;
const GRAIN = 0.012;
const SKIN_LEVEL = 0.4;
const SKIN_HUE = 0.6;
const SKIN_RANGE = [0.55, 1.8];
const HIGHLIGHT_KNEE = 0.82;
const HALO_CLEAR = [0.45, 0.92];
const LIP_SHADOW = 0.22;
const MOUTH_EDGE = 0.6;
const OUTFIT_REACH = 0.35;
const OUTFIT_DETAIL = 0.4;
const OUTFIT_SHADE = [0.75, 1.12];
const OUTFIT_HANDS = [1.2, 1.9];
const OUTFIT_OFF = Object.freeze({ center: [-1e5, -1e5], x: [0, 0], y: [0, 0], unit: 1 });

export const BACKGROUND_MODES = Object.freeze({ off: 0, blur: 1, image: 2 });

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
attribute float aHole;
attribute float aRegion;
attribute float aDepth;
uniform vec2 uSize;
varying vec2 vUv;
varying float vHole;
varying float vRegion;
void main() {
  vUv = aUv;
  vHole = aHole;
  vRegion = aRegion;
  gl_Position = vec4(aPos / uSize * 2.0 - 1.0, clamp(-aDepth * ${DEPTH_SCALE.toFixed(2)}, -0.99, 0.99), 1.0);
}`;

const WARP_FRAGMENT = `
precision mediump float;
uniform sampler2D uPhoto;
uniform sampler2D uAux;
uniform float uAuxPass;
uniform vec4 uEyeL;
uniform vec4 uEyeR;
uniform vec4 uLook;
uniform vec2 uClose;
varying vec2 vUv;
varying float vHole;
varying float vRegion;
const float SOFTEN = ${PHOTO_SOFTEN.toFixed(2)};
vec2 gaze(vec2 uv, vec4 eye, vec2 look) {
  vec2 q = (uv - eye.xy) / eye.zw;
  float m = 1.0 - smoothstep(0.45, 1.0, length(q));
  return uv - look * eye.zw * m;
}
vec3 lid(vec3 color, vec4 eye, float close) {
  if (close < 0.01) return color;
  vec2 q = (vUv - eye.xy) / eye.zw;
  float m = 1.0 - smoothstep(0.6, 1.05, length(q * vec2(1.0, 0.8)));
  vec3 skin = 0.5 * (texture2D(uPhoto, vec2(vUv.x, eye.y - eye.w * 1.25), 1.5).rgb + texture2D(uPhoto, vec2(vUv.x, eye.y + eye.w * 1.6), 1.5).rgb);
  float lash = (1.0 - smoothstep(0.0, 0.22, abs(q.y - 0.3))) * (1.0 - smoothstep(0.5, 0.95, abs(q.x))) * 0.45;
  return mix(color, skin * (1.0 - lash), m * smoothstep(0.15, 0.85, close));
}
void main() {
  vec3 aux = texture2D(uAux, vUv).rgb;
  float open = 1.0 - smoothstep(0.05, 0.95, vHole);
  if (uAuxPass > 0.5) {
    float skin = texture2D(uAux, vUv, 2.5).g;
    gl_FragColor = vec4(skin * aux.r * open, aux.b, 0.5 * (1.0 - open) + 0.5 * smoothstep(1.0, 2.0, vHole), vRegion);
  } else {
    vec2 uv = gaze(gaze(vUv, uEyeL, uLook.xy), uEyeR, uLook.zw);
    vec3 color = lid(lid(texture2D(uPhoto, uv, SOFTEN).rgb, uEyeL, uClose.x), uEyeR, uClose.y);
    gl_FragColor = vec4(color, aux.r * open);
  }
}`;

const REDUCE_FRAGMENT = `
precision mediump float;
uniform sampler2D uCam;
uniform sampler2D uWarp;
uniform sampler2D uAux;
uniform vec2 uStep;
uniform float uMode;
varying vec2 vUv;
vec4 tap(vec2 uv) {
  vec4 aux = texture2D(uAux, uv);
  float a = uMode > 1.5 ? 1.0 : aux.r * aux.a;
  vec3 color = (uMode < 0.5 || uMode > 1.5) ? texture2D(uCam, uv).rgb : texture2D(uWarp, uv).rgb;
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

const ZONE = `
uniform vec2 uZoneCenter;
uniform vec2 uZoneX;
uniform vec2 uZoneY;
uniform vec2 uPixels;
vec2 zoneAt(vec2 uv) {
  vec2 d = uv * uPixels - uZoneCenter;
  return vec2(dot(d, uZoneX), dot(d, uZoneY));
}
float zoneRadius(vec2 q) {
  return length((q - vec2(0.0, -0.35)) / vec2(2.55, 2.9));
}
uniform sampler2D uHair;
uniform float uHasHair;
float headZone(vec2 uv) {
  vec2 q = zoneAt(uv);
  float side = abs(q.x);
  float neck = smoothstep(0.8, 1.4, q.y) * (1.0 - smoothstep(0.55, 0.85, side));
  if (uHasHair > 0.5) {
    float face = 1.0 - smoothstep(0.9, 1.0, length((q - vec2(0.0, -0.3)) / vec2(2.1, 2.6)));
    face *= 1.0 - smoothstep(1.0, 1.5, q.y) * smoothstep(0.8, 1.05, side);
    float h = texture2D(uHair, uv).r;
    for (int k = 0; k < 8; k++) {
      float a = float(k) * 0.785398;
      vec2 dir = vec2(cos(a), sin(a)) / uPixels;
      h = max(h, texture2D(uHair, uv + dir * 4.0).r);
      h = max(h, texture2D(uHair, uv + dir * 9.0).r * 0.9);
    }
    float hair = smoothstep(0.05, 0.25, h) * (1.0 - neck);
    return max(face * (1.0 - neck), hair);
  }
  float head = 1.0 - smoothstep(0.82, 1.0, zoneRadius(q));
  return head * (1.0 - neck);
}
float headHalo(vec2 uv) {
  vec2 q = zoneAt(uv);
  return 1.0 - smoothstep(0.9, 1.45, zoneRadius(q - vec2(0.0, 0.3)));
}`;

const WEIGH_FRAGMENT = `
precision highp float;
uniform sampler2D uCam;
uniform sampler2D uCoef;
uniform float uHasMatte;
uniform vec2 uStep;
varying vec2 vUv;
${MATTE_SAMPLE}
float alphaAt(vec2 uv) {
  return uHasMatte > 0.5 ? callMatte(uCoef, texture2D(uCam, uv).rgb, uv) : 0.0;
}
void main() {
  vec2 a = vUv + vec2(-uStep.x, -uStep.y);
  vec2 b = vUv + vec2(uStep.x, -uStep.y);
  vec2 c = vUv + vec2(-uStep.x, uStep.y);
  vec2 d = vUv + vec2(uStep.x, uStep.y);
  vec3 color = 0.25 * (texture2D(uCam, a).rgb + texture2D(uCam, b).rgb + texture2D(uCam, c).rgb + texture2D(uCam, d).rgb);
  float reach = max(max(alphaAt(a), alphaAt(b)), max(alphaAt(c), alphaAt(d)));
  reach = max(reach, max(alphaAt(vUv + vec2(4.0 * uStep.x, 0.0)), alphaAt(vUv - vec2(4.0 * uStep.x, 0.0))));
  reach = max(reach, max(alphaAt(vUv + vec2(0.0, 4.0 * uStep.y)), alphaAt(vUv - vec2(0.0, 4.0 * uStep.y))));
  float w = pow(1.0 - reach, 3.0);
  gl_FragColor = vec4(color * w, w);
}`;

const PUSH_FRAGMENT = `
precision highp float;
uniform sampler2D uSource;
uniform vec2 uStep;
varying vec2 vUv;
void main() {
  gl_FragColor = 0.25 * (texture2D(uSource, vUv + vec2(-uStep.x, -uStep.y)) + texture2D(uSource, vUv + vec2(uStep.x, -uStep.y)) +
    texture2D(uSource, vUv + vec2(-uStep.x, uStep.y)) + texture2D(uSource, vUv + vec2(uStep.x, uStep.y)));
}`;

const PULL_FRAGMENT = `
precision highp float;
uniform sampler2D uOwn;
uniform sampler2D uCoarse;
uniform float uTop;
varying vec2 vUv;
void main() {
  vec4 own = texture2D(uOwn, vUv);
  vec3 mine = own.rgb / max(own.a, 1e-4);
  if (uTop > 0.5) {
    gl_FragColor = vec4(own.a > 1e-4 ? mine : vec3(0.5), 1.0);
    return;
  }
  vec3 coarse = texture2D(uCoarse, vUv).rgb;
  float trust = clamp(own.a * 3.0, 0.0, 1.0);
  gl_FragColor = vec4(mix(coarse, mine, trust), 1.0);
}`;

const BACKDROP_FRAGMENT = `
precision highp float;
uniform sampler2D uBlurred;
uniform sampler2D uFill;
varying vec2 vUv;
void main() {
  vec4 b = texture2D(uBlurred, vUv);
  vec3 fill = texture2D(uFill, vUv).rgb;
  vec3 mine = b.rgb / max(b.a, 1e-4);
  gl_FragColor = vec4(mix(fill, mine, smoothstep(0.02, 0.3, b.a)), clamp(b.a, 0.0, 1.0));
}`;

const COMPOSITE_FRAGMENT = `
precision highp float;
uniform sampler2D uCam;
uniform sampler2D uCoef;
uniform sampler2D uWarp;
uniform sampler2D uAux;
uniform sampler2D uLowCam;
uniform sampler2D uLowFace;
uniform sampler2D uWideCam;
uniform sampler2D uWideFace;
uniform sampler2D uFill;
uniform sampler2D uBackdrop;
uniform sampler2D uLowPlain;
uniform float uHasMatte;
uniform float uMode;
uniform float uFace;
uniform float uPresence;
uniform float uHide;
uniform vec2 uFaceUv;
uniform vec4 uImageRect;
uniform float uTongue;
uniform vec2 uTongueCenter;
uniform vec2 uTongueX;
uniform vec2 uTongueY;
uniform float uSkinRed;
uniform float uSeed;
uniform sampler2D uOutfit;
uniform float uOutfitOn;
uniform vec2 uOutfitCenter;
uniform vec2 uOutfitX;
uniform vec2 uOutfitY;
uniform vec2 uOutfitReach;
uniform vec2 uOutfitFace;
varying vec2 vUv;
${MATTE_SAMPLE}
${ZONE}
const vec3 LUMA = vec3(0.299, 0.587, 0.114);
vec3 chroma(vec3 c) {
  return c / (c.r + c.g + c.b + 0.03);
}
vec3 dress(vec3 front, vec3 cam, float alpha, float zone) {
  vec2 d = vUv * uPixels - uOutfitCenter;
  vec2 q = vec2(dot(d, uOutfitX), dot(d, uOutfitY));
  vec4 cloth = texture2D(uOutfit, (vec2(${OUTFIT_NECK[0].toFixed(1)}, ${OUTFIT_NECK[1].toFixed(1)}) + q * ${OUTFIT_UNIT.toFixed(1)}) / ${OUTFIT_SIZE.toFixed(1)});
  if (cloth.a < 0.002) return front;
  float mean = 0.0;
  for (int i = 0; i < 8; i++) {
    float angle = float(i) * 0.785398;
    mean += dot(texture2D(uCam, vUv + uOutfitReach * vec2(cos(angle), sin(angle))).rgb, LUMA);
  }
  mean = mean / 8.0;
  float detail = clamp(pow(dot(cam, LUMA) / max(mean, 0.02), ${OUTFIT_DETAIL.toFixed(2)}), ${OUTFIT_SHADE[0].toFixed(2)}, ${OUTFIT_SHADE[1].toFixed(2)});
  vec3 skin = vec3(0.0);
  for (int i = 0; i < 5; i++) {
    vec2 offset = i == 0 ? vec2(0.0) : uOutfitReach * vec2(i == 1 ? 1.0 : i == 2 ? -1.0 : 0.0, i == 3 ? 1.0 : i == 4 ? -1.0 : 0.0);
    skin += texture2D(uCam, uOutfitFace + offset).rgb;
  }
  skin = skin / 5.0;
  float near = 1.0 - smoothstep(0.035, 0.07, distance(chroma(cam), chroma(skin)));
  float bright = dot(cam, LUMA) / max(dot(skin, LUMA), 0.02);
  float hands = near * smoothstep(0.45, 0.65, bright) * (1.0 - smoothstep(1.6, 2.0, bright)) * smoothstep(${OUTFIT_HANDS[0].toFixed(2)}, ${OUTFIT_HANDS[1].toFixed(2)}, q.y);
  float hair = uHasHair > 0.5 ? smoothstep(0.4, 0.8, texture2D(uHair, vUv).r) : 0.0;
  float g = cloth.a * uOutfitOn * (1.0 - hands) * (1.0 - hair) * (1.0 - zone);
  vec3 tint = cloth.rgb / cloth.a;
  return mix(front, tint * detail * alpha, g);
}
float grain(vec2 at) {
  return fract(sin(dot(floor(at) + vec2(uSeed, uSeed * 0.61), vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
}
vec3 shoulder(vec3 x) {
  float k = ${HIGHLIGHT_KNEE.toFixed(2)};
  vec3 over = max(x - k, 0.0);
  return min(x, vec3(k)) + (1.0 - k) * (1.0 - exp(-over / (1.0 - k)));
}
float tongueAt(vec2 uv, vec3 cam) {
  vec2 d = uv * uPixels - uTongueCenter;
  vec2 q = vec2(dot(d, uTongueX), dot(d, uTongueY));
  float inside = 1.0 - smoothstep(0.75, 1.0, length(q));
  float red = (cam.r - cam.g) / (cam.r + cam.g + cam.b + 0.047);
  return inside * smoothstep(uSkinRed + 0.04, uSkinRed + 0.1, red);
}
vec3 ratio(vec4 cam, vec4 face) {
  return (cam.rgb + 0.004 * cam.a) / (face.rgb + 0.004 * face.a);
}
void main() {
  vec3 cam = texture2D(uCam, vUv).rgb;
  float alpha = uHasMatte > 0.5 ? callMatte(uCoef, cam, vUv) : 1.0;
  vec3 fill = texture2D(uFill, vUv).rgb;
  vec4 warp = vec4(0.0);
  vec4 aux = vec4(0.0);
  float zone = 0.0;
  float hole = 0.0;
  float cavity = 0.0;
  if (uFace > 0.5) {
    warp = texture2D(uWarp, vUv);
    aux = texture2D(uAux, vUv);
    hole = min(aux.b * 2.0, 1.0);
    cavity = max(aux.b * 2.0 - 1.0, 0.0);
    zone = headZone(vUv) * uPresence * (1.0 - hole) * uHasMatte;
  }
  vec3 front = clamp(cam - (1.0 - alpha) * fill, vec3(0.0), vec3(alpha));
  vec3 behind = alpha < 0.98 ? (cam - front) / (1.0 - alpha) : fill;
  if (uOutfitOn > 0.001 && uHasMatte > 0.5) front = dress(front, cam, alpha, zone);
  vec3 backdrop = behind;
  if (uMode > 1.5) {
    backdrop = texture2D(uBackdrop, uImageRect.xy + vUv * uImageRect.zw).rgb;
  } else if (uMode > 0.5) {
    backdrop = texture2D(uBackdrop, vUv).rgb;
  } else if (uFace > 0.5) {
    vec4 soft = texture2D(uBackdrop, vUv);
    float halo = headHalo(vUv) * uPresence * uHasMatte * (1.0 - smoothstep(${HALO_CLEAR[0].toFixed(2)}, ${HALO_CLEAR[1].toFixed(2)}, soft.a));
    backdrop = mix(behind, soft.rgb, halo);
  }
  float keep = alpha * (1.0 - zone);
  vec3 color = backdrop * (1.0 - keep) + front * (1.0 - zone);
  if (uFace > 0.5) color *= mix(1.0, mix(${MOUTH_EDGE.toFixed(2)}, 1.0, smoothstep(0.0, 1.0, cavity)), hole * uPresence);
  if (uFace > 0.5 && warp.a > 0.002) {
    vec4 lowCam = texture2D(uLowCam, vUv);
    vec4 lowFace = texture2D(uLowFace, vUv);
    vec4 wideCam = texture2D(uWideCam, vUv);
    vec4 wideFace = texture2D(uWideFace, vUv);
    vec4 centerCam = texture2D(uWideCam, uFaceUv);
    vec4 centerFace = texture2D(uWideFace, uFaceUv);
    vec3 meanCam = centerCam.rgb / max(centerCam.a, 1e-4);
    vec3 meanFace = centerFace.rgb / max(centerFace.a, 1e-4);
    bool known = centerFace.a > 0.01 && centerCam.a > 0.01;
    vec3 global = known ? clamp((meanCam + 0.004) / (meanFace + 0.004), 0.3, 3.0) : vec3(1.0);
    vec3 local = known ? clamp((wideCam.rgb + 0.12 * meanCam) / (wideFace.rgb + 0.12 * meanFace), 0.3, 3.0) : vec3(1.0);
    float near = (dot(lowCam.rgb, LUMA) + 0.004 * lowCam.a) / (dot(lowFace.rgb, LUMA) + 0.004 * lowFace.a);
    float far = (dot(wideCam.rgb, LUMA) + 0.004 * wideCam.a) / (dot(wideFace.rgb, LUMA) + 0.004 * wideFace.a);
    float shade = lowFace.a > 0.01 && wideFace.a > 0.01 ? clamp(near / far, 0.7, 1.4) : 1.0;
    shade = mix(1.0, shade, smoothstep(0.3, 0.7, lowFace.a));
    float level = max(dot(global, LUMA), 0.05);
    vec3 hue = global / level;
    vec3 hair = clamp(pow(hue, vec3(0.5)) * pow(level, 0.5), 0.45, 1.8);
    vec3 tone = clamp(pow(hue, vec3(${SKIN_HUE.toFixed(2)})) * pow(level, ${SKIN_LEVEL.toFixed(2)}), ${SKIN_RANGE[0].toFixed(2)}, ${SKIN_RANGE[1].toFixed(2)});
    vec3 lit = clamp(local / max(global, vec3(0.05)), 0.6, 1.6) * tone;
    float skin = clamp(aux.r / max(warp.a, 0.002), 0.0, 1.0);
    skin = smoothstep(0.15, 0.85, skin);
    vec3 face = clamp(shoulder(warp.rgb * mix(hair, lit * shade, skin)) + grain(vUv * uPixels) * ${GRAIN.toFixed(3)}, 0.0, 1.0);
    float lip = hole * (1.0 - hole) * 4.0;
    color *= 1.0 - ${LIP_SHADOW.toFixed(2)} * lip * uPresence;
    color = mix(color, face, warp.a * uPresence);
    if (uTongue > 0.001) color = mix(color, cam, tongueAt(vUv, cam) * uTongue * uPresence);
  }
  if (uHide > 0.001) {
    vec3 plain = texture2D(uLowPlain, vUv).rgb;
    vec3 hidden = uMode < 0.5 ? plain : mix(backdrop, plain, smoothstep(0.1, 0.6, alpha));
    color = mix(color, hidden, uHide);
  }
  gl_FragColor = vec4(color, 1.0);
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

function target(gl, width, height, type, depth = false) {
  const handle = texture(gl);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, type, null);
  const framebuffer = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, handle, 0);
  let renderbuffer = null;
  if (depth) {
    renderbuffer = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, renderbuffer);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, width, height);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, renderbuffer);
    gl.bindRenderbuffer(gl.RENDERBUFFER, null);
  }
  let ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
  if (!ok && renderbuffer) {
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, null);
    gl.deleteRenderbuffer(renderbuffer);
    renderbuffer = null;
    ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
  }
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  return ok ? { texture: handle, framebuffer, width, height, depth: Boolean(renderbuffer) } : null;
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

export function backgroundCapable(gl) {
  return Boolean(gl && gl.getParameter(gl.MAX_TEXTURE_IMAGE_UNITS) >= MIN_UNITS && floatTargets(gl));
}

export const VERTEX_FLOATS = 7;

export function vertexRegion(v, extraStart = Infinity) {
  if (v >= extraStart) return 1;
  if (v >= 68) return 0;
  return v <= 26 ? 0.25 : 1;
}

function signedArea(points, [a, b, c]) {
  const [p, q, r] = [points[a], points[b], points[c]];
  return (q[0] - p[0]) * (r[1] - p[1]) - (r[0] - p[0]) * (q[1] - p[1]);
}

export function warpVertices(mesh, layout, livePoints, photoSize, depths = null) {
  const out = new Float32Array(mesh.triangles.length * 3 * VERTEX_FLOATS);
  const extraStart = mesh.extraStart ?? Infinity;
  let at = 0;
  for (const triangle of mesh.triangles) {
    const folded = Boolean(triangle.cull) && signedArea(layout, triangle.v) * signedArea(livePoints, triangle.v) < 0;
    triangle.v.forEach((v, k) => {
      const placed = livePoints[folded ? triangle.v[0] : v];
      out[at++] = placed[0];
      out[at++] = placed[1];
      out[at++] = layout[v][0] / photoSize;
      out[at++] = layout[v][1] / photoSize;
      out[at++] = triangle.hole ? triangle.hole[k] : triangle.mouth ? 1 : 0;
      out[at++] = vertexRegion(v, extraStart);
      out[at++] = depths ? depths[v] : 0;
    });
  }
  return out;
}

export function coverRect(imageWidth, imageHeight, width, height) {
  const image = imageWidth / Math.max(1, imageHeight);
  const frame = width / Math.max(1, height);
  if (image > frame) {
    const span = frame / image;
    return [(1 - span) / 2, 0, span, 1];
  }
  const span = image / frame;
  return [0, (1 - span) / 2, 1, span];
}

export function hairRegion(pose, width, height) {
  const center = projectPose(pose, HAIR_CENTER);
  const side = HAIR_SIDE * pose.scale;
  const size = [Math.min(side, width), Math.min(side, height)];
  const fit = (value, extent, limit) => Math.min(Math.max(value, extent / 2), limit - extent / 2);
  return { center: [fit(center[0], size[0], width), fit(center[1], size[1], height)], size };
}

export function photoEyes(layout, photoSize) {
  return [[36, 39, [36, 37, 38, 39, 40, 41]], [42, 45, [42, 43, 44, 45, 46, 47]]].map(([a, b, ring]) => {
    const width = Math.hypot(layout[b][0] - layout[a][0], layout[b][1] - layout[a][1]) / photoSize;
    const center = centroid(layout, ring);
    return [center[0] / photoSize, center[1] / photoSize, EYE_UV_HALF[0] * width, EYE_UV_HALF[1] * width];
  });
}

export function lookUniform(look) {
  if (!look) return [0, 0, 0, 0];
  const pick = (side, k) => (Number.isFinite(look[side]?.[k]) ? look[side][k] * LOOK_GAIN : 0);
  return [pick(0, 0), pick(0, 1), pick(1, 0), pick(1, 1)];
}

export function axisFrame(box) {
  return {
    center: box.center,
    x: [box.ux[0] / box.half[0], box.ux[1] / box.half[0]],
    y: [box.uy[0] / box.half[1], box.uy[1] / box.half[1]],
  };
}

export function zoneFrame(head, pose) {
  const center = projectPose(pose, [0, 0, -0.35]);
  const frame = faceFrame(head);
  const scale = pose.scale || frame.span;
  const ux = [Math.cos(pose.roll), Math.sin(pose.roll)];
  const uy = [-ux[1], ux[0]];
  return {
    center,
    x: [ux[0] / scale, ux[1] / scale],
    y: [uy[0] / scale, uy[1] / scale],
  };
}

export function createFaceRenderer(canvas, { models = null } = {}) {
  const gl = faceWebGL(canvas);
  if (!gl) throw new Error("WebGL is unavailable");
  const format = floatTargets(gl);
  const half = gl.getExtension("OES_texture_half_float");
  const halfFloat = half && gl.getExtension("OES_texture_half_float_linear") &&
    gl.getExtension("EXT_color_buffer_half_float") ? half.HALF_FLOAT_OES : null;
  const units = gl.getParameter(gl.MAX_TEXTURE_IMAGE_UNITS);
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const meshBuffer = gl.createBuffer();
  const warp = compile(gl, WARP_VERTEX, WARP_FRAGMENT);
  const reduce = compile(gl, QUAD_VERTEX, REDUCE_FRAGMENT);
  const blur = compile(gl, QUAD_VERTEX, BLUR_FRAGMENT);
  const composite = compile(gl, QUAD_VERTEX, COMPOSITE_FRAGMENT);
  const weigh = compile(gl, QUAD_VERTEX, WEIGH_FRAGMENT);
  const push = compile(gl, QUAD_VERTEX, PUSH_FRAGMENT);
  const pull = compile(gl, QUAD_VERTEX, PULL_FRAGMENT);
  const backdropProgram = compile(gl, QUAD_VERTEX, BACKDROP_FRAGMENT);
  const camera = texture(gl);
  const photo = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  const auxPhoto = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  const imageTexture = texture(gl);
  const outfitTexture = texture(gl, gl.LINEAR_MIPMAP_LINEAR);
  let outfit = false;
  let matte = null;
  let faceMesh = null;
  let hairMatte = null;
  let width = 0;
  let height = 0;
  let targets = null;
  let face = null;
  let lost = false;
  let background = { mode: "off", strength: 0.5, image: null, imageWidth: 0, imageHeight: 0 };
  let lastHead = null;
  let frameCount = 0;

  function useModels(next) {
    if (!next || !format) return;
    if (!matte && units >= MIN_UNITS && (next.landscape || next.square)) {
      try {
        matte = createMatte(gl, next, { format });
      } catch (_) {
        matte = null;
      }
    }
    if (!hairMatte && next.hair) {
      try {
        hairMatte = createHairMatte(gl, next.hair, { format });
      } catch (_) {
        hairMatte = null;
      }
    }
    if (!faceMesh && next.detector && next.mesh) {
      try {
        faceMesh = createFaceMesh(gl, next, format);
      } catch (_) {
        faceMesh = null;
      }
    }
  }

  function upload(frame) {
    gl.bindTexture(gl.TEXTURE_2D, camera);
    gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
    gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, frame);
  }
  useModels(models);

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
    const fillWidth = Math.min(FILL_WIDTH, w);
    const fillHeight = Math.max(1, Math.round((fillWidth * h) / w));
    const pyramid = [];
    const pulled = [];
    let pw = fillWidth;
    let ph = fillHeight;
    for (let level = 0; level <= FILL_LEVELS; level += 1) {
      pyramid.push(make(pw, ph));
      pulled.push(make(pw, ph));
      pw = Math.max(1, Math.ceil(pw / 2));
      ph = Math.max(1, Math.ceil(ph / 2));
    }
    const blurWidth = Math.max(1, Math.ceil(fillWidth / 2));
    const blurHeight = Math.max(1, Math.ceil(fillHeight / 2));
    targets = {
      warp: target(gl, w, h, gl.UNSIGNED_BYTE, true),
      aux: target(gl, w, h, gl.UNSIGNED_BYTE, true),
      cam: make(lowWidth, lowHeight),
      face: make(lowWidth, lowHeight),
      plain: make(lowWidth, lowHeight),
      wideCam: make(lowWidth, lowHeight),
      wideFace: make(lowWidth, lowHeight),
      spare: make(lowWidth, lowHeight),
      pyramid,
      pulled,
      blurA: make(blurWidth, blurHeight),
      blurB: make(blurWidth, blurHeight),
      backdrop: make(blurWidth, blurHeight),
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
      if (program.uniforms[name]) gl.uniform1i(program.uniforms[name], unit);
    });
  }

  function into(out) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, out ? out.framebuffer : null);
    gl.viewport(0, 0, out ? out.width : width, out ? out.height : height);
  }

  function reduceInto(out, mode) {
    into(out);
    gl.useProgram(reduce.program);
    bindTextures(reduce, [["uCam", camera], ["uWarp", targets.warp.texture], ["uAux", targets.aux.texture]]);
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

  function blurInto(out, sigma, spare = targets.spare) {
    const step = Math.max(0.5, sigma / 2);
    blurPass(out, spare, step / out.width, 0);
    blurPass(spare, out, 0, step / out.height);
  }

  function setZone(program, zone) {
    const u = program.uniforms;
    if (!u.uZoneCenter) return;
    const z = zone || { center: [-1e5, -1e5], x: [0, 0], y: [0, 0] };
    gl.uniform2f(u.uZoneCenter, z.center[0], z.center[1]);
    gl.uniform2f(u.uZoneX, z.x[0], z.x[1]);
    gl.uniform2f(u.uZoneY, z.y[0], z.y[1]);
    gl.uniform2f(u.uPixels, width, height);
  }

  function buildFill(hasMatte, zone) {
    const levels = targets.pyramid;
    const first = levels[0];
    into(first);
    gl.useProgram(weigh.program);
    bindTextures(weigh, [["uCam", camera], ["uCoef", matte?.coefficients || camera], ["uMatteMask", matte?.mask || camera]]);
    gl.uniform1f(weigh.uniforms.uHasMatte, hasMatte ? 1 : 0);
    gl.uniform2f(weigh.uniforms.uStep, 0.25 * (width / first.width) / width, 0.25 * (height / first.height) / height);
    drawQuad(weigh);
    for (let level = 1; level < levels.length; level += 1) {
      const from = levels[level - 1];
      into(levels[level]);
      gl.useProgram(push.program);
      bindTextures(push, [["uSource", from.texture]]);
      gl.uniform2f(push.uniforms.uStep, 0.5 / from.width, 0.5 / from.height);
      drawQuad(push);
    }
    for (let level = levels.length - 1; level >= 0; level -= 1) {
      into(targets.pulled[level]);
      gl.useProgram(pull.program);
      const top = level === levels.length - 1;
      const coarse = top ? levels[level].texture : targets.pulled[level + 1].texture;
      bindTextures(pull, [["uOwn", levels[level].texture], ["uCoarse", coarse]]);
      gl.uniform1f(pull.uniforms.uTop, top ? 1 : 0);
      drawQuad(pull);
    }
    return targets.pulled[0].texture;
  }

  function buildBlurBackdrop(strength) {
    const source = targets.pyramid[1];
    into(targets.blurA);
    gl.useProgram(push.program);
    bindTextures(push, [["uSource", source.texture]]);
    gl.uniform2f(push.uniforms.uStep, 0, 0);
    drawQuad(push);
    const sigma = 1.5 + 5 * Math.max(0, Math.min(1, strength));
    const passes = strength > 0.6 ? 3 : 2;
    for (let pass = 0; pass < passes; pass += 1) blurInto(targets.blurA, sigma, targets.blurB);
    into(targets.backdrop);
    gl.useProgram(backdropProgram.program);
    bindTextures(backdropProgram, [["uBlurred", targets.blurA.texture], ["uFill", targets.pulled[1].texture]]);
    drawQuad(backdropProgram);
    return targets.backdrop.texture;
  }

  return {
    gl,
    useModels,
    get segmentation() {
      return Boolean(matte);
    },
    get matte() {
      return matte;
    },
    get faceMesh() {
      return faceMesh;
    },
    get hairMatte() {
      return hairMatte;
    },
    trackFace(frame, w, h) {
      if (!faceMesh || gl.isContextLost()) return null;
      upload(frame);
      return faceMesh.track(camera, w, h);
    },
    setPhoto(image, auxImage, mesh, layout, model, size) {
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      gl.bindTexture(gl.TEXTURE_2D, photo);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image);
      gl.generateMipmap(gl.TEXTURE_2D);
      gl.bindTexture(gl.TEXTURE_2D, auxPhoto);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, auxImage);
      gl.generateMipmap(gl.TEXTURE_2D);
      face = { mesh, layout, model, size, eyes: photoEyes(layout, size) };
    },
    clearPhoto() {
      face = null;
    },
    setBackground(next) {
      background = { ...background, ...next };
      if (next.image) {
        gl.bindTexture(gl.TEXTURE_2D, imageTexture);
        gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, next.image);
        background.imageWidth = next.image.naturalWidth || next.image.width;
        background.imageHeight = next.image.naturalHeight || next.image.height;
        background.image = true;
      }
    },
    get hasBackgroundImage() {
      return Boolean(background.image);
    },
    setOutfit(image) {
      outfit = Boolean(image);
      if (!image) return;
      gl.bindTexture(gl.TEXTURE_2D, outfitTexture);
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, true);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, image);
      gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
      gl.generateMipmap(gl.TEXTURE_2D);
    },
    get hasOutfit() {
      return outfit;
    },
    get lastHead() {
      return lastHead;
    },
    draw(frame, w, h, live, { presence = 1, hide = 0, runNetwork = true, runHair = true, needMatte = false, uploaded = false, expression = null, tongueBox = null, body = null, dressed = 0 } = {}) {
      if (gl.isContextLost()) {
        lost = true;
        return false;
      }
      resize(w, h);
      gl.disable(gl.BLEND);
      gl.disable(gl.DEPTH_TEST);
      if (!uploaded) upload(frame);
      const mode = BACKGROUND_MODES[background.mode] ?? 0;
      const activeMode = mode === 2 && !background.image ? 0 : mode;
      const showFace = Boolean(face && live && presence > 0.001);
      const wearing = Boolean(outfit && body && dressed > 0.001);
      const wantMatte = Boolean(matte) && (activeMode > 0 || showFace || wearing || needMatte);
      let seed = null;
      const seen = live || body?.points || null;
      if (seen) {
        const frameOfFace = faceFrame(seen);
        seed = [frameOfFace.mid[0] / w, frameOfFace.mid[1] / h, (0.8 * frameOfFace.span) / h];
      }
      const hasMatte = wantMatte && matte.update(camera, w, h, { runNetwork, face: seed }) && matte.ready;
      const showOutfit = wearing && hasMatte;
      let zone = null;
      let faceUv = [0.5, 0.5];
      lastHead = null;
      if (showFace) {
        const head = liveHead(face.model, live);
        lastHead = head;
        zone = zoneFrame(live, head.pose);
        const center = faceFrame(live).mid;
        faceUv = [center[0] / w, center[1] / h];
        const vertices = warpVertices(face.mesh, face.layout, head.points, face.size, vertexDepths(face.model, head.pose));
        gl.useProgram(warp.program);
        gl.bindBuffer(gl.ARRAY_BUFFER, meshBuffer);
        gl.bufferData(gl.ARRAY_BUFFER, vertices, gl.DYNAMIC_DRAW);
        const attrs = [["aPos", 2, 0], ["aUv", 2, 8], ["aHole", 1, 16], ["aRegion", 1, 20], ["aDepth", 1, 24]].map(([name, size, offset]) => {
          const at = gl.getAttribLocation(warp.program, name);
          gl.enableVertexAttribArray(at);
          gl.vertexAttribPointer(at, size, gl.FLOAT, false, VERTEX_FLOATS * 4, offset);
          return at;
        }).filter((at) => at >= 0);
        bindTextures(warp, [["uPhoto", photo], ["uAux", auxPhoto]]);
        gl.uniform2f(warp.uniforms.uSize, w, h);
        gl.uniform4f(warp.uniforms.uEyeL, ...face.eyes[0]);
        gl.uniform4f(warp.uniforms.uEyeR, ...face.eyes[1]);
        gl.uniform4f(warp.uniforms.uLook, ...lookUniform(expression?.look));
        gl.uniform2f(warp.uniforms.uClose, expression?.close?.[0] || 0, expression?.close?.[1] || 0);
        for (const [out, pass] of [[targets.warp, 0], [targets.aux, 1]]) {
          into(out);
          gl.clearColor(0, 0, 0, 0);
          if (out.depth) {
            gl.enable(gl.DEPTH_TEST);
            gl.depthFunc(gl.LESS);
            gl.depthMask(true);
            gl.clearDepth(1);
            gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
          } else {
            gl.clear(gl.COLOR_BUFFER_BIT);
          }
          gl.uniform1f(warp.uniforms.uAuxPass, pass);
          gl.bindBuffer(gl.ARRAY_BUFFER, meshBuffer);
          for (const at of attrs) gl.enableVertexAttribArray(at);
          gl.drawArrays(gl.TRIANGLES, 0, vertices.length / VERTEX_FLOATS);
          gl.disable(gl.DEPTH_TEST);
        }
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
      let hair = null;
      if (showFace && hasMatte && hairMatte && lastHead) {
        hair = hairMatte.update(camera, w, h, hairRegion(lastHead.pose, w, h), { run: runHair });
      }
      let fill = camera;
      let backdrop = camera;
      let imageRect = [0, 0, 1, 1];
      if (hasMatte && (activeMode > 0 || showFace || showOutfit)) fill = buildFill(true, showFace ? zone : null);
      if (activeMode === 1 && hasMatte) backdrop = buildBlurBackdrop(background.strength);
      else if (activeMode === 0 && hasMatte && showFace) backdrop = buildBlurBackdrop(HALO_STRENGTH);
      if (activeMode === 2) {
        backdrop = imageTexture;
        imageRect = coverRect(background.imageWidth, background.imageHeight, w, h);
      }
      if (hide > 0.001) {
        reduceInto(targets.plain, 2);
        for (let pass = 0; pass < HIDE_PASSES; pass += 1) blurInto(targets.plain, HIDE_SIGMA);
      }
      into(null);
      gl.useProgram(composite.program);
      bindTextures(composite, [
        ["uCam", camera],
        ["uCoef", matte?.coefficients || camera],
        ["uWarp", targets.warp.texture],
        ["uAux", targets.aux.texture],
        ["uLowCam", targets.cam.texture],
        ["uLowFace", targets.face.texture],
        ["uWideCam", targets.wideCam.texture],
        ["uWideFace", targets.wideFace.texture],
        ["uFill", fill],
        ["uBackdrop", backdrop],
        ["uLowPlain", targets.plain.texture],
        ["uHair", hair || camera],
        ["uMatteMask", matte?.mask || camera],
        ["uOutfit", outfitTexture],
      ]);
      gl.uniform1f(composite.uniforms.uHasHair, hair ? 1 : 0);
      gl.uniform1f(composite.uniforms.uHasMatte, hasMatte ? 1 : 0);
      gl.uniform1f(composite.uniforms.uMode, hasMatte || activeMode === 0 ? activeMode : 0);
      gl.uniform1f(composite.uniforms.uFace, showFace ? 1 : 0);
      gl.uniform1f(composite.uniforms.uPresence, showFace ? presence : 0);
      gl.uniform1f(composite.uniforms.uHide, hide);
      gl.uniform2f(composite.uniforms.uFaceUv, faceUv[0], faceUv[1]);
      gl.uniform4f(composite.uniforms.uImageRect, imageRect[0], imageRect[1], imageRect[2], imageRect[3]);
      const tongue = showFace && tongueBox && expression ? Math.max(0, Math.min(1, expression.tongue || 0)) : 0;
      const mouth = tongue > 0 ? axisFrame(tongueBox) : { center: [-1e5, -1e5], x: [0, 0], y: [0, 0] };
      gl.uniform1f(composite.uniforms.uTongue, tongue);
      gl.uniform2f(composite.uniforms.uTongueCenter, mouth.center[0], mouth.center[1]);
      gl.uniform2f(composite.uniforms.uTongueX, mouth.x[0], mouth.x[1]);
      gl.uniform2f(composite.uniforms.uTongueY, mouth.y[0], mouth.y[1]);
      gl.uniform1f(composite.uniforms.uSkinRed, expression?.skinRed ?? 0.12);
      frameCount = (frameCount + 1) % 4096;
      gl.uniform1f(composite.uniforms.uSeed, (frameCount * 7.31) % 97);
      const wear = showOutfit ? body.frame : OUTFIT_OFF;
      const wearFace = showOutfit ? faceFrame(body.points) : null;
      gl.uniform1f(composite.uniforms.uOutfitOn, showOutfit ? Math.min(1, dressed) : 0);
      gl.uniform2f(composite.uniforms.uOutfitCenter, wear.center[0], wear.center[1]);
      gl.uniform2f(composite.uniforms.uOutfitX, wear.x[0], wear.x[1]);
      gl.uniform2f(composite.uniforms.uOutfitY, wear.y[0], wear.y[1]);
      gl.uniform2f(composite.uniforms.uOutfitReach, (OUTFIT_REACH * wear.unit) / w, (OUTFIT_REACH * wear.unit) / h);
      gl.uniform2f(composite.uniforms.uOutfitFace, wearFace ? wearFace.mid[0] / w : 0.5, wearFace ? (wearFace.mid[1] + 0.6 * wearFace.span) / h : 0.5);
      setZone(composite, zone);
      drawQuad(composite, 1);
      return true;
    },
    get lost() {
      return lost || gl.isContextLost();
    },
    dispose() {
      matte?.dispose();
      faceMesh?.dispose();
      hairMatte?.dispose();
      gl.getExtension("WEBGL_lose_context")?.loseContext();
    },
  };
}
