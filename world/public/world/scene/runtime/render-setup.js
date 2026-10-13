import { bakeSkyEnvironment } from "../environment.js";
import { worldQualityProfile } from "../quality.js";
import {
  CONTINUOUS_CITY_SURFACE_Y,
  GROUND_PROBE_DROP,
  GROUND_PROBE_RISE,
} from "../layout.js";

export function functions({
  THREE,
  labelLayer,
  ctx,
}) {
  let profilerOwnerRoots = new Map();
  let profilerOwnerRootsAt = -Infinity;
  // A dedicated layer keeps sprites, lines, particles, and labels out of the
  // triangle inspector. Meshes retain their ordinary layer as well, so
  // restoring the normal camera mask is lossless.
  const TRIANGLE_VIEW_LAYER = 31;
  const groundProbeHits = [];

  // Height of the walkable ground at (x, z), searching from `fromY`. Returns
  // `fallback` where nothing walkable is beneath the point: district packages
  // carry their own floors, so the caller keeps the height it already had.
  const groundHeightCache = {
    surfaces: null,
    count: -1,
    x: 0,
    z: 0,
    fromY: NaN,
    fallback: NaN,
    height: 0,
  };

  function worldElementOwner(object) {
    const at = performance.now();
    if (at - profilerOwnerRootsAt >= 5000) {
      profilerOwnerRootsAt = at;
      profilerOwnerRoots = new Map();
      ctx.worldElements.forEach((element) => {
        element.roots.forEach((root) => {
          if (!profilerOwnerRoots.has(root)) {
            profilerOwnerRoots.set(root, element.label);
          }
        });
      });
    }
    const mirrored = object?.userData?.instancedMirror === true
      ? object.userData.mirrorOwnerSource
      : null;
    for (const start of mirrored && mirrored !== object ? [mirrored, object] : [object]) {
      let node = start;
      while (node) {
        const label = profilerOwnerRoots.get(node);
        if (label) return label;
        node = node.parent;
      }
    }
    return "";
  }

  function tagTriangleViewMeshes() {
    ctx.scene.traverse((child) => {
      if (child.isMesh) child.layers.enable(TRIANGLE_VIEW_LAYER);
      // Ground is one flat, textured walking surface. Keeping it on the
      // normal layer prevents the diagnostic wireframe from drawing the
      // internal triangles that every rendered surface needs underneath its
      // texture. Disable it here as well as skipping future ground meshes so
      // repeated inspector toggles remain clean.
      if (child.isMesh && child.userData.ground) {
        child.layers.disable(TRIANGLE_VIEW_LAYER);
      }
    });
  }

  function setTriangleView(enabled = false) {
    const active = enabled === true;
    if (active === ctx.triangleViewEnabled) return ctx.triangleViewEnabled;
    ctx.triangleViewEnabled = active;
    if (active) {
      ctx.triangleViewCameraMask = ctx.camera.layers.mask;
      ctx.triangleViewLabelsHidden = labelLayer.hidden;
      tagTriangleViewMeshes();
      ctx.camera.layers.set(TRIANGLE_VIEW_LAYER);
      ctx.scene.overrideMaterial = ctx.triangleViewMaterial;
      labelLayer.hidden = true;
    } else {
      ctx.camera.layers.mask = ctx.triangleViewCameraMask;
      ctx.scene.overrideMaterial = null;
      labelLayer.hidden = ctx.triangleViewLabelsHidden;
    }
    return ctx.triangleViewEnabled;
  }
  function buildSkyEnvironment() {
    if (ctx.compactRenderer) return;
    ctx.skyEnvironment?.dispose();
    ctx.skyEnvironment = null;
    ctx.skyEnvironmentPmrem?.dispose();
    ctx.skyEnvironmentPmrem = null;
    const prepared = ctx.preparedSkyEnvironment;
    ctx.preparedSkyEnvironment = null;
    const baked = prepared || bakeSkyEnvironment(THREE, ctx.renderer);
    ctx.skyEnvironment = baked?.environment || null;
    ctx.skyEnvironmentPmrem = baked?.pmrem || null;
    ctx.scene.environment = ctx.skyEnvironment?.texture || null;
  }
  function applyShadowQuality(far = ctx.shadowsFarCamera) {
    ctx.shadowsFarCamera = far === true;
    const on =
      ctx.qualityProfile.shadows && ctx.shadowsElementEnabled && !ctx.shadowsFarCamera;
    const programs = on || worldQualityProfile(ctx.bootQualityTier).shadows === true;
    ctx.sun.castShadow = programs;
    ctx.renderer.shadowMap.enabled = programs;
    ctx.sun.shadow.intensity = on ? 1 : 0;
    ctx.renderer.shadowMap.needsUpdate = on;
  }

  function walkableGroundSurfaces() {
    if (ctx.worldBuilt && ctx.groundSurfaces?.length) return ctx.groundSurfaces;
    ctx.groundSurfaces = [];
    ctx.world.traverse((child) => {
      if (child.isMesh && child.userData.ground) ctx.groundSurfaces.push(child);
    });
    return ctx.groundSurfaces;
  }

  function groundHeightAt(x, z, fromY, fallback) {
    const surfaces = walkableGroundSurfaces();
    if (!surfaces.length) return fallback;
    const cache = groundHeightCache;
    const dx = x - cache.x;
    const dz = z - cache.z;
    if (
      cache.surfaces === surfaces &&
      cache.count === surfaces.length &&
      cache.fromY === fromY &&
      cache.fallback === fallback &&
      dx * dx + dz * dz < 1e-6
    ) {
      return cache.height;
    }
    ctx.groundProbeOrigin.set(x, fromY + GROUND_PROBE_RISE, z);
    ctx.groundProbe.set(ctx.groundProbeOrigin, ctx.groundProbeDown);
    ctx.groundProbe.near = 0;
    ctx.groundProbe.far = GROUND_PROBE_RISE + GROUND_PROBE_DROP;
    groundProbeHits.length = 0;
    ctx.groundProbe.intersectObjects(surfaces, false, groundProbeHits);
    const height = groundProbeHits.length ? groundProbeHits[0].point.y : fallback;
    cache.surfaces = surfaces;
    cache.count = surfaces.length;
    cache.x = x;
    cache.z = z;
    cache.fromY = fromY;
    cache.fallback = fallback;
    cache.height = height;
    return height;
  }

  // Where a visitor arriving at (x, z) stands. Teleports read the destination
  // rather than the height they are leaving, so they land on the surface they
  // are aimed at instead of dropping onto it.
  function outdoorFloorYAt(x, z) {
    return groundHeightAt(
      x,
      z,
      CONTINUOUS_CITY_SURFACE_Y,
      CONTINUOUS_CITY_SURFACE_Y,
    );
  }

  return {
    worldElementOwner,
    tagTriangleViewMeshes,
    setTriangleView,
    buildSkyEnvironment,
    applyShadowQuality,
    groundHeightAt,
    outdoorFloorYAt,
  };
}
