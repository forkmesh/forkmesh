import {
  OFFICE_AVATAR_RADIUS,
  officeCampusSurfaceContains,
} from "../../world-office-tower.js";
import {
  ENCLOSURE_EXTERIOR_KEEP_RADIUS,
  ENCLOSURE_EXTERIOR_TRIANGLE_LIMIT,
  REPOSITORY_ISLAND_CENTER_X,
  REPOSITORY_RING_COLLAPSE_RADIUS,
  REPOSITORY_RING_EXPAND_RADIUS,
  REPOSITORY_RING_COLLAPSED_SCALE,
  REPOSITORY_RING_EXPANSION_RATE,
} from "../layout.js";
import {
  beachWalkSurfaceContains,
  cityWalkSurfaceContains,
  desertWalkSurfaceContains,
} from "../movement.js";
import {
  clamp,
  countTriangles,
  disposeOwnedGeometry,
} from "../object-utils.js";
import {
  COMPACT_DISTRICT_RESIDENCY,
  COMPACT_DISTRICT_VIEW_ENTER_MARGIN,
  COMPACT_DISTRICT_VIEW_EXIT_MARGIN,
} from "../quality.js";

export function functions({
  THREE,
  reducedMotion,
  ctx,
  fns,
}) {
  let activeEnclosureScene = "";
  const enclosureHiddenWorldRoots = new Map();
  let repositoryRingExpansion = 0;
  let repositoryRingActive = false;
  let enclosureCostVerdicts = new WeakMap();
  const enclosureKeepRoots = new Set();
  const enclosureWalkRoots = [];
  const enclosureWalkChildren = [];
  let enclosureWalkScene = "";

  function districtWalkSurfaceContains(x, z, radius) {
    for (let index = 0; index < ctx.districtPackages.length; index += 1) {
      const contains = ctx.districtPackages[index].instance.walkSurfaceContains;
      if (typeof contains === "function" && contains(x, z, radius)) return true;
    }
    return false;
  }

  function releaseCompactDistrictGpuResources(root) {
    const geometries = new Set();
    const textures = new Set();
    root?.traverse?.((child) => {
      if (child.geometry?.isBufferGeometry) geometries.add(child.geometry);
      const materials = Array.isArray(child.material)
        ? child.material
        : child.material
          ? [child.material]
          : [];
      materials.forEach((material) => {
        ctx.compactTextureSlots.forEach((slot) => {
          const texture = material?.[slot];
          if (texture?.isTexture) textures.add(texture);
        });
      });
    });
    let releasedGeometries = 0;
    geometries.forEach((geometry) => {
      if (disposeOwnedGeometry(geometry)) releasedGeometries += 1;
    });
    let releasedTextures = 0;
    textures.forEach((texture) => {
      if (ctx.officeFarSharedTextures.has(texture)) return;
      texture.dispose();
      releasedTextures += 1;
    });
    return { geometries: releasedGeometries, textures: releasedTextures };
  }

  function compactRootResident(district, root) {
    return (
      district.resident !== false ||
      (district.inView && root.userData.compactDistantProxy !== true)
    );
  }

  function compactRootDrawn(district, root) {
    return (
      compactRootResident(district, root) &&
      (
        root.userData.officeInterior !== true ||
        ctx.officeSceneMode !== "town" ||
        ctx.world.userData.officeExteriorDetailLevel === "furnished"
      ) &&
      (
        root.userData.beachScene !== true ||
        ctx.beachSceneActive
      )
    );
  }

  function compactRootVisible(root) {
    return enclosureHiddenWorldRoots.has(root)
      ? enclosureHiddenWorldRoots.get(root)
      : root.visible;
  }

  function syncCompactDistrictRoot(district, root) {
    if (!root) return;
    const resident = compactRootResident(district, root);
    const drawn = compactRootDrawn(district, root);
    if (enclosureHiddenWorldRoots.has(root)) {
      enclosureHiddenWorldRoots.set(root, drawn);
    } else {
      root.visible = drawn;
    }
    root.userData.compactDistrictResident = resident;
    if (resident) return;
    const released = releaseCompactDistrictGpuResources(root);
    district.releasedGeometries += released.geometries;
    district.releasedTextures += released.textures;
  }

  function registerCompactDistrictRoot(id, roots) {
    if (!ctx.compactRenderer) return;
    const district = ctx.compactDistricts.get(id);
    if (!district) return;
    (Array.isArray(roots) ? roots : [roots]).filter(Boolean).forEach((root) => {
      if (district.roots.has(root)) return;
      district.roots.add(root);
      district.boundsDirty = true;
      syncCompactDistrictRoot(district, root);
    });
  }

  function unregisterCompactDistrictRoot(id, root) {
    const district = ctx.compactDistricts.get(id);
    if (district?.roots.delete(root)) district.boundsDirty = true;
  }

  function moveCompactDistrictsWithin(moved, dx, dz) {
    if (!ctx.compactRenderer || !moved) return 0;
    let count = 0;
    ctx.compactDistricts.forEach((district) => {
      let inside = false;
      district.roots.forEach((root) => {
        for (let node = root; node && !inside; node = node.parent) {
          if (node === moved) inside = true;
        }
      });
      if (!inside) return;
      district.x += dx;
      district.z += dz;
      district.boundsDirty = true;
      count += 1;
    });
    if (count) {
      updateCompactDistrictResidency();
      updateCompactDistrictView();
    }
    return count;
  }

  function compactDistrictBounds(district) {
    if (!district.boundsDirty) return district.bounds;
    district.boundsDirty = false;
    district.bounds.makeEmpty();
    district.roots.forEach((root) => {
      if (root.userData.compactDistantProxy === true) return;
      district.bounds.expandByObject(root);
    });
    return district.bounds;
  }

  const compactViewFrustum = new THREE.Frustum();
  const compactViewMatrix = new THREE.Matrix4();
  const compactViewCorner = new THREE.Vector3();

  function compactBoxInView(box, margin) {
    for (const plane of compactViewFrustum.planes) {
      compactViewCorner.set(
        plane.normal.x > 0 ? box.max.x : box.min.x,
        plane.normal.y > 0 ? box.max.y : box.min.y,
        plane.normal.z > 0 ? box.max.z : box.min.z,
      );
      if (plane.distanceToPoint(compactViewCorner) < -margin) return false;
    }
    return true;
  }

  function updateCompactDistrictView() {
    if (!ctx.compactRenderer) return;
    const camera = ctx.camera;
    camera.updateMatrixWorld();
    compactViewMatrix.multiplyMatrices(
      camera.projectionMatrix,
      camera.matrixWorldInverse,
    );
    compactViewFrustum.setFromProjectionMatrix(
      compactViewMatrix,
      camera.coordinateSystem,
      camera.reversedDepth,
    );
    ctx.compactDistricts.forEach((district) => {
      if (!district.roots.size) return;
      const bounds = compactDistrictBounds(district);
      const inView =
        !bounds.isEmpty() &&
        compactBoxInView(
          bounds,
          district.inView
            ? COMPACT_DISTRICT_VIEW_EXIT_MARGIN
            : COMPACT_DISTRICT_VIEW_ENTER_MARGIN,
        );
      if (inView === district.inView) return;
      district.inView = inView;
      if (district.resident !== false) return;
      district.roots.forEach((root) => {
        if (root.userData.compactDistantProxy === true) return;
        syncCompactDistrictRoot(district, root);
      });
    });
  }

  function updateCompactDistrictResidency(force = false) {
    if (!ctx.compactRenderer) return;
    ctx.compactDistricts.forEach((district, id) => {
      district.distance = Math.hypot(
        ctx.player.position.x - district.x,
        ctx.player.position.z - district.z,
      );
      // The active Office floor must remain resident even if an unusual saved
      // pose temporarily lies beyond the outdoor boundary. A camera-close
      // exterior view also stays resident so the clear glass can reveal its
      // furnished LOD on compact renderers.
      const required =
        (
          id === "office" &&
          (
            ctx.officeSceneMode !== "town" ||
            ctx.world.userData.officeExteriorDetailLevel === "furnished"
          )
        ) ||
        (id === "beach" && ctx.beachSceneActive);
      const nextResident =
        required || district.resident === true
          ? required || district.distance <= district.exit
          : district.distance <= district.enter;
      if (!force && district.resident === nextResident) {
        // Floor/zoom synchronization and an administrator toggling an element
        // can legitimately change root.visible between residency samples.
        // Reassert the cold boundary without re-disposing an untouched root.
        district.roots.forEach((root) => {
          if (compactRootVisible(root) !== compactRootDrawn(district, root)) {
            syncCompactDistrictRoot(district, root);
          }
        });
        return;
      }
      if (force) district.boundsDirty = true;
      if (district.resident === true && !nextResident) district.evictions += 1;
      district.resident = nextResident;
      district.roots.forEach((root) => syncCompactDistrictRoot(district, root));
    });
  }

  function enclosureSceneRoots(mode) {
    if (mode === "office") {
      return [
        ctx.landmarkObjects.get("office"),
        ctx.officeBridge,
        ctx.officeLandscaping,
        ctx.officeInterior,
      ].filter(Boolean);
    }
    if (mode === "remote-district") {
      const district = ctx.packageSpaces.get(ctx.currentSpace);
      const record = district
        ? ctx.installedWorldPackages.get(district.packageId)
        : null;
      const roots = [record?.group, ctx.mainlandDistanceLod].filter(Boolean);
      const enclosure = record?.instance?.enclosure;
      const center = Array.isArray(enclosure?.center) ? enclosure.center : null;
      const radius = Number(enclosure?.radius) || 0;
      // Visitors and parked vehicles on this island are part of the local
      // campus, not mainland detail. Keep direct scene roots whose origins sit
      // on the parcel while the furnished city roots are replaced by proxy.
      if (center) {
        ctx.world.children.forEach((root) => {
          if (
            Math.hypot(
              root.position.x - center[0],
              root.position.z - center[2],
            ) <= radius + 48
          ) {
            roots.push(root);
          }
        });
      }
      return roots;
    }
    if (mode === "beach") return [ctx.beachScene, ctx.zipLine];
    return [];
  }

  function enclosureExteriorIsCostly(root, sceneRoot) {
    const cached = enclosureCostVerdicts.get(root);
    if (
      cached &&
      cached.sceneRoot === sceneRoot &&
      Math.abs(cached.x - root.position.x) < 0.5 &&
      Math.abs(cached.z - root.position.z) < 0.5
    ) {
      return cached.costly;
    }
    const costly = enclosureExteriorCostVerdict(root, sceneRoot);
    enclosureCostVerdicts.set(root, {
      sceneRoot,
      x: root.position.x,
      z: root.position.z,
      costly,
    });
    return costly;
  }

  function enclosureExteriorCostVerdict(root, sceneRoot) {
    if (countTriangles(root) <= ENCLOSURE_EXTERIOR_TRIANGLE_LIMIT) return false;
    sceneRoot.getWorldPosition(ctx.enclosureViewCenter);
    ctx.enclosureRootBounds.setFromObject(root);
    if (ctx.enclosureRootBounds.isEmpty()) return false;
    return (
      ctx.enclosureRootBounds
        .getCenter(ctx.enclosureRootCenter)
        .distanceTo(ctx.enclosureViewCenter) > ENCLOSURE_EXTERIOR_KEEP_RADIUS
    );
  }

  function enclosureWalkUnchanged(next, roots) {
    let unchanged =
      enclosureWalkScene === next &&
      enclosureWalkRoots.length === roots.length &&
      enclosureWalkChildren.length === ctx.world.children.length;
    for (let index = 0; unchanged && index < roots.length; index += 1) {
      unchanged = enclosureWalkRoots[index] === roots[index];
    }
    for (
      let index = 0;
      unchanged && index < ctx.world.children.length;
      index += 1
    ) {
      unchanged = enclosureWalkChildren[index] === ctx.world.children[index];
    }
    if (unchanged) return true;
    enclosureWalkScene = next;
    enclosureWalkRoots.length = roots.length;
    for (let index = 0; index < roots.length; index += 1) {
      enclosureWalkRoots[index] = roots[index];
    }
    enclosureWalkChildren.length = ctx.world.children.length;
    for (let index = 0; index < ctx.world.children.length; index += 1) {
      enclosureWalkChildren[index] = ctx.world.children[index];
    }
    return false;
  }

  function syncEnclosureSceneVisibility(force = false) {
    const next = ctx.beachSceneActive
      ? "beach"
      : ctx.officeSceneMode !== "town"
        ? "office"
        : ctx.packageSpaces.get(ctx.currentSpace)?.remoteHorizon
          ? "remote-district"
          : "";
    if (force || next !== activeEnclosureScene) {
      enclosureHiddenWorldRoots.forEach((visible, root) => {
        root.visible = visible;
      });
      enclosureHiddenWorldRoots.clear();
      activeEnclosureScene = next;
      ctx.world.userData.activeEnclosureScene = next;
      enclosureWalkScene = "";
      enclosureCostVerdicts = new WeakMap();
    }
    ctx.beachScene.visible = ctx.beachSceneActive;
    const remoteHorizon = next === "remote-district";
    const mainlandHigh =
      ctx.mainlandDistanceLod.userData.mainlandHighFidelity;
    const mainlandLow = ctx.mainlandDistanceLod.userData.mainlandLowPoly;
    mainlandHigh.visible = !remoteHorizon;
    mainlandLow.visible = remoteHorizon;
    ctx.mainlandDistanceLod._currentLevel = remoteHorizon ? 1 : 0;
    ctx.world.userData.mainlandDetailLevel = remoteHorizon ? "low" : "detailed";
    if (!next) return next;
    const roots = enclosureSceneRoots(next);
    if (enclosureWalkUnchanged(next, roots)) {
      enclosureHiddenWorldRoots.forEach((_, root) => {
        if (root.visible) root.visible = false;
      });
      return next;
    }
    const keep = enclosureKeepRoots;
    keep.clear();
    keep.add(ctx.player);
    roots.forEach((root) => keep.add(root));
    const panoramic = next === "office";
    ctx.world.children.forEach((root) => {
      if (keep.has(root)) {
        if (enclosureHiddenWorldRoots.has(root)) {
          root.visible = enclosureHiddenWorldRoots.get(root);
          enclosureHiddenWorldRoots.delete(root);
        }
        return;
      }
      if (panoramic && !enclosureExteriorIsCostly(root, roots[0])) return;
      if (!enclosureHiddenWorldRoots.has(root)) {
        enclosureHiddenWorldRoots.set(root, root.visible);
      }
      root.visible = false;
    });
    return next;
  }

  function updateRepositoryRingExpansion(delta) {
    const distance = Math.hypot(
      ctx.player.position.x - REPOSITORY_ISLAND_CENTER_X,
      ctx.player.position.z,
    );
    const inTown = ctx.officeSceneMode === "town" && !ctx.beachSceneActive;
    if (!inTown || distance > REPOSITORY_RING_COLLAPSE_RADIUS) {
      repositoryRingActive = false;
    } else if (distance <= REPOSITORY_RING_EXPAND_RADIUS) {
      repositoryRingActive = true;
    }
    const target = repositoryRingActive ? 1 : 0;
    if (repositoryRingExpansion === target) return;
    const next = reducedMotion
      ? target
      : repositoryRingExpansion +
        (target - repositoryRingExpansion) *
          clamp(delta * REPOSITORY_RING_EXPANSION_RATE, 0, 1);
    repositoryRingExpansion = Math.abs(target - next) < 0.004 ? target : next;
    ctx.world.userData.repositoryRingExpansion = repositoryRingExpansion;
    ctx.repositoryPortals.forEach(({ group }) => {
      applyRepositoryRingExpansion(group.userData.repositoryExpandedProfile);
    });
  }

  // A collapsed profile is not merely small, it is not drawn at all — that is
  // the point of leaving the rings folded away until somebody is in the room.
  // At the low end of the expansion it sits exactly on the portal's outline
  // torus, so appearing and disappearing there is seamless.
  function applyRepositoryRingExpansion(profile) {
    if (!profile) return;
    profile.visible = repositoryRingExpansion > 0.002;
    if (!profile.visible) return;
    profile.scale.setScalar(
      REPOSITORY_RING_COLLAPSED_SCALE +
        repositoryRingExpansion * (1 - REPOSITORY_RING_COLLAPSED_SCALE),
    );
  }

  function compactDistrictDiagnostics() {
    if (!ctx.compactRenderer) {
      return {
        enabled: false,
        resident: 0,
        total: 0,
        evictions: 0,
        releasedGeometries: 0,
        releasedTextures: 0,
        active: [],
        inView: [],
      };
    }
    const districts = [...ctx.compactDistricts.entries()].filter(
      ([, district]) => district.roots.size > 0,
    );
    return {
      enabled: true,
      resident: districts.filter(([, district]) => district.resident === true)
        .length,
      total: districts.length,
      evictions: districts.reduce(
        (total, [, district]) => total + district.evictions,
        0,
      ),
      releasedGeometries: districts.reduce(
        (total, [, district]) => total + district.releasedGeometries,
        0,
      ),
      releasedTextures: districts.reduce(
        (total, [, district]) => total + district.releasedTextures,
        0,
      ),
      active: districts
        .filter(([, district]) => district.resident === true)
        .map(([id]) => id)
        .slice(0, 5),
      inView: districts
        .filter(([, district]) => district.inView)
        .map(([id]) => id),
    };
  }

  return {
    districtWalkSurfaceContains,
    registerCompactDistrictRoot,
    unregisterCompactDistrictRoot,
    moveCompactDistrictsWithin,
    updateCompactDistrictResidency,
    updateCompactDistrictView,
    syncEnclosureSceneVisibility,
    updateRepositoryRingExpansion,
    applyRepositoryRingExpansion,
    compactDistrictDiagnostics,
  };
}

export function build({
  THREE,
  beachScene,
  campfire,
  ctx,
  districtWalkSurfaceContains,
  landmarkObjects,
  leaderboardDistrict,
  officeHighFidelity,
  registerCompactDistrictRoot,
  startHereBoard,
  syncEnclosureSceneVisibility,
  world,
}) {
  const activeWalkSurfaceContains = (x, z, radius = OFFICE_AVATAR_RADIUS) =>
    ctx.beachSceneActive
      ? beachWalkSurfaceContains(x, z, radius)
      : cityWalkSurfaceContains(x, z, radius) ||
        desertWalkSurfaceContains(x, z, radius) ||
        officeCampusSurfaceContains(x, z, radius) ||
        districtWalkSurfaceContains(x, z, radius);
  world.userData.repositoryRingExpansion = 0;

  // A hidden Object3D stops draw calls, but Three.js deliberately retains
  // every buffer and texture it uploaded for that object. On unified-memory
  // mobile GPUs, walking past several rich districts therefore used to retain
  // the whole World until the browser reset the context. Compact mode gives
  // each satellite district a residency boundary: cold roots are not drawn,
  // and their reusable CPU-side geometry/canvas sources stay intact while the
  // corresponding WebGL allocations are released. Re-entering a district
  // lazily uploads only that district again.
  const compactDistricts = new Map(
    Object.entries(COMPACT_DISTRICT_RESIDENCY).map(([id, boundary]) => [
      id,
      {
        ...boundary,
        roots: new Set(),
        resident: null,
        inView: false,
        bounds: new THREE.Box3(),
        boundsDirty: true,
        distance: Infinity,
        evictions: 0,
        releasedGeometries: 0,
        releasedTextures: 0,
      },
    ]),
  );
  ctx.compactDistricts = compactDistricts;
  const compactTextureSlots = [
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
  ctx.compactTextureSlots = compactTextureSlots;

  const enclosureViewCenter = new THREE.Vector3();
  ctx.enclosureViewCenter = enclosureViewCenter;
  const enclosureRootBounds = new THREE.Box3();
  ctx.enclosureRootBounds = enclosureRootBounds;
  const enclosureRootCenter = new THREE.Vector3();
  ctx.enclosureRootCenter = enclosureRootCenter;

  registerCompactDistrictRoot(
    "repositories",
    landmarkObjects.get("repositories"),
  );
  registerCompactDistrictRoot("leaderboards", leaderboardDistrict);
  registerCompactDistrictRoot("members", [campfire, startHereBoard]);
  // Keep the Office LOD parent resident so compact mainland visitors still
  // draw its tiny horizon silhouette. Page only the high-fidelity child out;
  // its low-poly sibling remains available for the horizon draw.
  if (officeHighFidelity) {
    officeHighFidelity.userData.compactDistantProxy = true;
  }
  registerCompactDistrictRoot("office", officeHighFidelity);
  registerCompactDistrictRoot("beach", beachScene);
  syncEnclosureSceneVisibility(true);
  return {
    activeWalkSurfaceContains,
  };
}
