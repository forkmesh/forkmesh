export const SEGMENT_MODEL_BASE = "/assets/call-segmentation/";
export const SEGMENT_MODELS = Object.freeze({ landscape: "selfie-landscape.fmnn", square: "selfie-square.fmnn" });

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
  const shapes = {};
  for (const [key, value] of Object.entries(header.shapes)) shapes[key] = value;
  return { ...header, shapes, weights };
}

export function decodeBase64(text) {
  const raw = globalThis.atob(text);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i += 1) out[i] = raw.charCodeAt(i);
  return out;
}

export async function loadSegmentModels({ fetchImpl = globalThis.fetch?.bind(globalThis), base = SEGMENT_MODEL_BASE } = {}) {
  const inline = globalThis.FmCallSegmentModels;
  const out = {};
  for (const [name, file] of Object.entries(SEGMENT_MODELS)) {
    try {
      if (inline?.[name]) {
        out[name] = parseSegmentModel(decodeBase64(inline[name]));
      } else if (fetchImpl) {
        const response = await fetchImpl(base + file);
        if (response.ok) out[name] = parseSegmentModel(await response.arrayBuffer());
      }
    } catch (_) {}
  }
  return out;
}

let modelPromise = null;

export function sharedSegmentModels(options) {
  if (!modelPromise) modelPromise = loadSegmentModels(options);
  return modelPromise;
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

function activation(name) {
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

export function layerShader(op, inShape, outShape, secondShape = null) {
  const [inH, inW, inC] = inShape;
  const [outH, outW, outC] = outShape;
  const si = slices(inC);
  const so = slices(outC);
  let body = "";
  const parts = [HEAD, fetchIn("a", "uInSize", inW, inH)];
  if (op.op === "conv" || op.op === "tconv") {
    parts.push(weightAt());
    const [kh, kw] = op.kernel;
    const [sh, sw] = op.stride;
    const [pt, pl] = op.pad || [0, 0];
    const position = op.op === "conv"
      ? `float ix = x * ${sw.toFixed(1)} + ${kx(sw, pl)}; float iy = y * ${sh.toFixed(1)} + ${ky(sh, pt)};`
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
  } else {
    throw new Error(`unsupported layer ${op.op}`);
  }
  parts.push(prelude(outW), body, `
  ${activation(op.act)}
  gl_FragColor = v;
}`);
  return parts.join("\n");
}

function kx(stride, pad) {
  return `float(kx) - ${pad.toFixed(1)}`;
}

function ky(stride, pad) {
  return `float(ky) - ${pad.toFixed(1)}`;
}

export function packWeights(op, weights) {
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
    const source = layerShader(layer, shapes[layer.in[0]], shapes[layer.out], second);
    if (!programs.has(source)) programs.set(source, compileProgram(gl, QUAD, source));
    let weights = null;
    if (layer.w) {
      const packed = packWeights(layer, model.weights);
      weights = { texture: makeTexture(packed.width, packed.height, toWeightData(packed.data)), width: packed.width, height: packed.height };
    }
    return { layer, program: programs.get(source), weights, input: tensor(layer.in[0]), second: layer.in[1] !== undefined ? tensor(layer.in[1]) : null, output: tensor(layer.out) };
  });
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  const input = tensor(model.input);
  const output = tensor(model.output);
  const [inH, inW] = shapes[model.input];

  return {
    input,
    output,
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
