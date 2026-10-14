export const SEGMENT_MODEL_BASE = "/assets/call-segmentation/";
export const SEGMENT_MODELS = Object.freeze({
  landscape: "selfie-landscape.fmnn",
  square: "selfie-square.fmnn",
  detector: "face-detector.fmnn",
  mesh: "face-mesh.fmnn",
  hair: "hair-192.fmnn",
});
export const MODEL_GROUPS = Object.freeze({ segment: ["landscape", "square"], face: ["detector", "mesh"], hair: ["hair"] });

const MAGIC = 0x4e4e4d46;

export function parseSegmentModel(buffer) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) !== MAGIC) throw new Error("not a segmentation model");
  const length = view.getUint32(8, true);
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(12, 12 + length)));
  const start = 12 + length;
  const count = (bytes.byteLength - start) >> 1;
  const aligned = new Uint8Array(count * 2);
  aligned.set(bytes.subarray(start, start + count * 2));
  const weights = new Uint16Array(aligned.buffer);
  return { ...header, outputs: header.outputs || [[header.output]], weights };
}

export function decodeBase64(text) {
  const raw = globalThis.atob(text);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) out[i] = raw.charCodeAt(i);
  return out;
}

export async function loadSegmentModels({ fetchImpl = globalThis.fetch?.bind(globalThis), base = SEGMENT_MODEL_BASE, names = Object.keys(SEGMENT_MODELS) } = {}) {
  const inline = globalThis.FmCallSegmentModels;
  const out = {};
  await Promise.all(names.map(async (name) => {
    try {
      if (inline?.[name]) {
        out[name] = parseSegmentModel(decodeBase64(inline[name]));
      } else if (fetchImpl) {
        const response = await fetchImpl(base + SEGMENT_MODELS[name]);
        if (response.ok) out[name] = parseSegmentModel(await response.arrayBuffer());
      }
    } catch (_) {}
  }));
  return out;
}

const modelPromises = new Map();

export function sharedSegmentModels(group = "segment", options = {}) {
  if (!modelPromises.has(group)) modelPromises.set(group, loadSegmentModels({ ...options, names: MODEL_GROUPS[group] || [] }));
  return modelPromises.get(group);
}

export function halfToFloat(h) {
  const sign = h & 0x8000 ? -1 : 1;
  const exponent = (h >> 10) & 0x1f;
  const fraction = h & 0x3ff;
  if (exponent === 0) return sign * 2 ** -14 * (fraction / 1024);
  if (exponent === 31) return fraction ? NaN : sign * Infinity;
  return sign * 2 ** (exponent - 15) * (1 + fraction / 1024);
}

export function floatToHalf(value) {
  const f32 = new Float32Array(1);
  const u32 = new Uint32Array(f32.buffer);
  f32[0] = value;
  const x = u32[0];
  const sign = (x >> 16) & 0x8000;
  let exponent = ((x >> 23) & 0xff) - 127 + 15;
  let mantissa = x & 0x7fffff;
  if (exponent <= 0) {
    if (exponent < -10) return sign;
    mantissa |= 0x800000;
    const shift = 14 - exponent;
    let half = mantissa >> shift;
    if ((mantissa >> (shift - 1)) & 1) half += 1;
    return sign | half;
  }
  if (exponent >= 31) return sign | 0x7c00;
  let half = sign | (exponent << 10) | (mantissa >> 13);
  if (mantissa & 0x1000) half += 1;
  return half;
}

const slices = (channels) => Math.ceil(channels / 4);

function activation(name, alphaRow = 0) {
  if (name === "prelu") return `v = max(v, 0.0) + wAt(s, ${alphaRow.toFixed(1)}) * min(v, 0.0);`;
  if (name === "relu") return "v = max(v, 0.0);";
  if (name === "hswish") return "v = v * clamp(v + 3.0, 0.0, 6.0) / 6.0;";
  if (name === "sigmoid") return "v = 1.0 / (1.0 + exp(-clamp(v, -30.0, 30.0)));";
  return "";
}

const HEAD = `
precision highp float;
uniform sampler2D uIn;
uniform sampler2D uIn2;
uniform sampler2D uW;
uniform vec2 uInSize;
uniform vec2 uIn2Size;
uniform vec2 uWSize;
`;

function prelude(outW) {
  return `
void main() {
  vec2 frag = floor(gl_FragCoord.xy);
  float s = floor((frag.x + 0.5) / ${outW.toFixed(1)});
  float x = frag.x - s * ${outW.toFixed(1)};
  float y = frag.y;`;
}

function fetchIn(name, size, width, height) {
  return `
vec4 ${name}At(float x, float y, float s) {
  if (x < 0.0 || y < 0.0 || x > ${(width - 0.5).toFixed(1)} || y > ${(height - 0.5).toFixed(1)}) return vec4(0.0);
  return texture2D(${name === "a" ? "uIn" : "uIn2"}, vec2(s * ${width.toFixed(1)} + x + 0.5, y + 0.5) / ${size});
}`;
}

function weightAt() {
  return `
vec4 wAt(float x, float y) {
  return texture2D(uW, vec2(x + 0.5, y + 0.5) / uWSize);
}`;
}

export function layerShader(op, inShape, outShape, secondShape = null, alphaRow = 0) {
  const [inH, inW, inC] = inShape;
  const [outH, outW, outC] = outShape;
  const si = slices(inC);
  const so = slices(outC);
  let body = "";
  const parts = [HEAD, fetchIn("a", "uInSize", inW, inH)];
  const weighted = op.op === "conv" || op.op === "tconv" || op.op === "dw";
  if (op.act === "prelu" && !weighted) parts.push(weightAt());
  if (op.op === "conv" || op.op === "tconv") {
    parts.push(weightAt());
    const [kh, kw] = op.kernel;
    const [sh, sw] = op.stride;
    const [pt, pl] = op.pad || [0, 0];
    const [dh, dw] = op.dilation || [1, 1];
    const position = op.op === "conv"
      ? `float ix = x * ${sw.toFixed(1)} + float(kx) * ${dw.toFixed(1)} - ${pl.toFixed(1)}; float iy = y * ${sh.toFixed(1)} + float(ky) * ${dh.toFixed(1)} - ${pt.toFixed(1)};`
      : `float nx = x + ${pl.toFixed(1)} - float(kx); float ny = y + ${pt.toFixed(1)} - float(ky);
      float ix = floor(nx / ${sw.toFixed(1)} + 0.001); float iy = floor(ny / ${sh.toFixed(1)} + 0.001);
      if (abs(ix * ${sw.toFixed(1)} - nx) > 0.01 || abs(iy * ${sh.toFixed(1)} - ny) > 0.01) continue;`;
    body = `
  vec4 v = wAt(s, ${so.toFixed(1)});
  for (int ky = 0; ky < ${kh}; ky++) {
    for (int kx = 0; kx < ${kw}; kx++) {
      ${position}
      if (ix < 0.0 || iy < 0.0 || ix > ${(inW - 0.5).toFixed(1)} || iy > ${(inH - 0.5).toFixed(1)}) continue;
      float base = float((ky * ${kw} + kx) * ${si} * 4);
      for (int i = 0; i < ${si}; i++) {
        vec4 t = aAt(ix, iy, float(i));
        float col = base + float(i * 4);
        v += wAt(col, s) * t.x + wAt(col + 1.0, s) * t.y + wAt(col + 2.0, s) * t.z + wAt(col + 3.0, s) * t.w;
      }
    }
  }`;
  } else if (op.op === "dw") {
    parts.push(weightAt());
    const [kh, kw] = op.kernel;
    const [sh, sw] = op.stride;
    const [pt, pl] = op.pad || [0, 0];
    body = `
  vec4 v = wAt(s, ${so.toFixed(1)});
  for (int ky = 0; ky < ${kh}; ky++) {
    for (int kx = 0; kx < ${kw}; kx++) {
      float ix = x * ${sw.toFixed(1)} + float(kx) - ${pl.toFixed(1)};
      float iy = y * ${sh.toFixed(1)} + float(ky) - ${pt.toFixed(1)};
      v += wAt(float(ky * ${kw} + kx), s) * aAt(ix, iy, s);
    }
  }`;
  } else if (op.op === "meanrow") {
    body = `
  vec4 v = vec4(0.0);
  for (int i = 0; i < ${inW}; i++) v += aAt(float(i), y, s);
  v /= ${inW.toFixed(1)};`;
  } else if (op.op === "meancol") {
    body = `
  vec4 v = vec4(0.0);
  for (int i = 0; i < ${inH}; i++) v += aAt(0.0, float(i), s);
  v /= ${inH.toFixed(1)};`;
  } else if (op.op === "add" || op.op === "mul") {
    const [bH, bW] = secondShape;
    parts.push(fetchIn("b", "uIn2Size", bW, bH));
    const other = bH === 1 && bW === 1 ? "bAt(0.0, 0.0, s)" : "bAt(x, y, s)";
    body = `
  vec4 v = aAt(x, y, s) ${op.op === "add" ? "+" : "*"} ${other};`;
  } else if (op.op === "resize") {
    const scaleX = op.align ? (inW - 1) / Math.max(1, outW - 1) : inW / outW;
    const scaleY = op.align ? (inH - 1) / Math.max(1, outH - 1) : inH / outH;
    const sx = op.half ? `max((x + 0.5) * ${scaleX.toFixed(6)} - 0.5, 0.0)` : `x * ${scaleX.toFixed(6)}`;
    const sy = op.half ? `max((y + 0.5) * ${scaleY.toFixed(6)} - 0.5, 0.0)` : `y * ${scaleY.toFixed(6)}`;
    body = `
  float fx = ${sx};
  float fy = ${sy};
  float x0 = floor(fx);
  float y0 = floor(fy);
  float x1 = min(x0 + 1.0, ${(inW - 1).toFixed(1)});
  float y1 = min(y0 + 1.0, ${(inH - 1).toFixed(1)});
  float tx = fx - x0;
  float ty = fy - y0;
  vec4 v = mix(mix(aAt(x0, y0, s), aAt(x1, y0, s), tx), mix(aAt(x0, y1, s), aAt(x1, y1, s), tx), ty);`;
  } else if (op.op === "act") {
    body = `
  vec4 v = aAt(x, y, s);`;
  } else if (op.op === "argmax") {
    body = `
  vec4 best = vec4(-65000.0);
  vec4 v = vec4(0.0);
  for (int k = 0; k < 4; k++) {
    float dy = float(k / 2);
    float dx = float(k) - dy * 2.0;
    vec4 t = aAt(x * 2.0 + dx, y * 2.0 + dy, s);
    vec4 pick = vec4(greaterThan(t, best));
    v = mix(v, vec4(float(k)), pick);
    best = max(best, t);
  }`;
  } else if (op.op === "unpool") {
    const [bH, bW] = secondShape;
    parts.push(fetchIn("b", "uIn2Size", bW, bH));
    body = `
  float px = floor(x / 2.0);
  float py = floor(y / 2.0);
  float k = (y - py * 2.0) * 2.0 + (x - px * 2.0);
  vec4 index = bAt(px, py, s);
  vec4 v = aAt(px, py, s) * vec4(lessThan(abs(index - vec4(k)), vec4(0.5)));`;
  } else if (op.op === "concat") {
    const [bH, bW, bC] = secondShape;
    parts.push(fetchIn("b", "uIn2Size", bW, bH));
    const sa = si;
    if (inC % 4 === 0) {
      body = `
  vec4 v = s < ${sa.toFixed(1)} - 0.5 ? aAt(x, y, s) : bAt(x, y, s - ${sa.toFixed(1)});`;
    } else {
      body = `
  vec4 v = vec4(0.0);
  for (int j = 0; j < 4; j++) {
    float c = s * 4.0 + float(j);
    float pick = 0.0;
    if (c < ${inC.toFixed(1)} - 0.5) {
      vec4 t = aAt(x, y, floor(c / 4.0));
      float m = c - floor(c / 4.0) * 4.0;
      pick = m < 0.5 ? t.x : m < 1.5 ? t.y : m < 2.5 ? t.z : t.w;
    } else if (c < ${(inC + bC).toFixed(1)} - 0.5) {
      float d = c - ${inC.toFixed(1)};
      vec4 t = bAt(x, y, floor(d / 4.0));
      float m = d - floor(d / 4.0) * 4.0;
      pick = m < 0.5 ? t.x : m < 1.5 ? t.y : m < 2.5 ? t.z : t.w;
    }
    if (j == 0) v.x = pick; else if (j == 1) v.y = pick; else if (j == 2) v.z = pick; else v.w = pick;
  }`;
    }
  } else if (op.op === "pad") {
    body = `
  vec4 v = s < ${si.toFixed(1)} - 0.5 ? aAt(x, y, s) : vec4(0.0);`;
  } else if (op.op === "maxpool") {
    const [kh, kw] = op.kernel;
    const [sh, sw] = op.stride;
    const [pt, pl] = op.pad || [0, 0];
    body = `
  vec4 v = vec4(-65000.0);
  for (int ky = 0; ky < ${kh}; ky++) {
    for (int kx = 0; kx < ${kw}; kx++) {
      float ix = x * ${sw.toFixed(1)} + float(kx) - ${pl.toFixed(1)};
      float iy = y * ${sh.toFixed(1)} + float(ky) - ${pt.toFixed(1)};
      if (ix < 0.0 || iy < 0.0 || ix > ${(inW - 0.5).toFixed(1)} || iy > ${(inH - 0.5).toFixed(1)}) continue;
      v = max(v, aAt(ix, iy, s));
    }
  }`;
  } else {
    throw new Error(`unsupported layer ${op.op}`);
  }
  parts.push(prelude(outW), body, `
  ${activation(op.act, alphaRow)}
  gl_FragColor = v;
}`);
  return parts.join("\n");
}

export function packAlpha(op, weights) {
  const so = slices(op.cout ?? op.channels);
  const data = new Uint16Array(so * 4);
  for (let c = 0; c < op.alpha[1]; c += 1) data[c] = weights[op.alpha[0] + c];
  return { data, width: so, height: 1, alphaRow: 0 };
}

function withAlpha(packed, op, weights) {
  if (!op.alpha) return { ...packed, alphaRow: 0 };
  const rows = packed.height + 1;
  const data = new Uint16Array(packed.width * rows * 4);
  data.set(packed.data);
  for (let c = 0; c < op.alpha[1]; c += 1) data[(packed.height * packed.width + (c >> 2)) * 4 + (c & 3)] = weights[op.alpha[0] + c];
  return { data, width: packed.width, height: rows, alphaRow: packed.height };
}

export function packWeights(op, weights) {
  return withAlpha(packLayer(op, weights), op, weights);
}

function packLayer(op, weights) {
  const read = (offset) => weights[op.w[0] + offset];
  const bias = (c) => (c < op.cout ? weights[op.b[0] + c] : 0);
  const so = slices(op.cout);
  if (op.op === "dw") {
    const [kh, kw] = op.kernel;
    const width = Math.max(kh * kw, so);
    const data = new Uint16Array(width * (so + 1) * 4);
    for (let s = 0; s < so; s += 1) {
      for (let k = 0; k < kh * kw; k += 1) {
        for (let j = 0; j < 4; j += 1) {
          const c = s * 4 + j;
          data[(s * width + k) * 4 + j] = c < op.cout ? read(k * op.cout + c) : 0;
        }
      }
      for (let j = 0; j < 4; j += 1) data[(so * width + s) * 4 + j] = bias(s * 4 + j);
    }
    return { data, width, height: so + 1 };
  }
  const [kh, kw] = op.kernel;
  const si = slices(op.cin);
  const width = Math.max(kh * kw * si * 4, so);
  const data = new Uint16Array(width * (so + 1) * 4);
  for (let s = 0; s < so; s += 1) {
    for (let y = 0; y < kh; y += 1) {
      for (let x = 0; x < kw; x += 1) {
        for (let i = 0; i < op.cin; i += 1) {
          const col = ((y * kw + x) * si * 4) + i;
          for (let j = 0; j < 4; j += 1) {
            const o = s * 4 + j;
            if (o < op.cout) data[(s * width + col) * 4 + j] = read(((o * kh + y) * kw + x) * op.cin + i);
          }
        }
      }
    }
    for (let j = 0; j < 4; j += 1) data[(so * width + s) * 4 + j] = bias(s * 4 + j);
  }
  return { data, width, height: so + 1 };
}

export function expandLayers(model) {
  const shapes = { ...model.shapes };
  const layers = [];
  let next = Math.max(...Object.keys(shapes).map(Number)) + 1;
  for (const op of model.ops) {
    if (op.op === "mean") {
      const [h, , c] = shapes[op.in[0]];
      const middle = next++;
      shapes[middle] = [h, 1, c];
      layers.push({ op: "meanrow", in: [op.in[0]], out: middle, act: "none" });
      layers.push({ op: "meancol", in: [middle], out: op.out, act: op.act });
    } else {
      layers.push(op);
    }
  }
  return { layers, shapes };
}

const QUAD = `
attribute vec2 aPos;
void main() {
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

function compileProgram(gl, vertex, fragment) {
  const program = gl.createProgram();
  for (const [type, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]]) {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
      throw new Error(gl.getShaderInfoLog(shader) || "shader failed");
    }
    gl.attachShader(program, shader);
    gl.deleteShader(shader);
  }
  gl.bindAttribLocation(program, 0, "aPos");
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

export function floatTargets(gl) {
  const half = gl.getExtension("OES_texture_half_float");
  gl.getExtension("EXT_color_buffer_half_float");
  const full = gl.getExtension("OES_texture_float");
  gl.getExtension("WEBGL_color_buffer_float");
  const options = [];
  if (half) options.push({ type: half.HALF_FLOAT_OES, half: true });
  if (full) options.push({ type: gl.FLOAT, half: false });
  for (const option of options) {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 4, 4, 0, gl.RGBA, option.type, null);
    const framebuffer = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    const ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.deleteFramebuffer(framebuffer);
    gl.deleteTexture(texture);
    if (ok) return option;
  }
  return null;
}

export function createNetwork(gl, model, format = floatTargets(gl)) {
  if (!format) throw new Error("no float render targets");
  const { layers, shapes } = expandLayers(model);
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const programs = new Map();
  const tensors = {};
  const sizeOf = (index) => {
    const [h, w, c] = shapes[index];
    return [w * slices(c), h];
  };
  const makeTexture = (width, height, data = null, type = format.type) => {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, type, data);
    return texture;
  };
  const tensor = (index) => {
    if (tensors[index]) return tensors[index];
    const [width, height] = sizeOf(index);
    const texture = makeTexture(width, height);
    const framebuffer = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    tensors[index] = { texture, framebuffer, width, height };
    return tensors[index];
  };
  const toWeightData = (data) => {
    if (format.half) return data;
    const out = new Float32Array(data.length);
    for (let i = 0; i < data.length; i += 1) out[i] = halfToFloat(data[i]);
    return out;
  };
  const steps = layers.map((layer) => {
    const second = layer.in[1] !== undefined ? shapes[layer.in[1]] : null;
    let packed = null;
    if (layer.w) packed = packWeights(layer, model.weights);
    else if (layer.alpha) packed = packAlpha({ ...layer, channels: shapes[layer.out][2] }, model.weights);
    const source = layerShader(layer, shapes[layer.in[0]], shapes[layer.out], second, packed?.alphaRow || 0);
    if (!programs.has(source)) programs.set(source, compileProgram(gl, QUAD, source));
    let weights = null;
    if (packed) {
      weights = { texture: makeTexture(packed.width, packed.height, toWeightData(packed.data)), width: packed.width, height: packed.height };
    }
    return { layer, program: programs.get(source), weights, input: tensor(layer.in[0]), second: layer.in[1] !== undefined ? tensor(layer.in[1]) : null, output: tensor(layer.out) };
  });
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  const input = tensor(model.input);
  const output = tensor(model.output);
  const outputs = (model.outputs || [[model.output]]).map((group) => group.map((index) => tensor(index)));
  const [inH, inW] = shapes[model.input];

  return {
    input,
    output,
    outputs,
    shapes,
    inputWidth: inW,
    inputHeight: inH,
    programCount: programs.size,
    run() {
      gl.bindBuffer(gl.ARRAY_BUFFER, quad);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
      gl.disable(gl.BLEND);
      for (const step of steps) {
        const { program, output: out } = step;
        gl.bindFramebuffer(gl.FRAMEBUFFER, out.framebuffer);
        gl.viewport(0, 0, out.width, out.height);
        gl.useProgram(program.program);
        gl.activeTexture(gl.TEXTURE0);
        gl.bindTexture(gl.TEXTURE_2D, step.input.texture);
        gl.uniform1i(program.uniforms.uIn, 0);
        gl.uniform2f(program.uniforms.uInSize, step.input.width, step.input.height);
        if (step.second && program.uniforms.uIn2) {
          gl.activeTexture(gl.TEXTURE1);
          gl.bindTexture(gl.TEXTURE_2D, step.second.texture);
          gl.uniform1i(program.uniforms.uIn2, 1);
          gl.uniform2f(program.uniforms.uIn2Size, step.second.width, step.second.height);
        }
        if (step.weights && program.uniforms.uW) {
          gl.activeTexture(gl.TEXTURE2);
          gl.bindTexture(gl.TEXTURE_2D, step.weights.texture);
          gl.uniform1i(program.uniforms.uW, 2);
          gl.uniform2f(program.uniforms.uWSize, step.weights.width, step.weights.height);
        }
        gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
      }
      gl.disableVertexAttribArray(0);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      return output;
    },
    read(index = model.output) {
      const target = tensor(index);
      gl.bindFramebuffer(gl.FRAMEBUFFER, target.framebuffer);
      const pixels = new Float32Array(target.width * target.height * 4);
      gl.readPixels(0, 0, target.width, target.height, gl.RGBA, gl.FLOAT, pixels);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      return { pixels, width: target.width, height: target.height, shape: shapes[index] };
    },
    dispose() {
      for (const entry of Object.values(tensors)) {
        gl.deleteTexture(entry.texture);
        gl.deleteFramebuffer(entry.framebuffer);
      }
      for (const step of steps) if (step.weights) gl.deleteTexture(step.weights.texture);
      for (const program of programs.values()) gl.deleteProgram(program.program);
      gl.deleteBuffer(quad);
    },
  };
}

const MATTE_QUAD = `
attribute vec2 aPos;
varying vec2 vUv;
void main() {
  vUv = aPos * 0.5 + 0.5;
  gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const INPUT_FRAGMENT = `
precision highp float;
uniform sampler2D uCam;
uniform vec2 uStep;
varying vec2 vUv;
void main() {
  vec3 c = texture2D(uCam, vUv + vec2(-uStep.x, -uStep.y)).rgb + texture2D(uCam, vUv + vec2(uStep.x, -uStep.y)).rgb +
    texture2D(uCam, vUv + vec2(-uStep.x, uStep.y)).rgb + texture2D(uCam, vUv + vec2(uStep.x, uStep.y)).rgb;
  gl_FragColor = vec4(c * 0.25, 0.0);
}`;

const TEMPORAL_FRAGMENT = `
precision highp float;
uniform sampler2D uRaw;
uniform sampler2D uPrev;
uniform float uFirst;
uniform float uHold;
varying vec2 vUv;
void main() {
  float raw = texture2D(uRaw, vUv).r;
  float prev = texture2D(uPrev, vUv).r;
  float change = abs(raw - prev);
  float rate = mix(uHold, 1.0, smoothstep(0.06, 0.4, change));
  float v = mix(prev, raw, uFirst > 0.5 ? 1.0 : rate);
  gl_FragColor = vec4(v, 0.0, 0.0, 1.0);
}`;

const MOMENTS_FRAGMENT = `
precision highp float;
uniform sampler2D uCam;
uniform sampler2D uMask;
uniform vec2 uStep;
varying vec2 vUv;
float luma(vec2 uv) { return dot(texture2D(uCam, uv).rgb, vec3(0.299, 0.587, 0.114)); }
void main() {
  float i = 0.25 * (luma(vUv + vec2(-uStep.x, -uStep.y)) + luma(vUv + vec2(uStep.x, -uStep.y)) +
    luma(vUv + vec2(-uStep.x, uStep.y)) + luma(vUv + vec2(uStep.x, uStep.y)));
  float p = texture2D(uMask, vUv).r;
  p = smoothstep(0.1, 0.9, p);
  gl_FragColor = vec4(i, p, i * p, i * i);
}`;

const BOX_FRAGMENT = `
precision highp float;
uniform sampler2D uSource;
uniform vec2 uStep;
varying vec2 vUv;
void main() {
  vec4 sum = vec4(0.0);
  for (int k = -RADIUS; k <= RADIUS; k++) sum += texture2D(uSource, vUv + uStep * float(k));
  gl_FragColor = sum / float(2 * RADIUS + 1);
}`;

const COEFFICIENT_FRAGMENT = `
precision highp float;
uniform sampler2D uMeans;
uniform float uEps;
varying vec2 vUv;
void main() {
  vec4 m = texture2D(uMeans, vUv);
  float variance = max(m.w - m.x * m.x, 0.0);
  float a = (m.z - m.x * m.y) / (variance + uEps);
  float b = m.y - a * m.x;
  gl_FragColor = vec4(a, b, variance / (variance + uEps * 3.0), 1.0);
}`;

const SEED_FRAGMENT = `
precision highp float;
uniform sampler2D uMask;
uniform sampler2D uPrev;
uniform vec2 uStep;
uniform vec3 uFace;
uniform float uFresh;
varying vec2 vUv;
void main() {
  float m = 0.0;
  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) m = max(m, texture2D(uMask, vUv + vec2(float(i), float(j)) * uStep).r);
  }
  float inside = smoothstep(0.25, 0.5, m);
  float seed = step(0.86, vUv.y);
  if (uFace.z > 0.0) seed = max(seed, step(length(vUv - uFace.xy), uFace.z));
  float prev = uFresh > 0.5 ? 0.0 : texture2D(uPrev, vUv).r;
  gl_FragColor = vec4(min(inside, max(seed, prev)), inside, 0.0, 1.0);
}`;

const GROW_FRAGMENT = `
precision highp float;
uniform sampler2D uSource;
uniform vec2 uStep;
varying vec2 vUv;
void main() {
  vec4 here = texture2D(uSource, vUv);
  float grown = here.r;
  grown = max(grown, texture2D(uSource, vUv + vec2(uStep.x, 0.0)).r);
  grown = max(grown, texture2D(uSource, vUv - vec2(uStep.x, 0.0)).r);
  grown = max(grown, texture2D(uSource, vUv + vec2(0.0, uStep.y)).r);
  grown = max(grown, texture2D(uSource, vUv - vec2(0.0, uStep.y)).r);
  grown = max(grown, texture2D(uSource, vUv + uStep).r);
  grown = max(grown, texture2D(uSource, vUv - uStep).r);
  grown = max(grown, texture2D(uSource, vUv + vec2(uStep.x, -uStep.y)).r);
  grown = max(grown, texture2D(uSource, vUv + vec2(-uStep.x, uStep.y)).r);
  gl_FragColor = vec4(min(here.g, grown), here.g, 0.0, 1.0);
}`;

const CLEAN_FRAGMENT = `
precision highp float;
uniform sampler2D uMask;
uniform sampler2D uKeep;
uniform vec2 uStep;
varying vec2 vUv;
void main() {
  float keep = 0.0;
  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) keep = max(keep, texture2D(uKeep, vUv + vec2(float(i), float(j)) * uStep).r);
  }
  gl_FragColor = vec4(texture2D(uMask, vUv).r * smoothstep(0.05, 0.4, keep), 0.0, 0.0, 1.0);
}`;

export const MATTE_SAMPLE = `
uniform sampler2D uMatteMask;
float callMatte(sampler2D coefficients, vec3 color, vec2 uv) {
  vec3 abc = texture2D(coefficients, uv).rgb;
  float i = dot(color, vec3(0.299, 0.587, 0.114));
  float guided = clamp(abc.x * i + abc.y, 0.0, 1.0);
  float hard = smoothstep(0.3, 0.7, texture2D(uMatteMask, uv).r);
  return mix(hard, guided, smoothstep(0.15, 0.6, abc.z));
}`;

const REGION_WIDTH = 64;
const GROW_ROUNDS = 8;

export function chooseModel(models, width, height) {
  const landscape = width / Math.max(1, height) > 1.2;
  return (landscape ? models.landscape || models.square : models.square || models.landscape) || null;
}

export function createMatte(gl, models, { format = floatTargets(gl), guideWidth: initialGuide = 320, radius = 4, eps = 0.0012, hold = 0.55 } = {}) {
  let guideWidth = initialGuide;
  if (!format || !(models.landscape || models.square)) return null;
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const programs = {
    input: compileProgram(gl, MATTE_QUAD, INPUT_FRAGMENT),
    temporal: compileProgram(gl, MATTE_QUAD, TEMPORAL_FRAGMENT),
    seed: compileProgram(gl, MATTE_QUAD, SEED_FRAGMENT),
    grow: compileProgram(gl, MATTE_QUAD, GROW_FRAGMENT),
    clean: compileProgram(gl, MATTE_QUAD, CLEAN_FRAGMENT),
    moments: compileProgram(gl, MATTE_QUAD, MOMENTS_FRAGMENT),
    box: compileProgram(gl, MATTE_QUAD, BOX_FRAGMENT.replace(/RADIUS/g, String(radius))),
    coefficients: compileProgram(gl, MATTE_QUAD, COEFFICIENT_FRAGMENT),
  };
  const networks = new Map();
  const target = (width, height, filter = gl.LINEAR) => {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, width, height, 0, gl.RGBA, format.type, null);
    const framebuffer = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { texture, framebuffer, width, height };
  };
  const host = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, host);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  let hostMask = null;
  let state = null;
  let guide = null;
  let regions = null;
  let regionsFresh = true;
  let first = true;
  let lastModel = null;
  let ran = 0;

  function network(model) {
    if (!networks.has(model)) networks.set(model, createNetwork(gl, model, format));
    return networks.get(model);
  }

  function draw(program, out, textures, uniforms = {}) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, out.framebuffer);
    gl.viewport(0, 0, out.width, out.height);
    gl.useProgram(program.program);
    textures.forEach(([name, texture], unit) => {
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.uniform1i(program.uniforms[name], unit);
    });
    for (const [name, value] of Object.entries(uniforms)) {
      if (!program.uniforms[name]) continue;
      if (Array.isArray(value) && value.length === 3) gl.uniform3f(program.uniforms[name], value[0], value[1], value[2]);
      else if (Array.isArray(value)) gl.uniform2f(program.uniforms[name], value[0], value[1]);
      else gl.uniform1f(program.uniforms[name], value);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.disableVertexAttribArray(0);
  }

  function ensureState(width, height) {
    if (state && state.width === width && state.height === height) return;
    state = { width, height, ping: target(width, height), pong: target(width, height), clean: target(width, height) };
    first = true;
    const rw = REGION_WIDTH;
    const rh = Math.max(4, Math.round((REGION_WIDTH * height) / width));
    regions = { width: rw, height: rh, ping: target(rw, rh), pong: target(rw, rh), keep: target(rw, rh) };
    regionsFresh = true;
  }

  function connect(face) {
    const step = [1 / regions.width, 1 / regions.height];
    draw(programs.seed, regions.ping, [["uMask", state.ping.texture], ["uPrev", regions.keep.texture]], {
      uStep: [0.5 / regions.width, 0.5 / regions.height],
      uFresh: regionsFresh ? 1 : 0,
      uFace: face || [0, 0, -1],
    });
    for (let round = 0; round < GROW_ROUNDS; round += 1) {
      draw(programs.grow, regions.pong, [["uSource", regions.ping.texture]], { uStep: step });
      [regions.ping, regions.pong] = [regions.pong, regions.ping];
    }
    [regions.keep, regions.ping] = [regions.ping, regions.keep];
    regionsFresh = false;
    draw(programs.clean, state.clean, [["uMask", state.ping.texture], ["uKeep", regions.keep.texture]], { uStep: step });
  }

  function ensureGuide(width, height) {
    const gw = Math.min(guideWidth, width);
    const gh = Math.max(2, Math.round((gw * height) / width));
    if (guide && guide.width === gw && guide.height === gh) return;
    guide = { width: gw, height: gh, a: target(gw, gh), b: target(gw, gh), coefficients: target(gw, gh) };
  }

  return {
    get ran() {
      return ran;
    },
    get coefficients() {
      return guide?.coefficients.texture || null;
    },
    get mask() {
      return state?.clean.texture || null;
    },
    get ready() {
      return Boolean(guide && state && !first);
    },
    setHostMask(bytes, width, height) {
      gl.bindTexture(gl.TEXTURE_2D, host);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, width, height, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, bytes);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
      hostMask = { width, height, fresh: true };
    },
    clearHostMask() {
      hostMask = null;
    },
    setGuideWidth(next) {
      guideWidth = Math.max(96, Math.round(next));
    },
    reset() {
      first = true;
    },
    update(camera, width, height, { runNetwork = true, face = null } = {}) {
      gl.disable(gl.BLEND);
      let raw = null;
      let rawWidth = 0;
      let rawHeight = 0;
      if (hostMask) {
        if (hostMask.fresh || first) {
          raw = host;
          rawWidth = hostMask.width;
          rawHeight = hostMask.height;
          hostMask.fresh = false;
        }
      } else {
        const model = chooseModel(models, width, height);
        if (model !== lastModel) {
          lastModel = model;
          first = true;
        }
        if (runNetwork || first) {
          const net = network(model);
          draw(programs.input, net.input, [["uCam", camera]], {
            uStep: [0.25 * (width / net.inputWidth) / width, 0.25 * (height / net.inputHeight) / height],
          });
          net.run();
          raw = net.output.texture;
          rawWidth = net.inputWidth;
          rawHeight = net.inputHeight;
          ran += 1;
        }
      }
      if (raw) {
        ensureState(rawWidth, rawHeight);
        draw(programs.temporal, state.pong, [["uRaw", raw], ["uPrev", state.ping.texture]], { uFirst: first ? 1 : 0, uHold: hold });
        [state.ping, state.pong] = [state.pong, state.ping];
        first = false;
      }
      if (!state) return false;
      connect(face);
      ensureGuide(width, height);
      draw(programs.moments, guide.a, [["uCam", camera], ["uMask", state.clean.texture]], {
        uStep: [0.25 * (width / guide.width) / width, 0.25 * (height / guide.height) / height],
      });
      draw(programs.box, guide.b, [["uSource", guide.a.texture]], { uStep: [1 / guide.width, 0] });
      draw(programs.box, guide.a, [["uSource", guide.b.texture]], { uStep: [0, 1 / guide.height] });
      draw(programs.coefficients, guide.b, [["uMeans", guide.a.texture]], { uEps: eps });
      draw(programs.box, guide.a, [["uSource", guide.b.texture]], { uStep: [1 / guide.width, 0] });
      draw(programs.box, guide.coefficients, [["uSource", guide.a.texture]], { uStep: [0, 1 / guide.height] });
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      return true;
    },
    dispose() {
      for (const net of networks.values()) net.dispose();
      gl.deleteTexture(host);
      for (const entry of [state?.ping, state?.pong, state?.clean, regions?.ping, regions?.pong, regions?.keep, guide?.a, guide?.b, guide?.coefficients]) {
        if (!entry) continue;
        gl.deleteTexture(entry.texture);
        gl.deleteFramebuffer(entry.framebuffer);
      }
      for (const program of Object.values(programs)) gl.deleteProgram(program.program);
      gl.deleteBuffer(quad);
    },
  };
}

const HAIR_CROP_FRAGMENT = `
precision highp float;
uniform sampler2D uCam;
uniform sampler2D uPrev;
uniform vec2 uCenter;
uniform vec2 uSide;
uniform vec2 uFrame;
uniform vec2 uSize;
varying vec2 vUv;
void main() {
  vec2 p = uCenter + (vUv - 0.5) * uSide;
  vec2 uv = p / uFrame;
  bool inside = uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
  vec2 step = uSide / uSize / uFrame / 3.0;
  vec3 c = vec3(0.0);
  for (int i = -1; i <= 1; i++) {
    for (int j = -1; j <= 1; j++) c += texture2D(uCam, uv + vec2(float(i), float(j)) * step).rgb;
  }
  c = inside ? c / 9.0 : vec3(0.0);
  float prev = inside ? texture2D(uPrev, uv).r : 0.0;
  gl_FragColor = vec4(c, prev);
}`;

const HAIR_FRAME_FRAGMENT = `
precision highp float;
uniform sampler2D uLogits;
uniform sampler2D uPrev;
uniform vec2 uCenter;
uniform vec2 uSide;
uniform vec2 uFrame;
uniform vec2 uLogitSize;
uniform float uRate;
varying vec2 vUv;
float hairAt(vec2 t) {
  vec4 l = texture2D(uLogits, (floor(t * uLogitSize) + 0.5) / uLogitSize);
  return 1.0 / (1.0 + exp(clamp(l.x - l.y, -30.0, 30.0)));
}
void main() {
  vec2 c = (vUv * uFrame - uCenter) / uSide + 0.5;
  float prev = texture2D(uPrev, vUv).r;
  float now = 0.0;
  if (c.x >= 0.0 && c.y >= 0.0 && c.x <= 1.0 && c.y <= 1.0) {
    vec2 t = c * uLogitSize - 0.5;
    vec2 f = fract(t);
    vec2 base = floor(t);
    float a = hairAt((base + vec2(0.5, 0.5)) / uLogitSize);
    float b = hairAt((base + vec2(1.5, 0.5)) / uLogitSize);
    float d = hairAt((base + vec2(0.5, 1.5)) / uLogitSize);
    float e = hairAt((base + vec2(1.5, 1.5)) / uLogitSize);
    now = mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
  }
  float change = abs(now - prev);
  float rate = mix(uRate, 1.0, smoothstep(0.1, 0.5, change));
  gl_FragColor = vec4(mix(prev, now, rate), 0.0, 0.0, 1.0);
}`;

const WARM_ROUNDS = 3;

export function createHairMatte(gl, model, { format = floatTargets(gl), width = 256, rate = 0.5 } = {}) {
  if (!format || !model) return null;
  const net = createNetwork(gl, model, format);
  const quad = gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER, quad);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
  const crop = compileProgram(gl, MATTE_QUAD, HAIR_CROP_FRAGMENT);
  const frame = compileProgram(gl, MATTE_QUAD, HAIR_FRAME_FRAGMENT);
  const logits = net.outputs[0][0];
  let state = null;
  let fresh = true;
  let ran = 0;

  function target(w, h) {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    const framebuffer = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    return { texture, framebuffer, width: w, height: h };
  }

  function draw(program, out, textures, uniforms) {
    gl.bindFramebuffer(gl.FRAMEBUFFER, out.framebuffer);
    gl.viewport(0, 0, out.width, out.height);
    gl.useProgram(program.program);
    textures.forEach(([name, texture], unit) => {
      gl.activeTexture(gl.TEXTURE0 + unit);
      gl.bindTexture(gl.TEXTURE_2D, texture);
      gl.uniform1i(program.uniforms[name], unit);
    });
    for (const [name, value] of Object.entries(uniforms)) {
      if (!program.uniforms[name]) continue;
      if (Array.isArray(value) && value.length === 3) gl.uniform3f(program.uniforms[name], value[0], value[1], value[2]);
      else if (Array.isArray(value)) gl.uniform2f(program.uniforms[name], value[0], value[1]);
      else gl.uniform1f(program.uniforms[name], value);
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, quad);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
    gl.disableVertexAttribArray(0);
  }

  return {
    get texture() {
      return state?.ping.texture || null;
    },
    get ran() {
      return ran;
    },
    reset() {
      fresh = true;
    },
    update(camera, w, h, region, { run = true } = {}) {
      const sw = Math.min(width, w);
      const sh = Math.max(2, Math.round((sw * h) / w));
      if (!state || state.width !== sw || state.height !== sh) {
        state = { width: sw, height: sh, ping: target(sw, sh), pong: target(sw, sh) };
        fresh = true;
      }
      gl.disable(gl.BLEND);
      const uniforms = { uCenter: region.center, uSide: region.size, uFrame: [w, h] };
      const rounds = fresh ? WARM_ROUNDS : 1;
      for (let round = 0; round < rounds; round += 1) {
        if (run || fresh) {
          draw(crop, net.input, [["uCam", camera], ["uPrev", state.ping.texture]], { ...uniforms, uSize: [net.inputWidth, net.inputHeight] });
          net.run();
          ran += 1;
        }
        draw(frame, state.pong, [["uLogits", logits.texture], ["uPrev", state.ping.texture]], {
          ...uniforms,
          uLogitSize: [net.inputWidth, net.inputHeight],
          uRate: fresh ? 1 : rate,
        });
        [state.ping, state.pong] = [state.pong, state.ping];
      }
      fresh = false;
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      return state.ping.texture;
    },
    dispose() {
      net.dispose();
      for (const entry of [state?.ping, state?.pong]) {
        if (!entry) continue;
        gl.deleteTexture(entry.texture);
        gl.deleteFramebuffer(entry.framebuffer);
      }
      gl.deleteProgram(crop.program);
      gl.deleteProgram(frame.program);
      gl.deleteBuffer(quad);
    },
  };
}
