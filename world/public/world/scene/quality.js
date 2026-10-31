import { OFFICE_DEPTH, OFFICE_ISLAND_CENTER } from "../world-office-tower.js";
import {
  BEACH_CENTER_X,
  BEACH_CENTER_Z,
  LEADERBOARD_ISLAND_CENTER_X,
  MEMBER_ISLAND_CENTER_Z,
  REPOSITORY_ISLAND_CENTER_X,
} from "./layout.js";

// Bound the *total* drawing-buffer area, not just DPR. A 4K display at DPR 1
// is still 8.3 million pixels; with a multisampled color/depth buffer that can
// consume hundreds of MB before a single World texture is counted. 1080p is
// enough detail for this wide camera and keeps every object in the scene.
export const RENDER_PIXEL_BUDGET = 1920 * 1080;
// The ordinary playable town view must stay comfortably below the point where
// browser/driver overhead begins to dominate a frame. This is deliberately a
// renderer-workload budget rather than an FPS target: unlike wall-clock FPS it
// remains comparable across CI, software WebGL, and real GPUs. The critical
// browser benchmark exercises it while walking at the default spawn.
export const WORLD_RENDER_WORKLOAD_BUDGET = Object.freeze({
  drawCalls: 450,
  triangles: 750_000,
});
export const RENDER_STALL_THRESHOLD_MS = 500;
// DevTools console work is surprisingly expensive while WebGL is already
// behind. Aggregate repeated stalls and emit at most one compact warning per
// window instead of making a slow frame slower once every second.
export const RENDER_STALL_LOG_COOLDOWN_MS = 5 * 60_000;
export const MOVEMENT_INPUT_LOG_COOLDOWN_MS = 5 * 60_000;
// Keep runtime diagnostics aligned with the browser presentation budget.
export const MOVEMENT_INPUT_DELAY_THRESHOLD_MS = 100;
export const SHADOW_MAP_UPDATE_MS = 10_000;
export const SHADOW_MAP_BUSY_RETRY_MS = 1_000;
export const SHADOW_MAP_STALL_COOLDOWN_MS = 10_000;
export const SHADOW_MAP_DEFERRED_RETRY_MS = 400;
export const DIAGNOSTICS_TOP_ELEMENTS_MS = 60_000;
export const SCENE_COMPLEXITY_SLICE_OBJECTS = 400;
export const SCENE_LOD_SAMPLE_MS = 500;
// Compact renderers keep only the district around the visitor resident on the
// GPU. The enter/exit gap prevents a walker near a boundary from repeatedly
// uploading and releasing the same buffers.
export const COMPACT_DISTRICT_RESIDENCY = Object.freeze({
  repositories: Object.freeze({
    x: REPOSITORY_ISLAND_CENTER_X,
    z: 0,
    enter: 100,
    exit: 122,
  }),
  leaderboards: Object.freeze({
    x: LEADERBOARD_ISLAND_CENTER_X,
    z: 0,
    enter: 100,
    exit: 122,
  }),
  members: Object.freeze({
    x: 0,
    z: MEMBER_ISLAND_CENTER_Z,
    enter: 100,
    exit: 122,
  }),
  office: Object.freeze({
    x: OFFICE_ISLAND_CENTER[0],
    z: OFFICE_ISLAND_CENTER[2],
    enter: OFFICE_DEPTH / 2 + 105,
    exit: OFFICE_DEPTH / 2 + 130,
  }),
  beach: Object.freeze({
    x: BEACH_CENTER_X,
    z: BEACH_CENTER_Z,
    enter: 120,
    exit: 145,
  }),
});
export const COMPACT_DISTRICT_VIEW_ENTER_MARGIN = 12;
export const COMPACT_DISTRICT_VIEW_EXIT_MARGIN = 36;
export const AVATAR_HIGHLIGHT_SAMPLE_MS = 100;
// setAnimationLoop follows the panel refresh rate. Rendering this procedural
// scene at 120/144Hz spends twice the GPU work for almost no perceptible gain,
// so simulation and presentation share a stable 60Hz ceiling. Slower displays
// still present every callback they provide.
export const RENDER_FRAME_INTERVAL_MS = 1000 / 60;
// The same scene framed as the band at the foot of a marketing page is a
// preview, not a session: nobody is steering it, and every frame it draws is
// GPU work the browser cannot spend compositing the page being scrolled past
// it. Halve the cadence until a visitor actually clicks into the band.
export const PREVIEW_RENDER_FRAME_INTERVAL_MS = 1000 / 30;
export const RENDER_FRAME_EARLY_TOLERANCE_MS = 0.75;
// A DPR-only cap still lets a large desktop allocate six to ten million
// colour/depth samples per frame. Bound the actual drawing buffer to roughly
// 1200p while retaining native resolution on ordinary 1080p displays.
const DESKTOP_RENDER_PIXEL_BUDGET = RENDER_PIXEL_BUDGET;
// Backstop for the Debug tab's per-object walk so a pathological scene can
// never build a million-entry array on the main thread.
export const SCENE_OBJECT_WALK_LIMIT = 5_000;
// The same backstop for one expanded level of the element tree: a single
// parent with more children than this reports the first slice, and the panel
// says how many were left out.
export const ELEMENT_PART_LIMIT = 500;
// Movement, camera controls, and rendering retain display cadence. Decorative
// callbacks have their own budget: compact GPUs update them at 30 Hz and a
// zoomed-out overview at 20 Hz, where sub-pixel fire/foliage changes cannot
// justify running every shader-adjacent CPU update on every frame.
const VISUAL_ANIMATION_COMPACT_MS = 1000 / 30;
export const VISUAL_ANIMATION_FAR_MS = 1000 / 20;
const VISUAL_ANIMATION_DESKTOP_MS = 1000 / 30;
export const REMOTE_LABEL_MAX_DISTANCE_SQ = 60 * 60;
export const REMOTE_DETAIL_NEAR_DISTANCE_SQ = 45 * 45;
const LOCAL_POINT_LIGHT_BUDGET_DESKTOP = 4;
const LOCAL_POINT_LIGHT_BUDGET_COMPACT = 2;
const LOCAL_POINT_LIGHT_BUDGET_LOW = 1;
// One rain/snow buffer is allocated at full size and drawn through a per-tier
// draw range, because the recurring cost is the CPU integration loop and the
// rasterised points, not the 8 KB of positions.
export const WEATHER_PARTICLE_COUNT = 700;
// ── Device tiers ───────────────────────────────────────────────────────────
// The renderer used to recognise exactly two device classes: "coarse pointer"
// and everything else. That one question cannot separate a flagship phone
// from a 3 GB Android on an old Mali, nor a workstation from a fanless laptop
// that has fallen back to software GL, so each half of the range was tuned
// for its strong end and its weak end dragged. These four tiers are named for
// the reference device each is calibrated against, ordered strongest first,
// and every quality knob the scene can lower hangs off them.
export const WORLD_QUALITY_TIERS = Object.freeze([
  "desktop-high",
  "laptop-mid",
  "mobile-mid",
  "mobile-low",
]);
// Knobs split into two kinds. Construction-time knobs (antialias, canvas
// plate scale, star and satellite populations) decide allocations the scene
// cannot rebuild against a live context, so only the boot tier reads them.
// The rest are live: the adaptive ladder below re-applies them mid-session.
export const WORLD_QUALITY_PROFILES = Object.freeze({
  "desktop-high": Object.freeze({
    tier: "desktop-high",
    label: "High-end desktop",
    compact: false,
    antialias: true,
    shadows: true,
    pixelRatioCap: 1.25,
    pixelBudget: RENDER_PIXEL_BUDGET,
    canvasTextureScale: 1,
    starCount: 1100,
    maxSatellites: 160,
    weatherParticles: WEATHER_PARTICLE_COUNT,
    pointLightBudget: LOCAL_POINT_LIGHT_BUDGET_DESKTOP,
    visualAnimationMs: VISUAL_ANIMATION_DESKTOP_MS,
    lodDistanceScale: 1,
    farLodZoom: 1.32,
  }),
  "laptop-mid": Object.freeze({
    tier: "laptop-mid",
    label: "Mid-tier laptop",
    compact: false,
    // Integrated graphics pay for multisampling out of the same memory
    // bandwidth the scene needs for its plates, and this camera is wide
    // enough that the resolution cap below hides the lost edge quality.
    antialias: false,
    shadows: true,
    pixelRatioCap: 1.1,
    pixelBudget: 1600 * 900,
    canvasTextureScale: 0.5,
    starCount: 900,
    maxSatellites: 120,
    weatherParticles: 520,
    pointLightBudget: 3,
    visualAnimationMs: VISUAL_ANIMATION_DESKTOP_MS,
    lodDistanceScale: 0.85,
    farLodZoom: 1.32,
  }),
  "mobile-mid": Object.freeze({
    tier: "mobile-mid",
    label: "Mid-tier mobile",
    compact: true,
    antialias: false,
    shadows: false,
    pixelRatioCap: 1,
    pixelBudget: 1280 * 720,
    canvasTextureScale: 0.25,
    starCount: 700,
    maxSatellites: 80,
    weatherParticles: 420,
    pointLightBudget: LOCAL_POINT_LIGHT_BUDGET_COMPACT,
    visualAnimationMs: VISUAL_ANIMATION_COMPACT_MS,
    lodDistanceScale: 0.55,
    farLodZoom: 1.12,
  }),
  "mobile-low": Object.freeze({
    tier: "mobile-low",
    label: "Low-tier mobile",
    compact: true,
    antialias: false,
    shadows: false,
    // Below one device pixel per CSS pixel. A 720p phone panel then shades
    // about a third of the samples it asked for, which is the difference
    // between a world that walks and one that stutters on this hardware.
    pixelRatioCap: 0.75,
    pixelBudget: 960 * 540,
    canvasTextureScale: 0.18,
    starCount: 260,
    // Every tracked satellite is an SGP4 propagation per sky tick on the main
    // thread, so this population is CPU work, not just draw calls.
    maxSatellites: 24,
    weatherParticles: 200,
    pointLightBudget: LOCAL_POINT_LIGHT_BUDGET_LOW,
    visualAnimationMs: VISUAL_ANIMATION_FAR_MS,
    lodDistanceScale: 0.4,
    farLodZoom: 0.95,
  }),
});
// A software rasteriser outranks every other signal: it is slower than any
// real GPU in this table whatever machine is around it.
const SOFTWARE_RENDERER_PATTERN =
  /swiftshader|llvmpipe|softpipe|software|microsoft basic|generic renderer/;
// Mobile GPU families that predate the performance this scene assumes, plus
// the integrated desktop parts that behave like them. Matched against the
// lower-cased unmasked renderer string, which many browsers withhold — an
// absent name simply does not vote.
const LOW_TIER_GPU_PATTERN =
  /mali-4|mali-t|mali-g3[0-9]|mali-g5[0-9]|mali-g6[0-9]|adreno \(tm\) [2-5][0-9][0-9]|powervr|videocore|u?hd graphics/;
// Four cores or 3 GB is where this scene stops fitting comfortably on a
// phone; the desktop side allows one more of each before it steps down,
// because a 4-core laptop still has an active cooling budget behind it.
const LOW_TIER_MOBILE_CORES = 4;
const LOW_TIER_MOBILE_MEMORY_GB = 3;
const LOW_TIER_DESKTOP_CORES = 4;
const LOW_TIER_DESKTOP_MEMORY_GB = 4;
// A window presenting under ~62% of the cadence the loop asked for is the
// difference between "smooth" and "dragging" on the devices this ladder
// exists for. Recovery needs a comfortable 90% across three consecutive
// windows so the world never climbs straight back into the frame rate that
// pushed it down.
const QUALITY_DOWNGRADE_RATIO = 0.62;
const QUALITY_UPGRADE_RATIO = 0.9;
export const QUALITY_SAMPLE_WINDOW_MS = 4_000;
const QUALITY_SETTLE_MS = 6_000;
const QUALITY_RECOVER_DWELL_MS = 30_000;
const QUALITY_UPGRADE_WINDOWS = 3;
// Two steps from the boot tier reaches mobile-low from a mid laptop. Further
// than that and the detection was simply wrong; dropping the whole ladder on
// one bad window would hide that instead of reporting it.
const QUALITY_MAX_AUTOMATIC_STEPS = 2;
export const QUALITY_CHANGE_LOG_LIMIT = 12;

export function worldQualityProfile(tier) {
  return (
    WORLD_QUALITY_PROFILES[tier] || WORLD_QUALITY_PROFILES[WORLD_QUALITY_TIERS[0]]
  );
}

/**
 * Classify a device from signals that are cheap, synchronous, and available
 * before the first allocation. None is decisive alone — deviceMemory is
 * Chromium-only, hardwareConcurrency counts little cores on phones, and a
 * hardened browser withholds the GPU name — so an unknown signal never votes
 * and the answer falls back to what the pointer type already established.
 */
export function classifyWorldDeviceTier({
  coarsePointer = false,
  cores = 0,
  memoryGb = 0,
  gpu = "",
  saveData = false,
  forceLowest = false,
} = {}) {
  if (forceLowest === true) return "mobile-low";
  const renderer = String(gpu || "").toLowerCase();
  if (SOFTWARE_RENDERER_PATTERN.test(renderer)) return "mobile-low";
  const weakGpu = LOW_TIER_GPU_PATTERN.test(renderer);
  const knownCores =
    Number.isFinite(cores) && cores > 0 ? Math.round(cores) : 0;
  const knownMemory =
    Number.isFinite(memoryGb) && memoryGb > 0 ? Number(memoryGb) : 0;
  if (coarsePointer === true) {
    const weak =
      saveData === true ||
      weakGpu ||
      (knownCores > 0 && knownCores <= LOW_TIER_MOBILE_CORES) ||
      (knownMemory > 0 && knownMemory <= LOW_TIER_MOBILE_MEMORY_GB);
    return weak ? "mobile-low" : "mobile-mid";
  }
  const modest =
    saveData === true ||
    weakGpu ||
    (knownCores > 0 && knownCores <= LOW_TIER_DESKTOP_CORES) ||
    (knownMemory > 0 && knownMemory <= LOW_TIER_DESKTOP_MEMORY_GB);
  return modest ? "laptop-mid" : "desktop-high";
}

/**
 * The adaptive ladder as one pure decision, so it can be exercised without a
 * GPU. `fps` is what a completed sample window measured, `targetFps` the
 * cadence the loop was asking for over that window (60 Hz normally, 30 Hz in
 * the marketing band's preview mode), and `goodWindows` the run of qualifying
 * windows *before* this one.
 */
export function adaptiveQualityDecision({
  fps = 0,
  targetFps = 60,
  tier = WORLD_QUALITY_TIERS[0],
  bootTier = WORLD_QUALITY_TIERS[0],
  goodWindows = 0,
  msSinceChange = Number.POSITIVE_INFINITY,
} = {}) {
  const target = Math.max(1, Number(targetFps) || 60);
  const ratio = Math.max(0, Number(fps) || 0) / target;
  const good = ratio >= QUALITY_UPGRADE_RATIO;
  const index = WORLD_QUALITY_TIERS.indexOf(tier);
  const bootIndex = WORLD_QUALITY_TIERS.indexOf(bootTier);
  if (index < 0 || bootIndex < 0) return { action: "hold", tier, ratio, good };
  const settled = Number(msSinceChange) >= QUALITY_SETTLE_MS;
  if (
    ratio < QUALITY_DOWNGRADE_RATIO &&
    settled &&
    index < WORLD_QUALITY_TIERS.length - 1 &&
    index - bootIndex < QUALITY_MAX_AUTOMATIC_STEPS
  ) {
    return {
      action: "downgrade",
      tier: WORLD_QUALITY_TIERS[index + 1],
      ratio,
      good,
    };
  }
  if (
    good &&
    index > bootIndex &&
    goodWindows + 1 >= QUALITY_UPGRADE_WINDOWS &&
    Number(msSinceChange) >= QUALITY_RECOVER_DWELL_MS
  ) {
    return {
      action: "upgrade",
      tier: WORLD_QUALITY_TIERS[index - 1],
      ratio,
      good,
    };
  }
  return { action: "hold", tier, ratio, good };
}

// Resolution is the largest single lever this scene has and the only one that
// takes effect on the very next frame, so every tier states both a density
// ceiling and a drawing-buffer area budget. A phone asking for DPR 3.5 and a
// 4K laptop asking for DPR 1 arrive here as the same problem: far more
// samples than this wide camera can spend them on.
export function worldRendererPixelRatio(
  width,
  height,
  devicePixelRatio,
  profile,
) {
  const cssWidth = Math.max(1, Number(width) || 1);
  const cssHeight = Math.max(1, Number(height) || 1);
  const dpr = Math.max(0.25, Number(devicePixelRatio) || 1);
  const tier = worldQualityProfile(profile?.tier);
  // A narrow desktop window is a small viewport, not a small device, so the
  // pointer-fine tiers keep the original width-sensitive ceiling underneath
  // their own cap.
  const densityCap = tier.compact
    ? tier.pixelRatioCap
    : Math.min(tier.pixelRatioCap, cssWidth < 700 ? 1.1 : 1.25);
  const pixelBudgetRatio = Math.sqrt(
    Math.min(DESKTOP_RENDER_PIXEL_BUDGET, tier.pixelBudget) /
      (cssWidth * cssHeight),
  );
  return Math.min(dpr, densityCap, pixelBudgetRatio);
}

export function worldRenderWorkloadStatus(calls = 0, triangles = 0) {
  const drawCalls = Math.max(0, Math.round(Number(calls) || 0));
  const renderedTriangles = Math.max(0, Math.round(Number(triangles) || 0));
  return {
    drawCalls,
    triangles: renderedTriangles,
    drawCallBudget: WORLD_RENDER_WORKLOAD_BUDGET.drawCalls,
    triangleBudget: WORLD_RENDER_WORKLOAD_BUDGET.triangles,
    withinBudget:
      drawCalls <= WORLD_RENDER_WORKLOAD_BUDGET.drawCalls &&
      renderedTriangles <= WORLD_RENDER_WORKLOAD_BUDGET.triangles,
  };
}

// The unmasked GPU name is the one boot signal that separates a software
// rasteriser from a real adapter, and it needs a live context to read. A 1x1
// probe answers it before the scene allocates anything, then hands the
// context straight back so it never counts against the browser's per-page
// WebGL budget. Every failure path simply yields no name, which votes for
// nothing.
let probedGpuRenderer = null;
const GPU_RENDERER_CACHE_KEY = "forkmesh.world.gpu.v1";

export function probeGpuRenderer() {
  if (probedGpuRenderer !== null) return probedGpuRenderer;
  probedGpuRenderer = "";
  try {
    const cached = globalThis.localStorage?.getItem?.(GPU_RENDERER_CACHE_KEY);
    if (typeof cached === "string" && cached.length <= 96) {
      probedGpuRenderer = cached;
      return probedGpuRenderer;
    }
  } catch (_) {}
  try {
    const canvas = document.createElement("canvas");
    canvas.width = 1;
    canvas.height = 1;
    const gl =
      canvas.getContext("webgl2", { powerPreference: "low-power" }) ||
      canvas.getContext("webgl", { powerPreference: "low-power" });
    if (!gl) return probedGpuRenderer;
    const debugInfo = gl.getExtension?.("WEBGL_debug_renderer_info");
    if (debugInfo) {
      probedGpuRenderer = String(
        gl.getParameter(debugInfo.UNMASKED_RENDERER_WEBGL) || "",
      ).slice(0, 96);
    }
    gl.getExtension?.("WEBGL_lose_context")?.loseContext?.();
    try {
      globalThis.localStorage?.setItem?.(GPU_RENDERER_CACHE_KEY, probedGpuRenderer);
    } catch (_) {}
  } catch (_) {
    // A blocked or exhausted WebGL stack is not a classification signal; the
    // scene's own context creation reports that failure on its own terms.
  }
  return probedGpuRenderer;
}

export function planWorldRenderer({
  container = null,
  forceCompactRenderer = false,
  qualityTier = "",
  win = globalThis.window,
} = {}) {
  const coarsePointer = Boolean(
    forceCompactRenderer || win?.matchMedia?.("(pointer: coarse)")?.matches,
  );
  const bootQualityTier = WORLD_QUALITY_PROFILES[qualityTier]
    ? qualityTier
    : classifyWorldDeviceTier({
        coarsePointer,
        cores: win?.navigator?.hardwareConcurrency,
        memoryGb: win?.navigator?.deviceMemory,
        gpu: probeGpuRenderer(),
        saveData: win?.navigator?.connection?.saveData === true,
        forceLowest: forceCompactRenderer === true,
      });
  const qualityProfile = worldQualityProfile(bootQualityTier);
  const compactRenderer = qualityProfile.compact;
  const requestedPixelRatio = Math.max(0.1, Number(win?.devicePixelRatio) || 1);
  const initialBounds = container?.getBoundingClientRect?.() || { width: 0, height: 0 };
  const requestedSurfacePixels =
    Math.max(
      Math.max(1, Number(initialBounds.width) || 0) *
        Math.max(1, Number(initialBounds.height) || 0),
      Math.max(1, Number(win?.innerWidth) || 0) *
        Math.max(1, Number(win?.innerHeight) || 0),
      Math.max(1, Number(win?.screen?.width) || 0) *
        Math.max(1, Number(win?.screen?.height) || 0),
    ) * requestedPixelRatio ** 2;
  const renderPixelBudget = Math.min(RENDER_PIXEL_BUDGET, qualityProfile.pixelBudget);
  const memoryConstrainedRenderer =
    !qualityProfile.antialias || requestedSurfacePixels > renderPixelBudget;
  const contextAttributes = {
    alpha: false,
    depth: true,
    stencil: false,
    antialias: !memoryConstrainedRenderer,
    premultipliedAlpha: true,
    preserveDrawingBuffer: false,
    powerPreference: compactRenderer ? "default" : "high-performance",
    failIfMajorPerformanceCaveat: false,
  };
  return {
    coarsePointer,
    bootQualityTier,
    qualityProfile,
    compactRenderer,
    initialBounds,
    memoryConstrainedRenderer,
    contextAttributes,
    key: JSON.stringify(contextAttributes),
  };
}

export function prepareWorldRendererContext(
  plan,
  THREE = null,
  doc = globalThis.document,
  bakeSky = null,
) {
  let context = null;
  try {
    const canvas = doc.createElement("canvas");
    context = canvas.getContext("webgl2", plan.contextAttributes);
    if (!context) return null;
    const renderer = THREE?.WebGLRenderer
      ? new THREE.WebGLRenderer({ canvas, context, ...plan.contextAttributes })
      : null;
    renderer?.capabilities?.getMaxAnisotropy?.();
    if (renderer) {
      renderer.outputColorSpace = THREE.SRGBColorSpace;
      renderer.toneMapping = THREE.ACESFilmicToneMapping;
    }
    const sky =
      renderer && bakeSky && !plan.compactRenderer ? bakeSky(THREE, renderer) : null;
    return { canvas, context, renderer, sky, key: plan.key };
  } catch (_) {
    try {
      context?.getExtension?.("WEBGL_lose_context")?.loseContext?.();
    } catch (_) {}
    return null;
  }
}

export function releaseWorldRendererContext(prepared) {
  try {
    prepared?.sky?.environment?.dispose?.();
    prepared?.sky?.pmrem?.dispose?.();
    prepared?.renderer?.dispose?.();
    prepared?.context?.getExtension?.("WEBGL_lose_context")?.loseContext?.();
  } catch (_) {}
}
