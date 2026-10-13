import { settledMatrixWalk } from "./settled-matrices.js";

export const RENDER_STALL_MIN_MS = 50;
export const RENDER_STALL_HISTORY = 24;
export const FIRST_SIGHT_HISTORY = 64;
export const SCENE_UPDATE_GROUPS = Object.freeze([
  ["matrix walk", ["matrices"]],
  ["animation", ["frame-animations", "simulation", "avatars", "effects", "ambient", "mirror"]],
  ["labels", ["labels"]],
  ["camera & proximity", ["camera", "proximity"]],
  ["housekeeping", ["housekeeping"]],
]);

const UPLOAD_METHODS = [
  "texImage2D",
  "texSubImage2D",
  "texImage3D",
  "texSubImage3D",
  "compressedTexImage2D",
  "compressedTexSubImage2D",
  "copyTexSubImage2D",
  "copyTexSubImage3D",
  "generateMipmap",
];
const WAIT_METHODS = [
  "getProgramParameter",
  "getShaderParameter",
  "getProgramInfoLog",
  "getShaderInfoLog",
  "getActiveUniform",
  "getUniformLocation",
  "getActiveAttrib",
  "getAttribLocation",
];
const COMPILE_METHODS = ["compileShader", "linkProgram"];

function uploadBytes(args) {
  for (let index = args.length - 1; index >= 0; index -= 1) {
    const value = args[index];
    if (!value || typeof value !== "object") continue;
    const width = Number(value.naturalWidth || value.videoWidth || value.width) || 0;
    const height = Number(value.naturalHeight || value.videoHeight || value.height) || 0;
    if (width && height) return width * height * 4;
    if (Number.isFinite(value.byteLength)) return value.byteLength;
  }
  return 0;
}

function zeroFrame() {
  return {
    uploads: 0,
    uploadMs: 0,
    uploadBytes: 0,
    uploadLabels: new Map(),
    programs: [],
    compileMs: 0,
    waitMs: 0,
    shadowDraws: 0,
    shadowMs: 0,
    otherDraws: 0,
    otherMs: 0,
  };
}

function top(map, limit, format) {
  return [...map.entries()]
    .sort((a, b) => b[1].ms - a[1].ms)
    .slice(0, limit)
    .map(([label, entry]) => format(label, entry));
}

export function createRenderStallMonitor({
  renderer,
  camera,
  ownerOf = () => "",
  isWarming = () => false,
  now = () => performance.now(),
} = {}) {
  let frame = zeroFrame();
  let draw = null;
  let phases = new Map();
  let markAt = now();
  let walkVisited = settledMatrixWalk.visited;
  let walkComposed = settledMatrixWalk.composed;
  const sceneUpdateParts = () => {
    const parts = [];
    for (const [label, ids] of SCENE_UPDATE_GROUPS) {
      let ms = 0;
      for (const id of ids) ms += phases.get(id) || 0;
      if (ms >= 1) parts.push({ label, ms });
    }
    return parts.sort((a, b) => b.ms - a.ms);
  };
  const current = {
    pass: "main",
    material: null,
    object: null,
    startedAt: 0,
  };
  const drawLabel = ({ material, object }) => {
    let element = "";
    try {
      element = ownerOf(object) || "";
    } catch {
      element = "";
    }
    return `${element || "unregistered"} · ${material?.name || object?.name || material?.type || "material"}`.slice(0, 80);
  };
  let hookedPrograms = null;
  let sequence = 0;
  const stalls = [];
  const firstSight = [];
  const totals = {
    frames: 0,
    stalls: 0,
    worstMs: 0,
    firstSight: 0,
    uploads: 0,
    uploadMs: 0,
    compileWaitMs: 0,
  };
  const gl = renderer?.getContext?.() || null;
  const wrap = (names, record) => {
    if (!gl) return;
    for (const name of names) {
      const original = gl[name];
      if (typeof original !== "function" || original.__forkmeshStallHook) continue;
      const hooked = function renderStallHook(...args) {
        const startedAt = now();
        try {
          return original.apply(this, args);
        } finally {
          record(name, now() - startedAt, args);
        }
      };
      hooked.__forkmeshStallHook = true;
      gl[name] = hooked;
    }
  };
  wrap(UPLOAD_METHODS, (name, ms, args) => {
    const bytes = name === "generateMipmap" ? 0 : uploadBytes(args);
    frame.uploadMs += ms;
    if (name === "generateMipmap") return;
    frame.uploads += 1;
    frame.uploadBytes += bytes;
    totals.uploads += 1;
    totals.uploadMs += ms;
    const label = draw ? drawLabel(draw) : "outside a draw";
    const entry = frame.uploadLabels.get(label) || { ms: 0, bytes: 0, count: 0 };
    entry.ms += ms;
    entry.bytes += bytes;
    entry.count += 1;
    frame.uploadLabels.set(label, entry);
  });
  wrap(WAIT_METHODS, (_name, ms) => {
    frame.waitMs += ms;
    totals.compileWaitMs += ms;
  });
  wrap(COMPILE_METHODS, (_name, ms) => {
    frame.compileMs += ms;
  });
  const hookPrograms = () => {
    const programs = renderer?.info?.programs;
    if (!Array.isArray(programs) || programs === hookedPrograms) return;
    hookedPrograms = programs;
    const push = programs.push;
    programs.push = function renderStallPrograms(...items) {
      if (draw && !isWarming()) {
        for (const program of items) noteFirstSight(program);
      }
      return push.apply(this, items);
    };
  };
  const noteFirstSight = (program) => {
    const { material, object, pass } = draw;
    let element = "";
    try {
      element = ownerOf(object) || "";
    } catch {
      element = "";
    }
    const entry = {
      at: Date.now(),
      program: String(program?.name || ""),
      type: String(material?.type || program?.name || "material"),
      material: String(material?.name || "").slice(0, 48),
      object: String(object?.name || object?.parent?.name || "").slice(0, 48),
      element: String(element).slice(0, 64),
      pass,
      sequence: ++sequence,
    };
    firstSight.push(entry);
    if (firstSight.length > FIRST_SIGHT_HISTORY) firstSight.shift();
    totals.firstSight += 1;
    frame.programs.push(entry);
  };
  hookPrograms();
  return {
    beginDraw(drawCamera, drawScene, material, object) {
      const pass = drawScene === null ? "shadow" : drawCamera === camera ? "main" : "other";
      current.pass = pass;
      current.material = material;
      current.object = object;
      if (pass !== "main") current.startedAt = now();
      draw = current;
    },
    endDraw() {
      if (draw?.pass === "shadow") {
        frame.shadowDraws += 1;
        frame.shadowMs += now() - draw.startedAt;
      } else if (draw?.pass === "other") {
        frame.otherDraws += 1;
        frame.otherMs += now() - draw.startedAt;
      }
      current.material = null;
      current.object = null;
      draw = null;
    },
    mark(phase) {
      const at = now();
      phases.set(phase, (phases.get(phase) || 0) + at - markAt);
      markAt = at;
    },
    beginFrame() {
      hookPrograms();
      if (phases.size) phases = new Map();
      walkVisited = settledMatrixWalk.visited;
      walkComposed = settledMatrixWalk.composed;
      markAt = now();
      if (frame.uploads || frame.programs.length || frame.uploadMs || frame.waitMs ||
        frame.compileMs || frame.shadowDraws || frame.otherDraws) {
        frame = zeroFrame();
      }
    },
    endFrame(frameMs, renderMs = 0) {
      totals.frames += 1;
      if (!(frameMs >= RENDER_STALL_MIN_MS)) return null;
      const programNames = frame.programs
        .slice(0, 4)
        .map((entry) => `${entry.type} · ${entry.element || entry.object || "unregistered"}`);
      if (frame.programs.length > 4) programNames.push(`+${frame.programs.length - 4} more`);
      const stall = {
        at: Date.now(),
        sequence: ++sequence,
        frameMs,
        renderMs,
        programs: frame.programs.length,
        programNames,
        compileMs: frame.compileMs,
        waitMs: frame.waitMs,
        shadowDraws: frame.shadowDraws,
        shadowMs: frame.shadowMs,
        otherDraws: frame.otherDraws,
        otherMs: frame.otherMs,
        updateParts: sceneUpdateParts(),
        matrixVisited: settledMatrixWalk.visited - walkVisited,
        matrixComposed: settledMatrixWalk.composed - walkComposed,
        uploads: frame.uploads,
        uploadMs: frame.uploadMs,
        uploadBytes: frame.uploadBytes,
        uploadNames: top(
          frame.uploadLabels,
          3,
          (label, entry) => `${label} ${Math.round(entry.ms)} ms`,
        ),
      };
      stalls.push(stall);
      if (stalls.length > RENDER_STALL_HISTORY) stalls.shift();
      totals.stalls += 1;
      totals.worstMs = Math.max(totals.worstMs, frameMs);
      return stall;
    },
    snapshot() {
      return {
        sequence,
        totals: { ...totals },
        stalls: stalls.slice(),
        firstSight: firstSight.slice(),
      };
    },
  };
}
