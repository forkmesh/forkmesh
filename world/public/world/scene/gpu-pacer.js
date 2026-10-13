export const COMPLETION_STATUS_KHR = 0x91b1;
export const PROGRAM_PACER_IN_FLIGHT = 1;
export const PROGRAM_PACER_FRAME_GAP = 1;

const PROGRAM_READS = [
  "getProgramInfoLog",
  "getActiveUniform",
  "getActiveAttrib",
  "getUniformLocation",
  "getAttribLocation",
  "getUniformBlockIndex",
  "getActiveUniformBlockName",
  "getActiveUniformBlockParameter",
  "getActiveUniforms",
  "uniformBlockBinding",
  "getFragDataLocation",
  "getTransformFeedbackVarying",
  "getAttachedShaders",
  "validateProgram",
  "useProgram",
  "detachShader",
];
const SHADER_READS = ["getShaderParameter", "getShaderInfoLog", "getTranslatedShaderSource"];

export function installProgramPacer(gl, {
  inFlight: maxInFlight = PROGRAM_PACER_IN_FLIGHT,
  frameGap = PROGRAM_PACER_FRAME_GAP,
} = {}) {
  const stats = {
    paced: false,
    queued: 0,
    issued: 0,
    forced: 0,
    frames: 0,
    busyFrames: 0,
  };
  const idle = {
    stats,
    hold() {},
    pace() {},
    tick() {},
    paced: (run) => run(),
    pending: () => 0,
    busy: () => false,
  };
  if (!gl || typeof gl.linkProgram !== "function") return idle;
  let parallel = null;
  try {
    parallel = gl.getExtension?.("KHR_parallel_shader_compile") || null;
  } catch {
    parallel = null;
  }
  if (!parallel) return idle;
  const original = {};
  const keep = (name) => {
    original[name] = typeof gl[name] === "function" ? gl[name] : null;
    return original[name];
  };
  const compileShader = keep("compileShader");
  const attachShader = keep("attachShader");
  const linkProgram = keep("linkProgram");
  const deleteProgram = keep("deleteProgram");
  const deleteShader = keep("deleteShader");
  const getProgramParameter = keep("getProgramParameter");
  let paced = false;
  let forcedDepth = 0;
  let frame = 0;
  let lastIssueFrame = -Infinity;
  let holdFrames = 0;
  const pendingShaders = new Set();
  const attached = new WeakMap();
  const queue = [];
  const queued = new Set();
  const flying = new Set();
  const pacing = () => paced || forcedDepth > 0;
  const compileAttached = (program) => {
    const shaders = attached.get(program);
    if (!shaders) return;
    shaders.forEach((shader) => {
      if (!pendingShaders.delete(shader)) return;
      compileShader.call(gl, shader);
    });
  };
  const issue = (program, forced) => {
    if (!queued.delete(program)) return;
    const index = queue.indexOf(program);
    if (index >= 0) queue.splice(index, 1);
    compileAttached(program);
    linkProgram.call(gl, program);
    flying.add(program);
    lastIssueFrame = frame;
    stats.issued += 1;
    if (forced) stats.forced += 1;
    stats.queued = queue.length;
  };
  const settle = () => {
    flying.forEach((program) => {
      let done = true;
      try {
        done = getProgramParameter.call(gl, program, COMPLETION_STATUS_KHR) !== false;
      } catch {
        done = true;
      }
      if (!done) return;
      flying.delete(program);
    });
    return flying.size;
  };
  gl.compileShader = function pacedCompileShader(shader) {
    if (pacing()) {
      pendingShaders.add(shader);
      return undefined;
    }
    return compileShader.call(this, shader);
  };
  gl.attachShader = function pacedAttachShader(program, shader) {
    if (program && shader) {
      let shaders = attached.get(program);
      if (!shaders) attached.set(program, (shaders = []));
      shaders.push(shader);
    }
    return attachShader.call(this, program, shader);
  };
  gl.linkProgram = function pacedLinkProgram(program) {
    if (!program) return linkProgram.call(this, program);
    if (pacing()) {
      if (!queued.has(program)) {
        queued.add(program);
        queue.push(program);
        stats.queued = queue.length;
      }
      return undefined;
    }
    compileAttached(program);
    return linkProgram.call(this, program);
  };
  gl.getProgramParameter = function pacedGetProgramParameter(program, pname) {
    if (queued.has(program)) {
      if (pname === COMPLETION_STATUS_KHR) return false;
      issue(program, true);
    }
    return getProgramParameter.call(this, program, pname);
  };
  gl.deleteProgram = function pacedDeleteProgram(program) {
    if (queued.delete(program)) {
      const index = queue.indexOf(program);
      if (index >= 0) queue.splice(index, 1);
      attached.get(program)?.forEach((shader) => pendingShaders.delete(shader));
      stats.queued = queue.length;
    }
    flying.delete(program);
    return deleteProgram.call(this, program);
  };
  gl.deleteShader = function pacedDeleteShader(shader) {
    if (pendingShaders.has(shader)) {
      queue.slice().forEach((program) => {
        if (attached.get(program)?.includes(shader)) issue(program, true);
      });
      if (pendingShaders.delete(shader)) compileShader.call(gl, shader);
    }
    return deleteShader.call(this, shader);
  };
  PROGRAM_READS.forEach((name) => {
    const read = keep(name);
    if (!read) return;
    gl[name] = function pacedProgramRead(program, ...rest) {
      if (queued.has(program)) issue(program, true);
      return read.call(this, program, ...rest);
    };
  });
  SHADER_READS.forEach((name) => {
    const read = keep(name);
    if (!read) return;
    gl[name] = function pacedShaderRead(shader, ...rest) {
      if (pendingShaders.has(shader)) {
        queue.slice().forEach((program) => {
          if (attached.get(program)?.includes(shader)) issue(program, true);
        });
        if (pendingShaders.delete(shader)) compileShader.call(gl, shader);
      }
      return read.call(this, shader, ...rest);
    };
  });
  return {
    stats,
    pace(value = true) {
      paced = value !== false;
      stats.paced = paced;
      if (!paced && forcedDepth === 0) {
        queue.slice().forEach((program) => issue(program, false));
      }
    },
    paced(run) {
      forcedDepth += 1;
      try {
        return run();
      } finally {
        forcedDepth -= 1;
      }
    },
    hold(frames = 2) {
      holdFrames = Math.max(holdFrames, frames);
    },
    tick() {
      frame += 1;
      stats.frames = frame;
      const inFlight = settle();
      if (inFlight > 0) stats.busyFrames += 1;
      if (holdFrames > 0) {
        holdFrames -= 1;
        return;
      }
      if (!queue.length) return;
      if (frame - lastIssueFrame < frameGap) return;
      if (flying.size >= maxInFlight) return;
      issue(queue[0], false);
    },
    pending: () => queue.length + flying.size,
    busy: () => flying.size > 0,
  };
}

export const UPLOAD_BUDGET_BYTES = 6 * 1024 * 1024;
export const UPLOAD_BUDGET_REFILL_MS = 1000 / 60;
const NEAREST_FILTER = 1003;
const LINEAR_FILTER = 1006;

export function textureUploadBytes(texture) {
  const image = texture?.image;
  if (!image) return 0;
  const width = Number(image.naturalWidth || image.videoWidth || image.width) || 0;
  const height = Number(image.naturalHeight || image.videoHeight || image.height) || 0;
  const depth = Math.max(1, Number(image.depth) || 1);
  const bytes = width * height * depth * 4;
  const mipmapped =
    texture.generateMipmaps !== false &&
    texture.minFilter !== LINEAR_FILTER &&
    texture.minFilter !== NEAREST_FILTER;
  return mipmapped ? Math.round((bytes * 4) / 3) : bytes;
}

export function createUploadBudget({
  bytes: capacity = UPLOAD_BUDGET_BYTES,
  refillMs = UPLOAD_BUDGET_REFILL_MS,
  now = () => performance.now(),
} = {}) {
  let level = capacity;
  let at = now();
  const stats = { granted: 0, refused: 0, grantedBytes: 0 };
  const refill = () => {
    const time = now();
    level = Math.min(capacity, level + ((time - at) / refillMs) * capacity);
    at = time;
    return level;
  };
  return {
    stats,
    take(bytes) {
      const size = Math.max(0, Number(bytes) || 0);
      if (refill() < Math.min(size, capacity)) {
        stats.refused += 1;
        return false;
      }
      level -= size;
      stats.granted += 1;
      stats.grantedBytes += size;
      return true;
    },
    takeTexture(texture) {
      return this.take(textureUploadBytes(texture));
    },
    level: () => refill(),
  };
}

export const STRIPED_UPLOAD_MIN_BYTES = 4 * 1024 * 1024;
export const STRIPED_UPLOAD_STRIPE_BYTES = 2 * 1024 * 1024;
export const STRIPED_UPLOAD_STRIPES_PER_FRAME = 2;
const RGBA_FORMAT = 1023;
const UNSIGNED_BYTE_TYPE = 1009;
const DRAW_TEXTURE_SLOTS = [
  "map",
  "emissiveMap",
  "alphaMap",
  "aoMap",
  "lightMap",
  "bumpMap",
  "normalMap",
  "roughnessMap",
  "metalnessMap",
  "specularMap",
];

export function stripeableTexture(texture) {
  const image = texture?.image;
  if (!texture?.isTexture || !image || typeof image !== "object") return false;
  if (
    texture.isDataTexture ||
    texture.isCompressedTexture ||
    texture.isCubeTexture ||
    texture.isVideoTexture ||
    texture.isRenderTargetTexture ||
    texture.isDataArrayTexture ||
    texture.isData3DTexture ||
    texture.isFramebufferTexture ||
    texture.isDepthTexture ||
    texture.isHTMLTexture
  ) {
    return false;
  }
  if (texture.format !== RGBA_FORMAT || texture.type !== UNSIGNED_BYTE_TYPE) return false;
  if (Array.isArray(texture.mipmaps) && texture.mipmaps.length) return false;
  if (image.data || image.complete === false) return false;
  const width = Number(image.width) || 0;
  const height = Number(image.height) || 0;
  if (!width || !height) return false;
  return width * height * 4 >= STRIPED_UPLOAD_MIN_BYTES;
}

export function createStripedUploads({
  renderer,
  THREE,
  budget,
  stripeBytes = STRIPED_UPLOAD_STRIPE_BYTES,
  perFrame = STRIPED_UPLOAD_STRIPES_PER_FRAME,
} = {}) {
  const gl = renderer?.getContext?.() || null;
  const state = renderer?.state || null;
  const jobs = [];
  const swaps = new Map();
  const reuploads = [];
  const uploadedVersions = new Map();
  const allocated = new WeakMap();
  const stats = {
    started: 0,
    stripes: 0,
    finished: 0,
    dropped: 0,
    swapped: 0,
    deferred: 0,
    reuploaded: 0,
  };
  const maxSize = Number(renderer?.capabilities?.maxTextureSize) || 4096;
  const usable = Boolean(gl && state && typeof state.pixelStorei === "function" &&
    typeof gl.texSubImage2D === "function" && renderer.capabilities?.isWebGL2 !== false);
  const drop = (job) => {
    const index = jobs.indexOf(job);
    if (index >= 0) jobs.splice(index, 1);
    job.texture.removeEventListener?.("dispose", job.onDispose);
  };
  const conversion = (texture) => {
    const management = THREE?.ColorManagement;
    if (!management || texture.colorSpace === THREE.NoColorSpace) return gl.NONE;
    const working = management.getPrimaries?.(management.workingColorSpace);
    const own = management.getPrimaries?.(texture.colorSpace);
    return working === own ? gl.NONE : gl.BROWSER_DEFAULT_WEBGL;
  };
  const needsMipmaps = (texture) =>
    texture.generateMipmaps !== false &&
    texture.minFilter !== LINEAR_FILTER &&
    texture.minFilter !== NEAREST_FILTER;
  const begin = (texture, onDone = null) => {
    if (!usable || !stripeableTexture(texture)) return false;
    const image = texture.image;
    if (image.width > maxSize || image.height > maxSize) return false;
    if (jobs.some((job) => job.texture === texture)) return true;
    const props = renderer.properties.get(texture);
    if (props.__webglTexture && props.__version === texture.version) return false;
    const size = `${image.width}x${image.height}`;
    if (!props.__webglTexture || props.__version === undefined) {
      texture.source.dataReady = false;
      try {
        renderer.initTexture(texture);
      } finally {
        texture.source.dataReady = true;
      }
      if (props.__webglTexture) allocated.set(props.__webglTexture, size);
    } else if (allocated.get(props.__webglTexture) !== size) {
      return false;
    }
    if (!props.__webglTexture) return false;
    props.__version = texture.version;
    const job = {
      texture,
      version: texture.version,
      webgl: props.__webglTexture,
      y: 0,
      rows: Math.max(16, Math.floor(stripeBytes / (image.width * 4))),
      onDone,
      onDispose: null,
    };
    job.onDispose = () => {
      drop(job);
      stats.dropped += 1;
    };
    texture.addEventListener?.("dispose", job.onDispose);
    jobs.push(job);
    stats.started += 1;
    return true;
  };
  const uploadStripe = (job) => {
    const { texture } = job;
    const image = texture.image;
    const rows = Math.min(job.rows, image.height - job.y);
    if (!budget.take(image.width * rows * 4)) return false;
    state.bindTexture(gl.TEXTURE_2D, job.webgl);
    state.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, texture.flipY);
    state.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, texture.premultiplyAlpha);
    state.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, conversion(texture));
    state.pixelStorei(gl.UNPACK_ALIGNMENT, texture.unpackAlignment);
    state.pixelStorei(gl.UNPACK_ROW_LENGTH, 0);
    state.pixelStorei(gl.UNPACK_SKIP_PIXELS, 0);
    state.pixelStorei(gl.UNPACK_SKIP_ROWS, job.y);
    try {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, job.y, image.width, rows, gl.RGBA, gl.UNSIGNED_BYTE, image);
    } finally {
      state.pixelStorei(gl.UNPACK_SKIP_ROWS, 0);
    }
    job.y += rows;
    stats.stripes += 1;
    if (job.y >= image.height) {
      if (needsMipmaps(texture)) gl.generateMipmap(gl.TEXTURE_2D);
      drop(job);
      stats.finished += 1;
      job.onDone?.(texture);
    }
    return true;
  };
  const deferDrawUploads = (material) => {
    if (!material || !renderer.properties?.has) return 0;
    let deferred = 0;
    for (let index = 0; index < DRAW_TEXTURE_SLOTS.length; index += 1) {
      const texture = material[DRAW_TEXTURE_SLOTS[index]];
      if (!texture?.isTexture || texture.isVideoTexture || texture.isRenderTargetTexture) continue;
      if (!renderer.properties.has(texture)) continue;
      const props = renderer.properties.get(texture);
      if (!props.__webglTexture || props.__version === undefined) continue;
      if (props.__version === texture.version) continue;
      if (uploadedVersions.has(texture)) {
        props.__version = texture.version;
        continue;
      }
      if (begin(texture)) continue;
      if (budget.take(textureUploadBytes(texture))) continue;
      uploadedVersions.set(texture, props.__version);
      props.__version = texture.version;
      reuploads.push(texture);
      stats.deferred += 1;
      deferred += 1;
    }
    return deferred;
  };
  const reupload = () => {
    while (reuploads.length) {
      const texture = reuploads[0];
      if (!renderer.properties.has(texture)) {
        reuploads.shift();
        uploadedVersions.delete(texture);
        continue;
      }
      if (!budget.take(textureUploadBytes(texture))) return;
      reuploads.shift();
      const props = renderer.properties.get(texture);
      props.__version = uploadedVersions.get(texture);
      uploadedVersions.delete(texture);
      renderer.initTexture(texture);
      stats.reuploaded += 1;
    }
  };
  const step = (limit = perFrame) => {
    reupload();
    let uploaded = 0;
    while (jobs.length && uploaded < limit) {
      const job = jobs[0];
      const props = renderer.properties.get(job.texture);
      if (job.texture.version !== job.version || props.__webglTexture !== job.webgl) {
        drop(job);
        stats.dropped += 1;
        continue;
      }
      if (!uploadStripe(job)) break;
      uploaded += 1;
    }
    return uploaded;
  };
  const swapMap = (material, texture, onSwap = null) => {
    if (!material) return false;
    const pending = swaps.get(material);
    if (pending && pending !== texture) {
      swaps.delete(material);
      pending.dispose?.();
    }
    const assign = () => {
      if (swaps.get(material) !== texture) return;
      swaps.delete(material);
      const previous = material.map;
      material.map = texture;
      material.needsUpdate = true;
      if (previous && previous !== texture) previous.dispose?.();
      stats.swapped += 1;
      onSwap?.(texture);
    };
    swaps.set(material, texture);
    if (!begin(texture, assign)) {
      assign();
      return false;
    }
    return true;
  };
  return {
    stats,
    begin,
    step,
    swapMap,
    deferDrawUploads,
    pending: () => jobs.length + reuploads.length,
    active: (texture) => jobs.some((job) => job.texture === texture),
    accepts: (texture) =>
      usable &&
      stripeableTexture(texture) &&
      texture.image.width <= maxSize &&
      texture.image.height <= maxSize,
  };
}
