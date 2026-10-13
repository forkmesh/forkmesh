import { canvasTextureScale } from "../canvas.js";
import {
  DAYLIGHT_ENVIRONMENT,
  LOCAL_ENVIRONMENT_OVERLAYS,
  SKY_ENVIRONMENT_HEMISPHERE_SHARE,
  SKY_ENVIRONMENT_INTENSITY,
} from "../environment.js";
import {
  LIGHT_LEVEL_DEFAULT,
  LIGHT_LEVEL_MAX,
  LIGHT_LEVEL_MIN,
} from "../gameplay-constants.js";
import { OFFICE_EXTERIOR_LOD_DISTANCE } from "../layout.js";
import { clamp } from "../object-utils.js";
import {
  QUALITY_CHANGE_LOG_LIMIT,
  QUALITY_SAMPLE_WINDOW_MS,
  WEATHER_PARTICLE_COUNT,
  WORLD_QUALITY_PROFILES,
  WORLD_QUALITY_TIERS,
  adaptiveQualityDecision,
  worldQualityProfile,
  worldRendererPixelRatio,
} from "../quality.js";
import { createWeather, followWeather, weatherDrawCount } from "../weather.js";

export function functions({
  THREE,
  ctx,
  fns,
}) {
  // Dark filler lights that hold the visible point-light count steady; see
  // padPointLightSlots().
  const pointLightSlots = [];
  const localPointLightCandidates = [];
  let currentTheme = "world";
  // The story selection the Office floors were last shown for.
  let officeFloorVisibilityKey = null;
  let qualityGoodWindows = 0;
  let qualityLastFps = 0;
  let qualityLastReason = "detected";
  const qualityChangeLog = [];


  function pointLightParentVisible(light) {
    let parent = light?.parent;
    while (parent) {
      if (parent.visible === false) return false;
      if (parent === ctx.scene) return true;
      parent = parent.parent;
    }
    return false;
  }

  function updateLocalPointLightBudget(far = ctx.farSceneDetail === true) {
    const budget = far ? 0 : ctx.qualityProfile.pointLightBudget;
    localPointLightCandidates.length = 0;
    for (const light of ctx.localPointLights) {
      if (!(light.intensity > 0) || !pointLightParentVisible(light)) {
        light.visible = false;
        continue;
      }
      light.getWorldPosition(ctx.localPointLightPosition);
      light.userData.budgetDistanceSq =
        ctx.localPointLightPosition.distanceToSquared(ctx.camera.position);
      localPointLightCandidates.push(light);
    }
    localPointLightCandidates.sort(
      (left, right) =>
        left.userData.budgetDistanceSq - right.userData.budgetDistanceSq,
    );
    // Candidates are sorted nearest first; every other light was switched off
    // above, so the budget is a prefix and needs no lookup set.
    const enabled = Math.min(budget, localPointLightCandidates.length);
    for (let index = 0; index < localPointLightCandidates.length; index += 1) {
      localPointLightCandidates[index].visible = index < enabled;
    }
    padPointLightSlots(ctx.qualityProfile.pointLightBudget - enabled);
    return enabled;
  }

  function ensurePointLightCount() {
    let visible = 0;
    for (const light of ctx.localPointLights) {
      if (light.visible && pointLightParentVisible(light)) visible += 1;
    }
    for (const slot of pointLightSlots) {
      if (slot.visible) visible += 1;
    }
    if (visible !== ctx.qualityProfile.pointLightBudget) updateLocalPointLightBudget();
  }

  // three.js compiles the number of visible point lights into every lit
  // shader. Walking toward the Office's lamps or zooming out (budget 0) used
  // to change that number and recompile every lit material in view — a
  // 100ms-1s hitch each time a new count was reached. Dark, out-of-range slot
  // lights fill whatever the budget leaves unused so the count only changes
  // when the quality tier itself does.
  function padPointLightSlots(count) {
    const wanted = Math.max(0, count);
    while (pointLightSlots.length < wanted) {
      const slot = new THREE.PointLight("#000000", 0, 0.01, 2);
      slot.name = "forkmesh-point-light-slot";
      slot.position.set(0, -10_000, 0);
      slot.castShadow = false;
      ctx.scene.add(slot);
      pointLightSlots.push(slot);
    }
    pointLightSlots.forEach((slot, index) => {
      slot.visible = index < wanted;
    });
  }

  function syncOfficeFloorVisibility() {
    (ctx.landmarkObjects.get("office") || ctx.officeInterior).getWorldPosition(
      ctx.officeLodWorldPosition,
    );
    const cameraDistance = ctx.camera.position.distanceTo(ctx.officeLodWorldPosition);
    const exteriorDetailDistance =
      OFFICE_EXTERIOR_LOD_DISTANCE * ctx.qualityProfile.lodDistanceScale;
    const exteriorDetailed =
      ctx.officeSceneMode === "town" &&
      ctx.farSceneDetail !== true &&
      cameraDistance <= exteriorDetailDistance;
    ctx.world.userData.officeExteriorDetailDistance = cameraDistance;
    // Compact renderers page the Office in by this level; on every renderer
    // the furnished floors stay drawn for as long as the full tower is (the
    // exterior THREE.LOD swaps both for the far tier's slabs together).
    ctx.world.userData.officeExteriorDetailLevel = exteriorDetailed
      ? "furnished"
      : "shell";
    fns.syncOfficeDistanceLevel(ctx.officeSceneMode !== "town" || exteriorDetailed);
    fns.syncOfficeAquariumResidency(cameraDistance, exteriorDetailDistance);
    // Only the story selection changes what is drawn: Town sees every floor
    // through the glass, a visitor inside sees the floor they stand on.
    // Story changes are independent of camera zoom, so a doorway warp and an
    // elevator arrival still expose the selected floor immediately.
    const floorKey =
      ctx.officeSceneMode === "town" ? "town" : ctx.officeCurrentFloorId;
    if (floorKey === officeFloorVisibilityKey) return;
    officeFloorVisibilityKey = floorKey;
    ctx.world.userData.officeInteriorDrawn = true;
    ctx.officeInterior.visible = true;
    for (const [floorId, floorGroup] of ctx.officeFloorGroups) {
      if (floorId === "lobby") continue;
      floorGroup.visible = ctx.officeSceneMode === "town"
        ? true
        : floorId === ctx.officeCurrentFloorId;
    }
    const lobbyShown =
      ctx.officeSceneMode === "town" || ctx.officeCurrentFloorId === "lobby";
    for (const lobbyOnly of [
      ctx.officeAquarium?.group,
      ctx.officeInterior.getObjectByName("forkmesh-office-floor-warp-hub"),
    ]) {
      if (lobbyOnly) lobbyOnly.visible = lobbyShown;
    }
    fns.syncOfficeDeferredFloorDisplays();
  }
  function applyRendererResolution() {
    const ratio = worldRendererPixelRatio(
      ctx.viewportRect.width,
      ctx.viewportRect.height,
      window.devicePixelRatio || 1,
      ctx.qualityProfile,
    );
    const current = ctx.renderer.getSize(ctx.rendererSizeScratch);
    if (
      ctx.renderer.getPixelRatio() === ratio &&
      current.x === ctx.viewportRect.width &&
      current.y === ctx.viewportRect.height
    ) {
      return;
    }
    ctx.renderer.setDrawingBufferSize(ctx.viewportRect.width, ctx.viewportRect.height, ratio);
  }

  function applyWeatherParticleBudget(count) {
    const budget = clamp(
      Math.round(Number(count) || WEATHER_PARTICLE_COUNT),
      24,
      WEATHER_PARTICLE_COUNT,
  );
  ctx.weatherParticleBudget = budget;
  if (!ctx.weather) return budget;
  ctx.weather.rain.geometry.setDrawRange(0, budget);
  ctx.weather.snow.geometry.setDrawRange(0, budget);
  // A live upgrade can widen the range before the next animation frame. Put
  // its newly visible prefix around the visitor now, so it cannot briefly
  // reveal particles parked at the old location.
  followWeather(ctx.weather.rain, ctx.weather.anchor);
  followWeather(ctx.weather.snow, ctx.weather.anchor);
  return budget;
}

  /**
   * Re-apply every knob that can change against a live WebGL context. The
   * construction-time ones (antialias, plate resolution, district residency,
   * the star and satellite allocations) keep their boot-tier values, so a
   * step down never asks this scene to rebuild an allocation mid-session and
   * a step up can never exceed what boot allocated.
   */
  function applyQualityTier(tier, reason = "manual") {
    if (ctx.disposed || !WORLD_QUALITY_PROFILES[tier]) return false;
    const previous = ctx.qualityProfile.tier;
    if (previous === tier) return false;
    const bootProfile = worldQualityProfile(ctx.bootQualityTier);
    ctx.qualityProfile = worldQualityProfile(tier);
    ctx.qualityChangedAt = performance.now();
    qualityLastReason = reason;
    qualityGoodWindows = 0;
    ctx.qualitySampleStartedAt = 0;
    ctx.qualitySampleFrames = 0;
    applyRendererResolution();
    fns.applyShadowQuality();
    applyWeatherParticleBudget(ctx.qualityProfile.weatherParticles);
    ctx.worldSky.setDetailBudget({
      stars: Math.min(bootProfile.starCount, ctx.qualityProfile.starCount),
      satellites: Math.min(
        bootProfile.maxSatellites,
        ctx.qualityProfile.maxSatellites,
      ),
    });
    // The LOD thresholds and the point-light budget both moved, so re-decide
    // them against the camera as it stands rather than at the next zoom.
    updateSceneLevelOfDetail(true);
    // The animation cadence changed; present on the next callback instead of
    // waiting out a deadline measured against the old interval.
    ctx.nextVisualAnimationAt = 0;
    ctx.nextRenderFrameAt = 0;
    qualityChangeLog.push({
      at: Math.round(ctx.qualityChangedAt),
      from: previous,
      to: ctx.qualityProfile.tier,
      reason,
      fps: Math.round(qualityLastFps),
    });
    if (qualityChangeLog.length > QUALITY_CHANGE_LOG_LIMIT) {
      qualityChangeLog.splice(0, qualityChangeLog.length - QUALITY_CHANGE_LOG_LIMIT);
    }
    // setSize cleared the drawing buffer. Paint once now so a quality step is
    // never visible as a blank canvas, using the same guard the resize path
    // uses so it cannot interrupt a live touch gesture.
    if (ctx.running && ctx.primaryPointerId === null && ctx.touchPointers.size === 0) {
      ctx.renderer.render(ctx.scene, ctx.camera);
    }
    return true;
  }

  function sampleAdaptiveQuality(time) {
    if (!ctx.adaptiveQualityEnabled || ctx.disposed) return;
    if (ctx.qualitySampleStartedAt <= 0) {
      ctx.qualitySampleStartedAt = time;
      ctx.qualitySampleFrames = 0;
      return;
    }
    ctx.qualitySampleFrames += 1;
    const elapsedMs = time - ctx.qualitySampleStartedAt;
    if (elapsedMs < QUALITY_SAMPLE_WINDOW_MS) return;
    // A window far longer than it asked for was not a slow device: the tab was
    // throttled, or something outside the loop blocked the main thread. Judging
    // it would step the whole world down for a stall it cannot fix.
    if (elapsedMs > QUALITY_SAMPLE_WINDOW_MS * 3) {
      ctx.qualitySampleStartedAt = time;
      ctx.qualitySampleFrames = 0;
      return;
    }
    const fps = (ctx.qualitySampleFrames * 1000) / elapsedMs;
    ctx.qualitySampleStartedAt = time;
    ctx.qualitySampleFrames = 0;
    qualityLastFps = fps;
    const decision = adaptiveQualityDecision({
      fps,
      targetFps: 1000 / ctx.renderFrameIntervalMs,
      tier: ctx.qualityProfile.tier,
      bootTier: ctx.qualityBaseTier,
      goodWindows: qualityGoodWindows,
      msSinceChange: performance.now() - ctx.qualityChangedAt,
    });
    qualityGoodWindows = decision.good ? qualityGoodWindows + 1 : 0;
    if (decision.action === "hold") return;
    applyQualityTier(
      decision.tier,
      decision.action === "downgrade" ? "frame-rate" : "recovered",
    );
  }

  function qualityState() {
    const index = WORLD_QUALITY_TIERS.indexOf(ctx.qualityProfile.tier);
    const baseIndex = WORLD_QUALITY_TIERS.indexOf(ctx.qualityBaseTier);
    return {
      tier: ctx.qualityProfile.tier,
      label: ctx.qualityProfile.label,
      bootTier: ctx.bootQualityTier,
      baseTier: ctx.qualityBaseTier,
      // Positive means the ladder has stepped down from where it started.
      steps: index >= 0 && baseIndex >= 0 ? index - baseIndex : 0,
      adaptive: ctx.adaptiveQualityEnabled,
      reason: qualityLastReason,
      sampledFps: Math.round(qualityLastFps),
      targetFps: Math.round(1000 / ctx.renderFrameIntervalMs),
      goodWindows: qualityGoodWindows,
      shadows:
        ctx.qualityProfile.shadows &&
        ctx.shadowsElementEnabled &&
        !ctx.shadowsFarCamera,
      pixelRatioCap: ctx.qualityProfile.pixelRatioCap,
      pixelBudget: ctx.qualityProfile.pixelBudget,
      canvasTextureScale,
      pointLightBudget: ctx.qualityProfile.pointLightBudget,
      weatherParticles: ctx.qualityProfile.weatherParticles,
      starCount: ctx.qualityProfile.starCount,
      maxSatellites: ctx.qualityProfile.maxSatellites,
      lodDistanceScale: ctx.qualityProfile.lodDistanceScale,
      farLodZoom: ctx.qualityProfile.farLodZoom,
      visualAnimationMs: Math.round(ctx.qualityProfile.visualAnimationMs * 100) / 100,
      changes: qualityChangeLog.map((entry) => ({ ...entry })),
    };
  }

  /**
   * Pin the world to a tier. QA drives the device matrix with this, and the
   * support path uses it when a visitor reports a world they cannot use. A
   * manual pin rebases the ladder so it never climbs above the pinned tier,
   * and `adaptive: false` stops it from moving at all.
   */
  function setQualityTier(tier, { adaptive = null } = {}) {
    if (adaptive !== null) ctx.adaptiveQualityEnabled = adaptive !== false;
    if (!WORLD_QUALITY_PROFILES[tier]) return qualityState();
    const requestedIndex = WORLD_QUALITY_TIERS.indexOf(tier);
    const bootIndex = WORLD_QUALITY_TIERS.indexOf(ctx.bootQualityTier);
    // A live context owns its boot-time buffers. It can always move to a
    // cheaper tier, but it must not claim to have restored stars, plates, or
    // residency that were never allocated. Select a more expensive tier with
    // ?quality= before construction instead.
    const resolvedTier =
      requestedIndex >= 0 && bootIndex >= 0 && requestedIndex < bootIndex
        ? ctx.bootQualityTier
        : tier;
    ctx.qualityBaseTier = resolvedTier;
    if (resolvedTier !== ctx.qualityProfile.tier) {
      applyQualityTier(resolvedTier, "requested");
    }
    return qualityState();
  }

  function updateSceneLevelOfDetail(force = false) {
    const far =
      ctx.officeSceneMode === "town" &&
      ctx.cameraMode === "third-person" &&
      ctx.cameraZoom >= ctx.qualityProfile.farLodZoom;
    if (!force && ctx.farSceneDetail === far) {
      syncOfficeFloorVisibility();
      fns.updateCompactDistrictResidency();
      updateLocalPointLightBudget(far);
      return;
    }
    ctx.farSceneDetail = far;
    fns.applyShadowQuality(far);

    syncOfficeFloorVisibility();
    fns.syncEnclosureSceneVisibility(force);
    fns.updateCompactDistrictResidency(force);

    updateLocalPointLightBudget(far);
    // Desktop keeps the complete aerial view. Compact devices retain the
    // terrain but page satellite detail in by physical proximity so a mobile
    // GPU never has to hold the entire World at once.
  }

  function updateWorldEnvironment() {
    const now = new Date();
    const observedMinute = now.getHours() * 60 + now.getMinutes();
    const minuteOfDay =
      ctx.daylightMode === "day"
        ? 12 * 60
        : ctx.daylightMode === "night"
          ? 0
          : observedMinute;
    ctx.localDaylightMinute = minuteOfDay;
    const localHour = minuteOfDay / 60;
    // Smooth civil-time daylight: sunrise ramps from 06:00–08:00 and sunset
    // from 18:00–20:00 in the visitor's own timezone.
    const sunrise = clamp((localHour - 6) / 2, 0, 1);
    const sunset = clamp((20 - localHour) / 2, 0, 1);
    const daylight = Math.min(sunrise, sunset);
    const easedDaylight = daylight * daylight * (3 - 2 * daylight);
    ctx.worldSky.setDaylightMinute?.(minuteOfDay, easedDaylight);
    // The sky bodies draw with tone mapping and fog off, so the exposure and
    // the lights an overlay dims never reach them. Hand the preset over
    // explicitly, or a rain sky keeps a full-strength summer sun in it.
    const skyOverlay = LOCAL_ENVIRONMENT_OVERLAYS[currentTheme];
    ctx.worldSky.setEnvironmentTint?.({
      tint: skyOverlay?.tint || "",
      strength: skyOverlay ? skyOverlay.tintStrength : 0,
      brightness: skyOverlay?.exposureMultiplier ?? 1,
    });
    const solarAngle = ((localHour - 6) / 14) * Math.PI;
    const state = {
      background: new THREE.Color(DAYLIGHT_ENVIRONMENT.background),
      hemiSky: new THREE.Color(DAYLIGHT_ENVIRONMENT.hemiSky),
      hemiGround: new THREE.Color(DAYLIGHT_ENVIRONMENT.hemiGround),
      sun: new THREE.Color(DAYLIGHT_ENVIRONMENT.sun),
      sunPower: DAYLIGHT_ENVIRONMENT.sunPower,
      hemiPower: DAYLIGHT_ENVIRONMENT.hemiPower,
      exposure: DAYLIGHT_ENVIRONMENT.exposure,
    };
    state.background.lerp(new THREE.Color("#78aeca"), easedDaylight);
    state.hemiSky.lerp(new THREE.Color("#dff5ff"), easedDaylight);
    state.hemiGround.lerp(new THREE.Color("#566a48"), easedDaylight);
    state.sun.lerp(new THREE.Color("#fff6d5"), easedDaylight);
    state.sunPower *= 0.04 + easedDaylight * 0.96;
    state.hemiPower *= 0.42 + easedDaylight * 0.58;
    state.exposure *= 0.72 + easedDaylight * 0.28;
    const overlay = LOCAL_ENVIRONMENT_OVERLAYS[currentTheme];
    if (overlay) {
      const tint = new THREE.Color(overlay.tint);
      state.background.lerp(tint, overlay.tintStrength);
      state.hemiSky.lerp(tint, overlay.tintStrength * 0.42);
      state.sun.lerp(tint, overlay.tintStrength * 0.18);
    }
    ctx.scene.background.copy(state.background);
    ctx.scene.fog?.color.copy(state.background);
    const lightMultiplier = ctx.lightLevel / LIGHT_LEVEL_DEFAULT;
    ctx.hemisphere.color.copy(state.hemiSky);
    ctx.hemisphere.groundColor.copy(state.hemiGround);
    ctx.hemisphere.intensity =
      state.hemiPower *
      (ctx.scene.environment ? SKY_ENVIRONMENT_HEMISPHERE_SHARE : 1) *
      (overlay?.lightMultiplier || 1) *
      lightMultiplier;
    // The environment is a daytime sky, so it fades with the sun.
    ctx.scene.environmentIntensity =
      SKY_ENVIRONMENT_INTENSITY *
      (0.15 + easedDaylight * 0.85) *
      (overlay?.lightMultiplier || 1) *
      lightMultiplier;
    ctx.sun.color.copy(state.sun);
    ctx.sun.intensity =
      state.sunPower * (overlay?.lightMultiplier || 1) * lightMultiplier;
    const solarX = Math.cos(solarAngle) * 92;
    const solarY = 8 + Math.max(0, Math.sin(solarAngle)) * 82;
    const solarZ = Math.sin(solarAngle * 0.72) * 58;
    ctx.sun.position.set(solarX, solarY, solarZ);
    ctx.moon.intensity =
      (0.08 + (1 - easedDaylight) * 0.9) *
      (overlay?.lightMultiplier || 1) *
      lightMultiplier;
    ctx.moon.position.set(-solarX, 44 + (1 - easedDaylight) * 30, -solarZ);
    ctx.renderer.toneMappingExposure =
      state.exposure *
      (overlay?.exposureMultiplier || 1) *
      lightMultiplier;
  }

  // Weather is an outdoor effect: the field is parked while the visitor is
  // inside the Office tower so a preset never rains through its ceilings.
  function weatherFieldVisible(kind) {
    if (ctx.officeSceneMode !== "town") return false;
    return kind === "rain"
      ? currentTheme === "rain"
      : currentTheme === "snow" || currentTheme === "winter";
  }

  // Wrap whichever field is showing back around the visitor. This runs from
  // the animation loop as well as from setTheme(), and deliberately sits
  // outside the reduced-motion gate: a visitor who has asked for less motion
  // still gets the preset they picked, it simply does not fall.
  function ensureWeather() {
    if (ctx.weather) return ctx.weather;
    ctx.weather = createWeather(THREE, ctx.scene);
    fns.registerWorldElement(
      "weather", "Weather (rain & snow)", "Systems",
      [ctx.weather.rain, ctx.weather.snow],
    );
    applyWeatherParticleBudget(ctx.weatherParticleBudget);
    return ctx.weather;
  }

  function syncWeatherField() {
    const rainVisible = weatherFieldVisible("rain");
    const snowVisible = weatherFieldVisible("snow");
    if (!ctx.weather && !rainVisible && !snowVisible) return;
    ensureWeather();
    ctx.weather.rain.visible = rainVisible;
    ctx.weather.snow.visible = snowVisible;
    if (!ctx.weather.rain.visible && !ctx.weather.snow.visible) return;
    ctx.weather.anchor.set(
      ctx.player.position.x,
      ctx.player.position.y,
      ctx.player.position.z,
    );
    followWeather(ctx.weather.rain, ctx.weather.anchor);
    followWeather(ctx.weather.snow, ctx.weather.anchor);
  }

  // What a visitor can actually see of their preset, and where it is. A field
  // still centred on the map origin while the avatar stands 300m away is the
  // shape of the reported fault, so the centre and the worst-case distance
  // from the avatar are reported rather than only which layer is switched on.
  function weatherFieldState() {
    if (!ctx.weather) {
      return {
        kind: "",
        rain: false,
        snow: false,
        anchor: [0, 0, 0],
        center: [0, 0, 0],
        span: [0, 0, 0],
        farthest: 0,
      };
    }
    const field = ctx.weather.rain.visible
      ? ctx.weather.rain
      : ctx.weather.snow.visible
        ? ctx.weather.snow
        : null;
    const state = {
      kind: String(field?.userData?.kind || ""),
      rain: ctx.weather.rain.visible === true,
      snow: ctx.weather.snow.visible === true,
      anchor: ctx.weather.anchor.toArray(),
      center: [0, 0, 0],
      span: [0, 0, 0],
      farthest: 0,
    };
    if (!field) return state;
    const positions = field.geometry.attributes.position.array;
    const drawn = weatherDrawCount(field);
    const low = [Infinity, Infinity, Infinity];
    const high = [-Infinity, -Infinity, -Infinity];
    for (let index = 0; index < drawn * 3; index += 3) {
      for (let axis = 0; axis < 3; axis += 1) {
        const value = positions[index + axis];
        if (value < low[axis]) low[axis] = value;
        if (value > high[axis]) high[axis] = value;
      }
      state.farthest = Math.max(
        state.farthest,
        Math.hypot(
          positions[index] - ctx.weather.anchor.x,
          positions[index + 2] - ctx.weather.anchor.z,
        ),
      );
    }
    state.center = low.map((value, axis) => (value + high[axis]) / 2);
    state.span = high.map((value, axis) => value - low[axis]);
    return state;
  }

  function getEnvironmentState() {
    const skyState = ctx.worldSky.getState();
    return {
      theme: currentTheme,
      lightLevel: ctx.lightLevel,
      sunIntensity: ctx.sun.intensity,
      hemisphereIntensity: ctx.hemisphere.intensity,
      exposure: ctx.renderer.toneMappingExposure,
      sunPosition: ctx.sun.position.toArray(),
      background: `#${ctx.scene.background.getHexString()}`,
      weather: weatherFieldState(),
      // Only the preset-driven part of the sky. The rest of worldSky's state
      // (which bodies are drawn, where the satellites are) tracks the camera
      // and the clock, neither of which a personal environment preset owns.
      sky: {
        tint: skyState.environmentTint,
        tintStrength: skyState.environmentTintStrength,
        brightness: skyState.environmentBrightness,
      },
    };
  }

  function setTheme(theme) {
    currentTheme =
      theme === "world" || LOCAL_ENVIRONMENT_OVERLAYS[theme]
        ? theme
        : "world";
    syncWeatherField();
    updateWorldEnvironment();
  }

  function setLightLevel(value) {
    const numeric = Number(value);
    ctx.lightLevel = clamp(
      Number.isFinite(numeric) ? numeric : LIGHT_LEVEL_DEFAULT,
      LIGHT_LEVEL_MIN,
      LIGHT_LEVEL_MAX,
    );
    updateWorldEnvironment();
    return ctx.lightLevel;
  }

  function setDaylightMode(value) {
    ctx.daylightMode =
      value === "day" || value === "night" ? value : "auto";
    updateWorldEnvironment();
    return ctx.daylightMode;
  }

  return {
    ensurePointLightCount,
    updateLocalPointLightBudget,
    syncOfficeFloorVisibility,
    applyRendererResolution,
    applyWeatherParticleBudget,
    sampleAdaptiveQuality,
    qualityState,
    setQualityTier,
    updateSceneLevelOfDetail,
    updateWorldEnvironment,
    syncWeatherField,
    getEnvironmentState,
    setTheme,
    setLightLevel,
    setDaylightMode,
  };
}
