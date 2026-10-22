import {
  collectRepeatedMeshes,
  commonAncestor,
  createInstancedMirrorSet,
} from "../../world-resource-cache.js";
import {
  canvasTextureScale,
  paintCanvasJob,
  paintDeferredCanvases,
} from "../canvas.js";
import {
  clamp,
  disposeOwnedGeometry,
  disposeOwnedMaterial,
  objectIsEffectivelyVisible,
} from "../object-utils.js";
import {
  DIAGNOSTICS_TOP_ELEMENTS_MS,
  RENDER_PIXEL_BUDGET,
  SCENE_COMPLEXITY_SLICE_OBJECTS,
  worldRenderWorkloadStatus,
} from "../quality.js";
import { groundPlaqueParts } from "../arrival.js";
import { SCENE_BUILD_FRAME_MS, yieldSceneBuild, yieldSceneFrame } from "../scene-build.js";
import { warmMaterialsAsync } from "../shader-warmup.js";

export function functions({
  THREE,
  labelLayer,
  ctx,
  fns,
}) {
  let diagnosticsContextInfo = null;
  const STATIC_MIRROR_SYNC_FRAMES = 20;
  let diagnosticsSceneStats = null;
  let diagnosticsSceneStatsAt = -Infinity;
  let diagnosticsSceneStatsIdle = 0;
  let diagnosticsTopElements = [];
  let diagnosticsTopElementsAt = -Infinity;

  const FIRST_VIEW_TEXTURE_SLOTS = ["map", "emissiveMap", "alphaMap"];
  const FIRST_VIEW_PAINT_DISTANCE = 70;
  const TEXTURE_SLOTS = [
    "map",
    "normalMap",
    "roughnessMap",
    "metalnessMap",
    "aoMap",
    "emissiveMap",
    "bumpMap",
    "alphaMap",
    "envMap",
    "lightMap",
    "displacementMap",
    "specularMap",
    "matcap",
    "gradientMap",
  ];

  // three.js compiles a shader the first time a material is drawn and
  // uploads a texture the first time it is sampled. Both used to happen on
  // the frame a district first swung into view: looking toward the Office
  // cost a 400-600ms stall on a desktop GPU and seconds on a laptop. Once the
  // first frames are up, do that work ahead of time in idle slices. A slice
  // builds a few materials no earlier slice has seen, and the next slice waits
  // until the GPU has finished them: compiling a whole district at once queued
  // a second of shaders that stalled the GPU process, and every synchronous
  // read behind it, including the guard's first draws. compileAsync lets
  // drivers with parallel shader compilation build off the main thread.
  // Compact renderers skip the texture uploads (their GPU memory budget is
  // the reason plates are paged) and every paged-out district.
  const GPU_WARMUP_DELAY_MS = 1_500;
  // Most materials share a program with one built earlier, so a slice rarely
  // starts more than one or two shaders.
  const GPU_WARMUP_MATERIALS_PER_SLICE = 4;
  const GPU_WARMUP_TEXTURES_PER_SLICE = 6;
  const GPU_WARMUP_SLICE_MS = 4;
  const GPU_WARMUP_PRIME_MS = 2;
  const GPU_WARMUP_RESCAN_MS = 4_000;
  const GPU_WARMUP_SCAN_OBJECTS = 1_000;
  const GPU_WARMUP_REFLECTION_REACH = 150;
  const GPU_WARMUP_REVEAL_TIMEOUT_MS = 3_000;
  const GPU_WARMUP_LIGHTS_REFRESH_MS = 1_000;
  const GPU_WARMUP_SHADOW_MARGIN = 24;
  let gpuWarmupTimer = 0;
  let gpuWarmupIdle = 0;
  const GEOMETRY_DEDUPE_INTERVAL_MS = 5_000;
  const GEOMETRY_DEDUPE_RETRY_MS = 250;
  let geometryDedupeTimer = 0;
  let geometryDedupeIdle = 0;


  // GPU name, antialias, and WebGL tier are immutable per context, so they
  // are probed once. Privacy-hardened browsers may refuse the debug-renderer
  // extension entirely; the panel then simply reports no GPU name.
  function rendererContextInfo() {
    if (diagnosticsContextInfo) return diagnosticsContextInfo;
    const info = {
      gpu: "",
      antialias: !ctx.memoryConstrainedRenderer,
      webgl2: ctx.renderer.capabilities?.isWebGL2 === true,
      maxTextureSize: Math.max(
        0,
        Number(ctx.renderer.capabilities?.maxTextureSize) || 0,
      ),
      maxTextures: Math.max(
        0,
        Number(ctx.renderer.capabilities?.maxTextures) || 0,
      ),
    };
    try {
      const gl = ctx.renderer.getContext();
      const attributes = gl?.getContextAttributes?.();
      if (attributes) info.antialias = attributes.antialias === true;
      const debugInfo = gl?.getExtension?.("WEBGL_debug_renderer_info");
      if (debugInfo) {
        info.gpu = String(
          gl.getParameter(debugInfo.UNMASKED_RENDERER_WEBGL) || "",
        ).slice(0, 96);
      }
    } catch {
      diagnosticsContextInfo = info;
      return info;
    }
    diagnosticsContextInfo = info;
    return info;
  }

  function toneMappingName(value) {
    if (value === THREE.ACESFilmicToneMapping) return "ACES filmic";
    if (value === THREE.ReinhardToneMapping) return "Reinhard";
    if (value === THREE.CineonToneMapping) return "Cineon";
    if (value === THREE.LinearToneMapping) return "linear";
    if (value === THREE.NoToneMapping) return "off";
    return `#${value}`;
  }

  function shadowMapTypeName(value) {
    if (value === THREE.PCFSoftShadowMap) return "PCF soft";
    if (value === THREE.PCFShadowMap) return "PCF";
    if (value === THREE.VSMShadowMap) return "VSM";
    if (value === THREE.BasicShadowMap) return "basic";
    return `#${value}`;
  }

  // RGBA8 plus a third for mipmaps — a display heuristic for spotting
  // disproportionately large assets, not an exact meter.
  function estimateTextureBytes(texture) {
    const image = texture?.isCubeTexture
      ? texture.image?.[0]
      : texture?.image;
    const faces = texture?.isCubeTexture ? 6 : 1;
    const width = Number(image?.width) || 0;
    const height = Number(image?.height) || 0;
    if (!width || !height) return 0;
    return Math.round(width * height * 4 * 1.33) * faces;
  }

  function* sceneComplexitySteps() {
    const stats = {
      objects: 0,
      hiddenObjects: 0,
      meshes: 0,
      instancedMeshes: 0,
      instancedInstances: 0,
      skinnedMeshes: 0,
      sprites: 0,
      pointsObjects: 0,
      lineObjects: 0,
      lights: 0,
      activeLights: 0,
      pointLights: 0,
      activePointLights: 0,
      shadowLights: 0,
      shadowCasters: 0,
      transparentDrawables: 0,
      doubleSidedDrawables: 0,
      cullingOff: 0,
      uniqueGeometries: 0,
      uniqueMaterials: 0,
      uniqueTextures: 0,
      geometryBytes: 0,
      textureBytes: 0,
      renderTargetBytes: 0,
      topElements: [],
    };
    const geometries = new Set();
    const materials = new Set();
    const textures = new Set();
    const visit = (child) => {
      stats.objects += 1;
      if (child.visible === false) stats.hiddenObjects += 1;
      if (child.isLight) {
        stats.lights += 1;
        if (objectIsEffectivelyVisible(child)) stats.activeLights += 1;
        if (child.isPointLight) {
          stats.pointLights += 1;
          if (objectIsEffectivelyVisible(child)) {
            stats.activePointLights += 1;
          }
        }
        if (child.castShadow) {
          stats.shadowLights += 1;
          const size = child.shadow?.mapSize;
          stats.renderTargetBytes +=
            Math.max(0, Number(size?.x) || 0) *
            Math.max(0, Number(size?.y) || 0) *
            4;
        }
        return;
      }
      const drawable =
        child.isMesh || child.isPoints || child.isLine || child.isSprite;
      if (!drawable) return;
      // Invisible meshes used only for precise interaction are not submitted
      // to WebGL and should not inflate scene/draw complexity diagnostics.
      if (child.userData?.raycastProxy === true) return;
      if (child.isMesh) {
        stats.meshes += 1;
        if (child.castShadow) stats.shadowCasters += 1;
      }
      if (child.isInstancedMesh) {
        stats.instancedMeshes += 1;
        stats.instancedInstances += Math.max(0, Number(child.count) || 0);
      }
      if (child.isSkinnedMesh) stats.skinnedMeshes += 1;
      if (child.isSprite) stats.sprites += 1;
      if (child.isPoints) stats.pointsObjects += 1;
      if (child.isLine) stats.lineObjects += 1;
      if (child.frustumCulled === false) stats.cullingOff += 1;
      const geometry = child.geometry;
      if (geometry && !geometries.has(geometry)) {
        geometries.add(geometry);
        let bytes = 0;
        for (const attribute of Object.values(geometry.attributes || {})) {
          bytes += attribute?.array?.byteLength || 0;
        }
        bytes += geometry.index?.array?.byteLength || 0;
        stats.geometryBytes += bytes;
      }
      const childMaterials = Array.isArray(child.material)
        ? child.material
        : child.material
          ? [child.material]
          : [];
      let transparent = false;
      let doubleSided = false;
      for (const material of childMaterials) {
        if (!material) continue;
        if (material.transparent === true) transparent = true;
        if (material.side === THREE.DoubleSide) doubleSided = true;
        if (materials.has(material)) continue;
        materials.add(material);
        for (const slot of TEXTURE_SLOTS) {
          const texture = material[slot];
          if (!texture?.isTexture || textures.has(texture)) continue;
          textures.add(texture);
          stats.textureBytes += estimateTextureBytes(texture);
        }
      }
      if (transparent) stats.transparentDrawables += 1;
      if (doubleSided) stats.doubleSidedDrawables += 1;
    };
    const pending = [ctx.scene];
    let visited = 0;
    while (pending.length) {
      const child = pending.pop();
      visit(child);
      const children = child.children;
      for (let index = children.length - 1; index >= 0; index -= 1) {
        pending.push(children[index]);
      }
      visited += 1;
      if (visited % SCENE_COMPLEXITY_SLICE_OBJECTS === 0) yield visited;
    }
    stats.uniqueGeometries = geometries.size;
    stats.uniqueMaterials = materials.size;
    stats.uniqueTextures = textures.size;
    // The lounge mirror renders the world into a cube target every frame.
    stats.renderTargetBytes +=
      Math.max(0, Number(ctx.reflectionTarget?.width) || 0) *
      Math.max(0, Number(ctx.reflectionTarget?.height) || 0) *
      4 *
      6;
    // Name the heaviest enabled world elements so "what is costing me
    // triangles" is answered with friendly labels, not Object3D numbers.
    const at = performance.now();
    if (at - diagnosticsTopElementsAt >= DIAGNOSTICS_TOP_ELEMENTS_MS) {
      yield visited;
      diagnosticsTopElementsAt = at;
      const totals = [];
      for (const element of [...ctx.worldElements.values()]) {
        if (!fns.worldElementEnabled(element.id)) continue;
        let triangles = 0;
        let drawables = 0;
        for (const root of [...element.roots]) {
          if (!root?.parent) continue;
          const stack = [root];
          while (stack.length) {
            const child = stack.pop();
            for (let index = 0; index < child.children.length; index += 1) {
              stack.push(child.children[index]);
            }
            visited += 1;
            if (visited % SCENE_COMPLEXITY_SLICE_OBJECTS === 0) yield visited;
            if (
              !(child.isMesh || child.isPoints || child.isLine || child.isSprite) ||
              child.userData?.raycastProxy === true
            ) {
              continue;
            }
            drawables += 1;
            if (!child.isMesh) continue;
            const geometry = child.geometry;
            triangles += Math.floor(
              (geometry?.index?.count ?? geometry?.attributes?.position?.count ?? 0) / 3,
            );
          }
        }
        if (triangles > 0) {
          totals.push({ label: element.label, triangles, drawables });
        }
      }
      diagnosticsTopElements = totals
        .sort((a, b) => b.triangles - a.triangles)
        .slice(0, 3);
    }
    stats.topElements = diagnosticsTopElements;
    return stats;
  }

  function getDiagnostics(now = performance.now()) {
    const sampleNow = Number.isFinite(Number(now))
      ? Number(now)
      : performance.now();
    const elapsedMs = Math.max(1, sampleNow - ctx.diagnosticsSampleAt);
    const frames = ctx.diagnosticsFrameCount;
    const fps = ctx.running
      ? Math.max(0, Math.min(1000, (frames * 1000) / elapsedMs))
      : 0;
    const frameTimeMs =
      ctx.running && frames
        ? Math.max(0, Math.min(60_000, elapsedMs / frames))
        : 0;
    // Average main-thread work per frame over the same window. The gap
    // between it and the wall-clock frame time is what separates "the CPU is
    // busy" from "we are waiting on the GPU or vsync".
    const cpuFrameMs =
      ctx.running && frames
        ? Math.max(0, Math.min(60_000, ctx.diagnosticsFrameWorkTotalMs / frames))
        : 0;
    ctx.diagnosticsFrameWorkTotalMs = 0;
    let frameTimeP95Ms = 0;
    if (ctx.diagnosticsFrameTimes.length) {
      const sorted = [...ctx.diagnosticsFrameTimes].sort((a, b) => a - b);
      frameTimeP95Ms =
        sorted[
          Math.min(
            sorted.length - 1,
            Math.floor(sorted.length * 0.95),
          )
        ];
      ctx.diagnosticsFrameTimes.length = 0;
    }
    ctx.diagnosticsSampleAt = sampleNow;
    ctx.diagnosticsFrameCount = 0;
    const longestFrameMs = ctx.diagnosticsLongestFrameMs;
    const longFrames = ctx.diagnosticsLongFrames;
    const pointerMoves = ctx.diagnosticsPointerMoves;
    const pointerWorstGapMs = ctx.diagnosticsPointerWorstGapMs;
    const worstMovementInputMs = ctx.diagnosticsWorstMovementInputMs;
    ctx.diagnosticsLongestFrameMs = 0;
    ctx.diagnosticsLongFrames = 0;
    ctx.diagnosticsPointerMoves = 0;
    ctx.diagnosticsPointerWorstGapMs = 0;
    ctx.diagnosticsWorstMovementInputMs = 0;
    // The full scene walk is the one genuinely heavy reading, so it is
    // recomputed at most every five seconds and only while something polls.
    if (
      (diagnosticsSceneStats === null ||
        sampleNow - diagnosticsSceneStatsAt >= 5000) &&
      !diagnosticsSceneStatsIdle
    ) {
      diagnosticsSceneStatsAt = sampleNow;
      const steps = sceneComplexitySteps();
      const schedule = (callback) =>
        typeof window.requestIdleCallback === "function"
          ? window.requestIdleCallback(callback, { timeout: 2000 })
          : window.setTimeout(callback, 0);
      const advance = (deadline) => {
        diagnosticsSceneStatsIdle = 0;
        if (ctx.disposed) return;
        let step = steps.next();
        while (
          !step.done &&
          typeof deadline?.timeRemaining === "function" &&
          deadline.timeRemaining() > 2
        ) {
          step = steps.next();
        }
        if (step.done) diagnosticsSceneStats = step.value;
        else diagnosticsSceneStatsIdle = schedule(advance);
      };
      diagnosticsSceneStatsIdle = schedule(advance);
    }
    const contextInfo = rendererContextInfo();
    const drawingBuffer = ctx.renderer.getDrawingBufferSize(
      ctx.diagnosticsDrawingBuffer,
    );
    return {
      fps,
      frameTimeMs,
      frameTimeP95Ms,
      cpuFrameMs,
      worstFrameSinceLoadMs: ctx.diagnosticsWorstFrameSinceLoadMs,
      rendererCalls: ctx.diagnosticsRendererCalls,
      rendererTriangles: ctx.diagnosticsRendererTriangles,
      rendererLines: ctx.diagnosticsRendererLines,
      rendererPoints: ctx.diagnosticsRendererPoints,
      renderWorkload: worldRenderWorkloadStatus(
        ctx.diagnosticsRendererCalls,
        ctx.diagnosticsRendererTriangles,
      ),
      longestFrameMs,
      longFrames,
      pointerMoves,
      pointerWorstGapMs,
      inputResponseMs: ctx.diagnosticsLastMovementInputMs,
      worstInputResponseMs: worstMovementInputMs,
      dragging: ctx.primaryPointerId !== null,
      interactiveObjects: ctx.interactive.length,
      animations: ctx.animated.length,
      geometries: Math.max(0, Number(ctx.renderer.info?.memory?.geometries) || 0),
      textures: Math.max(0, Number(ctx.renderer.info?.memory?.textures) || 0),
      programs: Math.max(0, Number(ctx.renderer.info?.programs?.length) || 0),
      // Main-view draws held back a frame while their shader compiled.
      shaderCompileDeferrals: ctx.shaderCompileGuard.deferredDraws,
      renderStalls: ctx.renderStalls.snapshot(),
      remoteAvatars: ctx.remotePlayers.size,
      // How much of the crowd the four shared avatar batches are carrying.
      avatarPartInstances: ctx.avatarPartInstances.stats(),
      figureBatches: ctx.figureBatches.stats(),
      shadowsEnabled:
        ctx.renderer.shadowMap.enabled &&
        ctx.sun.castShadow &&
        ctx.sun.shadow.intensity > 0,
      disabledElements: ctx.disabledWorldElements.size,
      pixelRatio: ctx.renderer.getPixelRatio(),
      cameraMode: ctx.cameraMode,
      space: ctx.currentSpace,
      moving: ctx.wasWalking,
      zoom: ctx.cameraMode === "first-person" ? ctx.firstPersonZoom : ctx.cameraZoom,
      paused: !ctx.running,
      sky: ctx.worldSky.getState(),
      residency: fns.compactDistrictDiagnostics(),
      quality: fns.qualityState(),
      sceneStats: diagnosticsSceneStats,
      output: {
        drawingBufferWidth: drawingBuffer.x,
        drawingBufferHeight: drawingBuffer.y,
        cssWidth: Math.round(ctx.viewportRect.width),
        cssHeight: Math.round(ctx.viewportRect.height),
        webgl2: contextInfo.webgl2,
        antialias: contextInfo.antialias,
        gpu: contextInfo.gpu,
        maxTextureSize: contextInfo.maxTextureSize,
        maxTextures: contextInfo.maxTextures,
        compactRenderer: ctx.compactRenderer,
        memoryConstrainedRenderer: ctx.memoryConstrainedRenderer,
        canvasTextureScale,
        qualityTier: ctx.qualityProfile.tier,
        pixelBudget: Math.min(RENDER_PIXEL_BUDGET, ctx.qualityProfile.pixelBudget),
        threeRevision: String(THREE.REVISION || ""),
        toneMapping: toneMappingName(ctx.renderer.toneMapping),
        exposure: ctx.renderer.toneMappingExposure,
        colorSpace: String(ctx.renderer.outputColorSpace || ""),
        shadowMapType: shadowMapTypeName(ctx.renderer.shadowMap.type),
        shadowMapSize: Math.max(0, Number(ctx.sun.shadow?.mapSize?.x) || 0),
        shadowAutoUpdate: ctx.renderer.shadowMap.autoUpdate === true,
        cameraFov: ctx.camera.fov,
        cameraNear: ctx.camera.near,
        cameraFar: ctx.camera.far,
      },
    };
  }
  function scheduleGeometryDedupe(delay = GEOMETRY_DEDUPE_INTERVAL_MS) {
    geometryDedupeTimer = window.setTimeout(() => {
      geometryDedupeTimer = 0;
      const run = (deadline) => {
        geometryDedupeIdle = 0;
        if (ctx.disposed) return;
        const budget = typeof deadline?.timeRemaining === "function"
          ? clamp(deadline.timeRemaining(), 2, 8)
          : 4;
        const complete = ctx.geometryDeduper.sweep(ctx.scene, budget);
        if (complete) instanceRepeatedStaticMeshes();
        scheduleGeometryDedupe(
          complete ? GEOMETRY_DEDUPE_INTERVAL_MS : GEOMETRY_DEDUPE_RETRY_MS,
        );
      };
      if (typeof window.requestIdleCallback === "function") {
        geometryDedupeIdle = window.requestIdleCallback(run, { timeout: 2_000 });
      } else {
        run(null);
      }
    }, delay);
  }
  function instanceRepeatedStaticMeshes() {
    const roots = [
      ctx.landmarkObjects.get("office")?.userData?.officeHighFidelity,
      ctx.officeInterior,
      ctx.officeLandscaping,
      ctx.leaderboardDistrict,
      ctx.arrivalBox,
      ctx.campfire,
      ctx.desertPlots,
    ].filter(Boolean);
    let added = 0;
    for (const root of roots) added += mirrorRepeatedMeshes(root);
    if (ctx.world && groundPlaqueParts.size) {
      const mirror = staticMirrorFor(ctx.world);
      groundPlaqueParts.forEach((mesh) => {
        let node = mesh.parent;
        while (node && node !== ctx.world) node = node.parent;
        if (!node) return;
        groundPlaqueParts.delete(mesh);
        if (mirror.add(mesh)) added += 1;
      });
    }
    return added;
  }
  function mirrorRepeatedMeshes(root, mirrors = null) {
    let added = 0;
    const groups = collectRepeatedMeshes(THREE, root, {
      skip: (object) =>
        Boolean(object.userData?.instancedParts) ||
        Boolean(object.userData?.badgeIdentity),
    });
    for (const group of groups) {
      const mirror = staticMirrorFor(commonAncestor(group, root));
      mirrors?.add(mirror);
      group.forEach((mesh) => {
        if (mirror.add(mesh)) added += 1;
      });
    }
    return added;
  }
  function instanceRepeatedMeshesWithin(root) {
    const mirrors = new Set();
    const added = mirrorRepeatedMeshes(root, mirrors);
    root.updateMatrixWorld(true);
    mirrors.forEach((mirror) => mirror.sync(null, { force: true }));
    return added;
  }
  function releaseStaticMirrorsWithin(root) {
    const sources = [];
    root.traverse((object) => {
      if (object.isMesh && !object.isInstancedMesh) sources.push(object);
    });
    for (const [ancestor, mirror] of ctx.staticMirrorSets) {
      let node = ancestor;
      while (node && node !== root) node = node.parent;
      if (node === root) {
        ctx.staticMirrorSets.delete(ancestor);
        ctx.instancedMirrorSets.delete(mirror);
        mirror.dispose();
        continue;
      }
      sources.forEach((source) => mirror.remove(source));
    }
  }
  function staticMirrorFor(ancestor) {
    let mirror = ctx.staticMirrorSets.get(ancestor);
    if (!mirror) {
      mirror = createInstancedMirrorSet(THREE, ancestor, {
        name: `${ancestor.name || "forkmesh"}-instances`,
        interval: STATIC_MIRROR_SYNC_FRAMES,
        phase: ctx.staticMirrorSets.size,
      });
      ctx.staticMirrorSets.set(ancestor, mirror);
      ctx.instancedMirrorSets.add(mirror);
    }
    return mirror;
  }
  function cancelGeometryDedupe() {
    if (geometryDedupeTimer) window.clearTimeout(geometryDedupeTimer);
    if (geometryDedupeIdle && typeof window.cancelIdleCallback === "function") {
      window.cancelIdleCallback(geometryDedupeIdle);
    }
    geometryDedupeTimer = 0;
    geometryDedupeIdle = 0;
  }
  const firstViewTextures = new Set();
  const noteFirstViewTextures = (material, object) => {
    const elements = object?.matrixWorld?.elements;
    if (elements) {
      const dx = elements[12] - ctx.camera.position.x;
      const dy = elements[13] - ctx.camera.position.y;
      const dz = elements[14] - ctx.camera.position.z;
      if (dx * dx + dy * dy + dz * dz > FIRST_VIEW_PAINT_DISTANCE ** 2) return;
    }
    for (let index = 0; index < FIRST_VIEW_TEXTURE_SLOTS.length; index += 1) {
      const texture = material[FIRST_VIEW_TEXTURE_SLOTS[index]];
      if (texture?.isTexture) firstViewTextures.add(texture);
    }
  };
  function paintFirstViewCanvases(budgetMs) {
    const jobs = ctx.constructionCanvasPaints;
    const startedAt = performance.now();
    let remaining = 0;
    for (let index = 0; index < jobs.length; index += 1) {
      const job = jobs[index];
      if (!job || !firstViewTextures.has(job.texture)) continue;
      if (performance.now() - startedAt >= budgetMs) {
        remaining += 1;
        continue;
      }
      jobs[index] = null;
      if (paintCanvasJob(job)) ctx.renderer.initTexture(job.texture);
    }
    return remaining;
  }
  function releaseConstructionCanvases() {
    const jobs = ctx.constructionCanvasPaints.filter(Boolean);
    ctx.constructionCanvasPaints = [];
    firstViewTextures.clear();
    paintDeferredCanvases(
      jobs,
      () => ctx.disposed,
      ctx.compactRenderer
        ? null
        : (texture) => ctx.stripedUploads.begin(texture) || ctx.renderer.initTexture(texture),
      (texture) =>
        ctx.stripedUploads.accepts(texture) || ctx.uploadBudget.takeTexture(texture),
    );
  }
  function warmFirstView({ shadows = true, budgetMs = 0, resume = false } = {}) {
    if (ctx.disposed) return 0;
    if (!resume) fns.updateSceneLevelOfDetail();
    const before = ctx.renderer.info.programs?.length || 0;
    const shadowUpdate = ctx.renderer.shadowMap.needsUpdate;
    const frame = ctx.renderer.info.render.frame;
    if (!shadows) ctx.renderer.shadowMap.needsUpdate = false;
    if (!resume) ctx.scene.updateMatrixWorld();
    try {
      ctx.shaderCompileGuard.warm(
        () => ctx.settledCuller.render(ctx.renderer, ctx.camera),
        noteFirstViewTextures,
        budgetMs,
        resume,
      );
    } finally {
      ctx.renderer.shadowMap.needsUpdate =
        shadowUpdate || (shadows && ctx.renderer.shadowMap.enabled);
      ctx.renderer.info.render.frame = frame;
    }
    return (ctx.renderer.info.programs?.length || 0) - before;
  }
  async function prepareFirstView({ timeoutMs = 900, sliceMs = 8 } = {}) {
    const startedAt = performance.now();
    const deadline = startedAt + Math.max(0, Number(timeoutMs) || 0);
    const guard = ctx.shaderCompileGuard;
    const primedBefore = guard.primedPrograms;
    let waiting = 0;
    warmFirstView();
    ctx.firstViewHoldUntil = Math.max(ctx.firstViewHoldUntil, deadline + 100);
    await yieldSceneBuild();
    let unpainted = 0;
    let frameStartedAt = performance.now();
    while (!ctx.disposed) {
      unpainted = paintFirstViewCanvases(sliceMs);
      waiting = guard.primeReadyPrograms(sliceMs);
      if (waiting === 0 && unpainted === 0) break;
      if (performance.now() >= deadline) break;
      if (unpainted === 0 && guard.pendingPrograms() > 0) {
        await new Promise((resolve) => setTimeout(() => resolve(), 16));
        frameStartedAt = performance.now();
      } else if (performance.now() - frameStartedAt >= SCENE_BUILD_FRAME_MS) {
        await yieldSceneFrame();
        frameStartedAt = performance.now();
      } else {
        await yieldSceneBuild();
      }
    }
    ctx.firstViewHoldUntil = 0;
    ctx.programPacer.pace();
    releaseConstructionCanvases();
    return {
      primed: guard.primedPrograms - primedBefore,
      waiting,
      programs: ctx.renderer.info.programs?.length || 0,
      warmedDraws: guard.warmedDraws,
      unpainted,
      ms: performance.now() - startedAt,
    };
  }

  const warmLights = new Set();
  let warmLightsAt = -Infinity;
  let warmLightTarget = null;
  const litFromScene = (light) => {
    for (let node = light; node; node = node.parent) {
      if (node.visible === false) return false;
      if (node === ctx.scene) return true;
    }
    return false;
  };
  function warmTarget() {
    const at = performance.now();
    if (at - warmLightsAt >= GPU_WARMUP_LIGHTS_REFRESH_MS) {
      warmLightsAt = at;
      warmLights.clear();
      ctx.scene.traverse((object) => {
        if (object.isLight) warmLights.add(object);
      });
    }
    warmLightTarget ||= {
      isScene: true,
      get fog() {
        return ctx.scene.fog;
      },
      get environment() {
        return ctx.scene.environment;
      },
      get environmentRotation() {
        return ctx.scene.environmentRotation;
      },
      traverseVisible(visit) {
        warmLights.forEach((light) => {
          if (litFromScene(light)) visit(light);
        });
      },
    };
    return warmLightTarget;
  }
  function warmBeforeReveal(root, timeoutMs = GPU_WARMUP_REVEAL_TIMEOUT_MS) {
    if (ctx.disposed || !root) return Promise.resolve(root);
    let pending;
    try {
      pending = ctx.programPacer.paced(() =>
        warmMaterialsAsync(ctx.renderer, root, ctx.camera, warmTarget()),
      );
    } catch (_) {
      return Promise.resolve(root);
    }
    return Promise.race([
      pending.catch(() => root),
      new Promise((resolve) => window.setTimeout(() => resolve(root), timeoutMs)),
    ]);
  }
  function warmLogoReflection(timeoutMs = GPU_WARMUP_REVEAL_TIMEOUT_MS) {
    const target = ctx.reflectionTarget;
    const faceCamera = ctx.reflectionCamera?.children?.[0];
    if (ctx.disposed || !target || !faceCamera) return Promise.resolve();
    fns.markLogoReflectionLayer();
    const origin = ctx.reflectionCamera.getWorldPosition(new THREE.Vector3());
    const reach = faceCamera.far ** 2;
    const objects = [];
    const stack = fns.logoReflectionRoots().slice();
    while (stack.length) {
      const object = stack.pop();
      if (object.visible === false || object.userData?.logoReflectionSkip === true) continue;
      for (let index = 0; index < object.children.length; index += 1) {
        stack.push(object.children[index]);
      }
      if (!(object.isMesh || object.isPoints || object.isLine || object.isSprite)) continue;
      if (!object.layers.test(ctx.reflectionCamera.layers)) continue;
      const elements = object.matrixWorld.elements;
      if (
        (elements[12] - origin.x) ** 2 +
          (elements[13] - origin.y) ** 2 +
          (elements[14] - origin.z) ** 2 >
        reach
      ) {
        continue;
      }
      objects.push(object);
    }
    if (!objects.length) return Promise.resolve();
    const renderer = ctx.renderer;
    const previousTarget = renderer.getRenderTarget();
    const current = new Map();
    objects.forEach((object) => {
      (Array.isArray(object.material) ? object.material : [object.material]).forEach((material) => {
        const program = material && renderer.properties.get(material).currentProgram;
        if (program) current.set(material, program);
      });
    });
    let pending;
    renderer.setRenderTarget(target, 0);
    try {
      pending = ctx.programPacer.paced(() =>
        warmMaterialsAsync(
          renderer,
          { traverse: (visit) => objects.forEach(visit), traverseVisible: () => {} },
          faceCamera,
          warmTarget(),
        ),
      );
    } catch (_) {
      pending = Promise.resolve();
    } finally {
      renderer.setRenderTarget(previousTarget);
      current.forEach((program, material) => {
        renderer.properties.get(material).currentProgram = program;
      });
    }
    const textures = new Set();
    objects.forEach((object) => {
      (Array.isArray(object.material) ? object.material : [object.material]).forEach((material) => {
        FIRST_VIEW_TEXTURE_SLOTS.forEach((slot) => {
          const texture = material?.[slot];
          if (texture?.isTexture && !texture.isRenderTargetTexture && !texture.isCubeTexture) {
            textures.add(texture);
          }
        });
      });
    });
    const uploads = [...textures];
    const uploadAll = () =>
      new Promise((resolve) => {
        const next = () => {
          if (ctx.disposed) {
            resolve();
            return;
          }
          while (uploads.length) {
            const texture = uploads[0];
            const image = texture.image;
            if (
              texture.version === 0 ||
              !image ||
              image.complete === false ||
              renderer.properties.get(texture).__version === texture.version
            ) {
              uploads.shift();
              continue;
            }
            if (!ctx.uploadBudget.takeTexture(texture)) break;
            uploads.shift();
            try {
              renderer.initTexture(texture);
            } catch (_) {}
          }
          if (uploads.length) window.requestAnimationFrame(next);
          else resolve();
        };
        next();
      });
    return Promise.race([
      pending.catch(() => {}).then(uploadAll),
      new Promise((resolve) => window.setTimeout(resolve, timeoutMs)),
    ]);
  }
  function scheduleGpuWarmup() {
    const seenVariants = new Set();
    const uploadedVersions = new WeakMap();
    const reflectionOrigin = new THREE.Vector3();
    let queue = [];
    let textureQueue = [];
    let scan = null;
    let nextScanAt = 0;
    const materialsOf = (object) =>
      Array.isArray(object.material)
        ? object.material
        : object.material
          ? [object.material]
          : [];
    const batchRoot = (drawables) => ({
      traverse: (visit) => drawables.forEach(visit),
      traverseVisible: () => {},
    });
    const drawable = (object) =>
      (object.isMesh || object.isPoints || object.isLine || object.isSprite) &&
      object.userData?.raycastProxy !== true &&
      object.layers.test(ctx.camera.layers);
    const variantKind = (object) =>
      object.isInstancedMesh
        ? object.instanceColor
          ? "instanced-colour"
          : "instanced"
        : object.isSkinnedMesh
          ? "skinned"
          : object.isPoints
            ? "points"
            : object.isLine
              ? "line"
              : object.isSprite
                ? "sprite"
                : "mesh";
    const reflectionInReach = () => {
      if (!ctx.reflectionCamera || !ctx.reflectionTarget) return false;
      ctx.reflectionCamera.getWorldPosition(reflectionOrigin);
      return (
        reflectionOrigin.distanceToSquared(ctx.player.position) <=
        GPU_WARMUP_REFLECTION_REACH ** 2
      );
    };
    const shadowSphere = new THREE.Sphere();
    const shadowEye = new THREE.Vector3();
    const shadowTarget = new THREE.Vector3();
    const shadowView = new THREE.Matrix4();
    const shadowInverse = new THREE.Matrix4();
    const shadowCenter = new THREE.Vector3();
    const shadowCasterReach = () => {
      if (!ctx.renderer.shadowMap.enabled || !ctx.sun.castShadow) return null;
      const shadowCamera = ctx.sun.shadow?.camera;
      if (!shadowCamera?.isOrthographicCamera) return null;
      shadowEye.setFromMatrixPosition(ctx.sun.matrixWorld);
      shadowTarget.setFromMatrixPosition(ctx.sun.target.matrixWorld);
      shadowView.lookAt(shadowEye, shadowTarget, shadowCamera.up);
      shadowView.setPosition(shadowEye);
      shadowInverse.copy(shadowView).invert();
      return {
        inverse: shadowInverse,
        halfWidth: Math.max(Math.abs(shadowCamera.left), Math.abs(shadowCamera.right)),
        halfHeight: Math.max(Math.abs(shadowCamera.top), Math.abs(shadowCamera.bottom)),
        near: shadowCamera.near,
        far: shadowCamera.far,
      };
    };
    const parkShadowCaster = (object, reach) => {
      const parked = object.userData.forkmeshShadowParked === true;
      if (!object.castShadow && !parked) return;
      if (object.isSkinnedMesh || object.isInstancedMesh || object.isBatchedMesh) return;
      const geometry = object.geometry;
      if (!geometry) return;
      if (!geometry.boundingSphere) geometry.computeBoundingSphere();
      if (!geometry.boundingSphere) return;
      shadowSphere.copy(geometry.boundingSphere).applyMatrix4(object.matrixWorld);
      shadowCenter.copy(shadowSphere.center).applyMatrix4(reach.inverse);
      const margin = shadowSphere.radius + GPU_WARMUP_SHADOW_MARGIN;
      const inReach =
        Math.abs(shadowCenter.x) <= reach.halfWidth + margin &&
        Math.abs(shadowCenter.y) <= reach.halfHeight + margin &&
        -shadowCenter.z >= reach.near - margin &&
        -shadowCenter.z <= reach.far + margin;
      if (object.castShadow && !inReach) {
        object.castShadow = false;
        object.userData.forkmeshShadowParked = true;
      } else if (parked && inReach) {
        object.castShadow = true;
        delete object.userData.forkmeshShadowParked;
      }
    };
    function* scanSteps() {
      const found = new Map();
      const px = ctx.player.position.x;
      const py = ctx.player.position.y;
      const pz = ctx.player.position.z;
      const reflection = reflectionInReach();
      const reflectionFar = ctx.reflectionCamera?.children?.[0]?.far || 0;
      const reflectionDistance = reflection
        ? reflectionOrigin.distanceToSquared(ctx.player.position)
        : 0;
      const stack = [ctx.scene];
      const shadowReach = shadowCasterReach();
      const reflectionStack = reflection ? [...fns.logoReflectionRoots()] : [];
      let visited = 0;
      const otherFloors = new Set();
      ctx.officeFloorGroups?.forEach((group, floorId) => {
        if (floorId !== "lobby") otherFloors.add(group);
      });
      while (reflectionStack.length) {
        const object = reflectionStack.pop();
        if (otherFloors.has(object)) continue;
        if (object.userData?.logoReflectionSkip === true) continue;
        if (object.visible === false && object !== ctx.officeInterior) continue;
        for (let index = 0; index < object.children.length; index += 1) {
          reflectionStack.push(object.children[index]);
        }
        visited += 1;
        if (visited % GPU_WARMUP_SCAN_OBJECTS === 0) yield;
        if (!drawable(object)) continue;
        const elements = object.matrixWorld.elements;
        if (
          (elements[12] - reflectionOrigin.x) ** 2 +
            (elements[13] - reflectionOrigin.y) ** 2 +
            (elements[14] - reflectionOrigin.z) ** 2 >
          reflectionFar ** 2
        ) {
          continue;
        }
        const kind = variantKind(object);
        for (const material of materialsOf(object)) {
          if (!material || material.visible === false) continue;
          const key = `reflection:${material.id}:${kind}`;
          if (!seenVariants.has(key) && !found.has(key)) {
            found.set(key, {
              key,
              object,
              material,
              target: ctx.reflectionTarget,
              distance: reflectionDistance,
            });
          }
        }
      }
      const figureStack = [false];
      while (stack.length) {
        const object = stack.pop();
        const inFigure = figureStack.pop();
        if (object.userData?.compactDistrictResident === false) continue;
        const childInFigure = inFigure || ctx.figureBatches.stateOf(object) !== "";
        for (let index = 0; index < object.children.length; index += 1) {
          stack.push(object.children[index]);
          figureStack.push(childInFigure);
        }
        visited += 1;
        if (visited % GPU_WARMUP_SCAN_OBJECTS === 0) yield;
        if (object.isMesh && shadowReach) parkShadowCaster(object, shadowReach);
        if (!drawable(object)) continue;
        const elements = object.matrixWorld.elements;
        const dx = elements[12] - px;
        const dy = elements[13] - py;
        const dz = elements[14] - pz;
        const distance = dx * dx + dy * dy + dz * dz;
        const kind = variantKind(object);
        for (const material of materialsOf(object)) {
          if (!material || material.visible === false) continue;
          const key = `${material.id}:${kind}`;
          if (!seenVariants.has(key) && !found.has(key)) {
            found.set(key, {
              key,
              object,
              material,
              target: null,
              distance,
              textures: !inFigure,
            });
          }
        }
      }
      return [...found.values()].sort((a, b) => a.distance - b.distance);
    }
    const queueTextures = (material) => {
      if (ctx.compactRenderer) return;
      for (const slot of TEXTURE_SLOTS) {
        const texture = material[slot];
        if (!texture?.isTexture || texture.isCubeTexture || texture.isVideoTexture) continue;
        if (texture.isRenderTargetTexture) continue;
        textureQueue.push(texture);
      }
    };
    const compileEntries = (entries, target) => {
      if (!entries.length) return;
      const renderer = ctx.renderer;
      const previousTarget = renderer.getRenderTarget();
      const current = new Map();
      if (target) {
        entries.forEach(({ object }) => {
          materialsOf(object).forEach((material) => {
            const program = renderer.properties.get(material).currentProgram;
            if (program) current.set(material, program);
          });
        });
        renderer.setRenderTarget(target, 0);
      }
      try {
        void ctx.programPacer.paced(() =>
          warmMaterialsAsync(
            renderer,
            batchRoot(entries.map((entry) => entry.object)),
            ctx.camera,
            warmTarget(),
          ),
        ).catch(() => {});
      } catch (_) {
      } finally {
        if (target) {
          renderer.setRenderTarget(previousTarget);
          current.forEach((program, material) => {
            renderer.properties.get(material).currentProgram = program;
          });
        }
      }
    };
    const compileNext = () => {
      const main = [];
      const reflected = [];
      let fresh = 0;
      while (queue.length && fresh < GPU_WARMUP_MATERIALS_PER_SLICE) {
        const entry = queue.shift();
        if (seenVariants.has(entry.key) || !entry.object.parent) continue;
        seenVariants.add(entry.key);
        fresh += 1;
        (entry.target ? reflected : main).push(entry);
        if (!entry.target && entry.textures) queueTextures(entry.material);
      }
      compileEntries(main, null);
      compileEntries(reflected, ctx.reflectionTarget);
      return fresh;
    };
    const uploadTextures = (hasTime) => {
      let uploads = 0;
      while (
        uploads < GPU_WARMUP_TEXTURES_PER_SLICE &&
        textureQueue.length &&
        hasTime()
      ) {
        const texture = textureQueue[0];
        const image = texture.image;
        if (
          texture.version === 0 ||
          !image ||
          image.complete === false ||
          uploadedVersions.get(texture) === texture.version ||
          ctx.renderer.properties.get(texture).__version === texture.version
        ) {
          textureQueue.shift();
          continue;
        }
        if (ctx.stripedUploads.begin(texture)) {
          textureQueue.shift();
          uploads += 1;
          uploadedVersions.set(texture, texture.version);
          continue;
        }
        if (!ctx.uploadBudget.takeTexture(texture)) break;
        textureQueue.shift();
        uploads += 1;
        uploadedVersions.set(texture, texture.version);
        try {
          ctx.renderer.initTexture(texture);
        } catch (_) {}
      }
    };
    const idle = (callback) => {
      if (typeof window.requestIdleCallback === "function") {
        gpuWarmupIdle = window.requestIdleCallback(callback, { timeout: 500 });
      } else {
        gpuWarmupTimer = window.setTimeout(callback, 32);
      }
    };
    const step = (deadline) => {
      gpuWarmupIdle = 0;
      gpuWarmupTimer = 0;
      if (ctx.disposed) return;
      if (!ctx.running || ctx.firstViewHoldUntil > performance.now()) {
        gpuWarmupTimer = window.setTimeout(step, GPU_WARMUP_DELAY_MS);
        return;
      }
      const startedAt = performance.now();
      const hasTime = () =>
        typeof deadline?.timeRemaining === "function" && !deadline.didTimeout
          ? deadline.timeRemaining() > 1
          : performance.now() - startedAt < GPU_WARMUP_SLICE_MS;
      if (!scan && !queue.length && performance.now() >= nextScanAt) {
        scan = scanSteps();
      }
      while (scan && hasTime()) {
        const next = scan.next();
        if (next.done) {
          queue = next.value;
          scan = null;
          nextScanAt = performance.now() + GPU_WARMUP_RESCAN_MS;
        }
      }
      if (ctx.shaderCompileGuard.pendingPrograms() === 0) {
        ctx.shaderCompileGuard.primeReadyPrograms(GPU_WARMUP_PRIME_MS);
        if (hasTime()) compileNext();
      }
      uploadTextures(hasTime);
      if (
        scan ||
        queue.length ||
        textureQueue.length ||
        ctx.shaderCompileGuard.pendingPrograms() > 0
      ) {
        idle(step);
        return;
      }
      gpuWarmupTimer = window.setTimeout(
        () => idle(step),
        Math.max(0, nextScanAt - performance.now()),
      );
    };
    gpuWarmupTimer = window.setTimeout(() => {
      gpuWarmupTimer = 0;
      if (ctx.disposed) return;
      idle(step);
    }, GPU_WARMUP_DELAY_MS);
  }
  function cancelGpuWarmup() {
    if (gpuWarmupTimer) window.clearTimeout(gpuWarmupTimer);
    if (gpuWarmupIdle && typeof window.cancelIdleCallback === "function") {
      window.cancelIdleCallback(gpuWarmupIdle);
    }
    gpuWarmupTimer = 0;
    gpuWarmupIdle = 0;
  }

  function dispose() {
    cancelGpuWarmup();
    cancelGeometryDedupe();
    ctx.disposed = true;
    ctx.renderer.setAnimationLoop(null);
    window.clearTimeout(ctx.renderStallWarningTimer);
    window.clearTimeout(ctx.movementInputWarningTimer);
    ctx.renderStallWarningTimer = 0;
    ctx.movementInputWarningTimer = 0;
    window.clearTimeout(ctx.movementEventTimer);
    window.clearTimeout(ctx.officeMovementEventTimer);
    ctx.movementEventTimer = 0;
    ctx.officeMovementEventTimer = 0;
    ctx.pendingMovementEvent = null;
    ctx.pendingOfficeMovementEvent = null;
    ctx.worldSky.dispose();
    ctx.reflectionTarget.dispose();
    ctx.logoReflectionEnvironment?.dispose();
    ctx.resizeObserver?.disconnect();
    window.removeEventListener("resize", ctx.resize);
    window.removeEventListener("scroll", fns.invalidateCanvasRect, {
      capture: true,
    });
    window.removeEventListener("orientationchange", fns.invalidateCanvasRect);
    window.visualViewport?.removeEventListener("scroll", fns.invalidateCanvasRect);
    window.cancelAnimationFrame(ctx.resizeFrame);
    ctx.resizeFrame = 0;
    ctx.renderer.domElement.removeEventListener("pointerdown", fns.handlePointerDown);
    ctx.renderer.domElement.removeEventListener("dblclick", fns.handleDoubleClick);
    ctx.renderer.domElement.removeEventListener("pointermove", fns.handlePointerMove);
    ctx.renderer.domElement.removeEventListener("wheel", fns.handleWheel);
    ctx.renderer.domElement.removeEventListener(
      "webglcontextlost",
      ctx.handleContextLost,
    );
    ctx.renderer.domElement.removeEventListener(
      "webglcontextrestored",
      ctx.handleContextRestored,
    );
    window.removeEventListener("pointerup", fns.handlePointerUp);
    window.removeEventListener("pointercancel", fns.handlePointerCancel);
    window.removeEventListener("keydown", fns.handleKeyDown);
    window.removeEventListener("keyup", fns.handleKeyUp);
    window.removeEventListener("blur", fns.handleWindowBlur);
    ctx.touchPointers.clear();
    ctx.externalTouchInteractionActive = false;
    ctx.keys.clear();
    ctx.touchKeys.clear();
    ctx.touchMovement.set(0, 0);
    // Put deleted pieces back before the sweep below: a detached subtree is
    // not reachable from the scene, and its GPU buffers would outlive the
    // renderer that allocated them.
    [...ctx.deletedWorldObjects.keys()].forEach((key) => fns.restoreWorldObject(key));
    ctx.latestSocialBannerPayload = null;
    [...ctx.installedWorldPackages.keys()].forEach((id) =>
      fns.removeWorldPackage(id),
    );
    ctx.deferredDistrictInstalls.length = 0;
    ctx.scene.traverse((child) => {
      disposeOwnedGeometry(child.geometry);
      if (Array.isArray(child.material)) {
        child.material.forEach((material) => {
          disposeOwnedMaterial(material, true);
        });
      } else {
        disposeOwnedMaterial(child.material, true);
      }
    });
    ctx.triangleViewMaterial.dispose();
    ctx.skyEnvironment?.dispose();
    ctx.skyEnvironmentPmrem?.dispose();
    ctx.worldProfiler.dispose();
    ctx.renderer.dispose();
    ctx.renderer.domElement.remove();
    labelLayer.replaceChildren();
  }

  return {
    rendererContextInfo,
    getDiagnostics,
    scheduleGeometryDedupe,
    instanceRepeatedStaticMeshes,
    instanceRepeatedMeshesWithin,
    releaseStaticMirrorsWithin,
    prepareFirstView,
    releaseConstructionCanvases,
    warmFirstView,
    scheduleGpuWarmup,
    warmBeforeReveal,
    warmLogoReflection,
    dispose,
  };
}
