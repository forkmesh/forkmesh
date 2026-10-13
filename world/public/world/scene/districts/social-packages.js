import { BUNDLED_WORLD_PACKAGES } from "../../world-marketplace-packages.js";
import {
  OFFICE_ISLAND_CENTER,
  officeHexWallPoint,
} from "../../world-office-tower.js";
import {
  BLOG_BANNER_OPTIONS,
  MASTODON_KIOSK_REFRESH_MS,
  createMastodonKiosk,
  createSocialBanner,
  drawMastodonCountdown,
  drawMastodonLastPost,
  socialBannerTexture,
} from "../boards.js";
import { repaintCanvasTexture } from "../canvas.js";
import {
  CONTINUOUS_CITY_SURFACE_Y,
  REMOTE_DISTRICT_RADIUS,
  WORLD_RADIUS,
} from "../layout.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { packageSpace } from "../movement.js";
import {
  clamp,
  disposeObject3D,
  removeInteractiveObject,
  setShadows,
} from "../object-utils.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  onOfficeElevatorSound,
  ctx,
  fns,
}) {
  // Post artwork (the blog feed's item images) is a same-origin /assets URL:
  // it only reaches the board texture once it decodes CORS-clean, and its
  // load repaints the boards that draw art. A host that refuses the read
  // simply leaves the placeholder plate in place.
  const socialBannerImages = new Map();


  function socialBannerImage(url) {
    const key = String(url || "");
    if (!key) return null;
    const cached = socialBannerImages.get(key);
    if (cached) return cached.image;
    const record = { image: null };
    socialBannerImages.set(key, record);
    const image = new Image();
    image.crossOrigin = "anonymous";
    image.decoding = "async";
    image.onload = () => {
      if (ctx.disposed) return;
      record.image = image;
      for (const banner of ctx.socialBanners) {
        if (banner.options.postArt) repaintSocialBanner(banner);
      }
    };
    image.onerror = () => {};
    image.src = key;
    return null;
  }

  function repaintSocialBanner(record) {
    const face = record.group.getObjectByName(
      `forkmesh-${record.options.id}-banner-face`,
    );
    if (!face?.material) return false;
    ctx.stripedUploads.swapMap(
      face.material,
      socialBannerTexture(THREE, record.options, record.snapshot || null, socialBannerImage),
    );
    return true;
  }

  function socialBannerSnapshot(record, payload) {
    const feed = payload?.[record.feedId];
    if (feed && typeof feed === "object" && feed.state === "ready") {
      return { posts: Array.isArray(feed.posts) ? feed.posts : [] };
    }
    if (
      feed &&
      typeof feed === "object" &&
      record.feedId === "twitter"
    ) {
      return {
        unavailable: true,
        reason: String(feed.reason || "").slice(0, 180),
        humanTodo: String(feed.humanTodo || "").slice(0, 220),
      };
    }
    return null;
  }

  function removeWorldPackage(id) {
    const packageId = String(id || "");
    const deferred = ctx.deferredDistrictInstalls.findIndex(
      (item) => item.packageId === packageId,
    );
    if (deferred >= 0) ctx.deferredDistrictInstalls.splice(deferred, 1);
    const record = ctx.installedWorldPackages.get(packageId);
    if (!record) return false;
    ctx.installedWorldPackages.delete(packageId);
    for (const records of [ctx.socialBanners, ctx.socialBannerTimerRecords]) {
      const index = records.indexOf(record);
      if (index >= 0) records.splice(index, 1);
    }
    if (record.kind === "district") {
      ctx.districtPackages = ctx.districtPackages.filter((item) => item !== record);
      const trampoline = ctx.animated.indexOf(record.animatedTrampoline);
      if (trampoline >= 0) ctx.animated.splice(trampoline, 1);
      record.spaces.forEach((spaceId) => ctx.packageSpaces.delete(spaceId));
      ctx.packageAccessRequested.delete(packageId);
      // A visitor standing in (or riding through) the district comes back to
      // Town before the island under them disappears.
      if (record.spaces.includes(ctx.currentSpace)) {
        fns.setPackageSpace("town-square");
        fns.setSpawn({ space: "town-square", x: -8.1, z: 30, heading: Math.PI });
        fns.syncEnclosureSceneVisibility(true);
      }
    } else {
      fns.removeBillboardFromIsland(record.billboard);
    }
    removeInteractiveObject(ctx.interactive, record.group);
    record.group?.parent?.remove?.(record.group);
    ctx.worldElements.delete(packageId);
    record.instance.dispose?.();
    return true;
  }

  function installBannerPackage(packageId, entry, module, instance) {
    const record = {
      packageId,
      kind: "banner",
      entry,
      module,
      instance,
      group: instance.group,
      options: instance.options,
      feedId: String(instance.feedId),
      snapshot: null,
      billboard: null,
    };
    record.billboard = fns.placeBillboardOnIsland(
      record.group,
      instance.placement,
    );
    ctx.socialBanners.push(record);
    ctx.socialBannerTimerRecords.push(record);
    ctx.installedWorldPackages.set(packageId, record);
    fns.registerWorldElement(
      packageId,
      entry.manifest.label || packageId,
      entry.manifest.category || "Packages",
      record.group,
      () => ctx.installedWorldPackages.get(packageId) === record,
    );
    if (ctx.latestSocialBannerPayload) {
      record.snapshot = socialBannerSnapshot(
        record,
        ctx.latestSocialBannerPayload,
      );
      repaintSocialBanner(record);
    }
    return true;
  }

  // The host capabilities a district package receives. Everything it can
  // touch goes through here: no scene internals leak into package code.
  function districtPackageHost(packageId, module, packageAnimated) {
    return {
      THREE,
      interactive: ctx.interactive,
      animated: packageAnimated,
      disposeObject3D,
      createSocialBanner: (options) =>
        createSocialBanner(THREE, ctx.interactive, options),
      get player() {
        return ctx.player;
      },
      emitSound: (stage, trip) =>
        onOfficeElevatorSound(
          stage,
          trip,
          module.soundProfiles?.[stage] ?? null,
        ),
      requestAccess: () => fns.requestPackageAccess(packageId),
      releaseAccess: () => ctx.packageAccessRequested.delete(packageId),
      setFloorY: fns.setPackageFloorY,
      setSpace: fns.setPackageSpace,
    };
  }

  function installDistrictPackage(packageId, entry, module) {
    const packageAnimated = [];
    const instance = module.install(
      districtPackageHost(packageId, module, packageAnimated),
    );
    if (!instance?.group) {
      instance?.dispose?.();
      return false;
    }
    const animatedTrampoline = (time, delta) => {
      for (let index = 0; index < packageAnimated.length; index += 1) {
        packageAnimated[index](time, delta);
      }
    };
    const spaces = [];
    Object.entries(instance.spaces || {}).forEach(([spaceId, spec]) => {
      if (!packageSpace(spaceId) || !spec) return;
      ctx.packageSpaces.set(spaceId, {
        packageId,
        floorY: Number(spec.floorY) || 0.38,
        radius: Math.min(
          REMOTE_DISTRICT_RADIUS,
          Math.max(WORLD_RADIUS, Number(spec.radius) || WORLD_RADIUS),
        ),
        ceiling: Math.min(
          REMOTE_DISTRICT_RADIUS,
          Math.max(40, Number(spec.ceiling) || 40),
        ),
        label: String(spec.label || entry.manifest.label || packageId),
        remoteHorizon: spec.remoteHorizon === true,
      });
      spaces.push(spaceId);
    });
    const record = {
      packageId,
      kind: "district",
      entry,
      module,
      instance,
      group: instance.group,
      animatedTrampoline,
      spaces,
      // Reused every frame so the update loop allocates nothing.
      frame: {
        time: 0,
        delta: 0,
        get player() {
          return ctx.player;
        },
        setFloorY: fns.setPackageFloorY,
        setSpace: fns.setPackageSpace,
        requestAccess: () => fns.requestPackageAccess(packageId),
        releaseAccess: () => ctx.packageAccessRequested.delete(packageId),
      },
    };
    ctx.world.add(record.group);
    ctx.animated.push(animatedTrampoline);
    ctx.installedWorldPackages.set(packageId, record);
    ctx.districtPackages = [...ctx.districtPackages, record];
    fns.registerWorldElement(
      packageId,
      entry.manifest.label || packageId,
      entry.manifest.category || "Packages",
      record.group,
      () => ctx.installedWorldPackages.get(packageId) === record,
    );
    if (ctx.lastOfficeAccessPayload) instance.setAccess?.(ctx.lastOfficeAccessPayload);
    if (ctx.worldBuilt) instance.onWorldReady?.({ officeInterior: ctx.officeInterior });
    if (
      ctx.pendingPackageSpawn &&
      spaces.includes(String(ctx.pendingPackageSpawn.space || ""))
    ) {
      const spawn = ctx.pendingPackageSpawn;
      ctx.pendingPackageSpawn = null;
      fns.setSpawn(spawn);
      fns.syncEnclosureSceneVisibility(true);
    }
    return true;
  }

  function installLoadedWorldPackage(packageId, entry, module) {
    if (
      module?.manifest?.id !== packageId ||
      typeof module.install !== "function"
    ) {
      return false;
    }
    removeWorldPackage(packageId);
    // Banner boards declare a feed; anything else is a district.
    if (module.feedId || module.manifest.feedId) {
      const instance = module.install({
        createSocialBanner: (options) =>
          createSocialBanner(THREE, ctx.interactive, options),
        disposeObject3D,
      });
      if (
        !instance?.group ||
        !instance?.options ||
        !String(instance.feedId || "")
      ) {
        instance?.dispose?.();
        return false;
      }
      return installBannerPackage(packageId, entry, module, instance);
    }
    if (!ctx.sceneConstructed) {
      ctx.deferredDistrictInstalls.push({ packageId, entry, module });
      return true;
    }
    return installDistrictPackage(packageId, entry, module);
  }

  // Resolves to whether the package is (or is queued to be) installed. Banner
  // boards install synchronously and their promise is already settled; a lazy
  // district resolves once its module has loaded.
  function installWorldPackage(id) {
    const packageId = String(id || "");
    const entry = ctx.worldPackageModules.get(packageId);
    if (!entry || entry.manifest?.id !== packageId) {
      return Promise.resolve(false);
    }
    if (entry.module) {
      return Promise.resolve(
        installLoadedWorldPackage(packageId, entry, entry.module),
      );
    }
    if (typeof entry.load !== "function") return Promise.resolve(false);
    const pending = ctx.pendingWorldPackageInstalls.get(packageId);
    if (pending) return pending;
    const promise = Promise.resolve()
      .then(() => entry.load())
      .then(
        (module) => {
          ctx.pendingWorldPackageInstalls.delete(packageId);
          if (!ctx.running) return false;
          entry.module = module;
          return installLoadedWorldPackage(packageId, entry, module);
        },
        () => {
          ctx.pendingWorldPackageInstalls.delete(packageId);
          return false;
        },
      );
    ctx.pendingWorldPackageInstalls.set(packageId, promise);
    return promise;
  }

  // Repaint the banner faces from the Worker's /api/world/social-posts
  // snapshot. A feed that is missing or unavailable keeps (or returns to)
  // the static sign rather than showing a blank board.
  function updateSocialBanners(payload) {
    ctx.latestSocialBannerPayload =
      payload && typeof payload === "object" ? payload : null;
    for (const record of ctx.socialBanners) {
      record.snapshot = socialBannerSnapshot(record, payload);
      repaintSocialBanner(record);
    }
  }

  // The stand plates under each banner: a per-second countdown to the next
  // feed sync and a per-minute staleness readout. Both key their repaints
  // like the Mastodon kiosk plates so a tick that changes nothing visible
  // never rebuilds a texture.
  function updateSocialBannerTimers(payload) {
    let repainted = false;
    for (const record of ctx.socialBannerTimerRecords) {
      const timers = payload?.[record.options.id];
      if (!timers || typeof timers !== "object") continue;
      const total = Math.max(
        1000, Number(timers.totalMs) || MASTODON_KIOSK_REFRESH_MS);
      const remaining = clamp(Number(timers.remainingMs) || 0, 0, total);
      const loading = Boolean(timers.loading);
      const countdownKey =
        `${loading ? 1 : 0}:${Math.ceil(remaining / 1000)}:${total}`;
      if (countdownKey !== record.countdownKey) {
        record.countdownKey = countdownKey;
        // The dial repaints every second; find it once, not by a subtree
        // search per tick.
        const dial = (record.countdownDial ??= record.group.getObjectByName(
          `forkmesh-${record.options.id}-banner-countdown`,
        ));
        if (
          repaintCanvasTexture(dial?.material, (context) =>
            drawMastodonCountdown(context, remaining, total, loading),
          )
        ) {
          dial.userData.countdownSeconds = Math.ceil(remaining / 1000);
          dial.userData.countdownLoading = loading;
          repainted = true;
        }
      }
      const since =
        Number.isFinite(Number(timers.sinceMs)) && Number(timers.sinceMs) >= 0
          ? Number(timers.sinceMs)
          : null;
      const sinceKey =
        since === null ? "-" : String(Math.floor(since / 60_000));
      if (sinceKey !== record.stalenessKey) {
        record.stalenessKey = sinceKey;
        const plate = (record.stalenessPlate ??= record.group.getObjectByName(
          `forkmesh-${record.options.id}-banner-lastpost`,
        ));
        const config = record.options.staleness;
        if (
          repaintCanvasTexture(plate?.material, (context) =>
            drawMastodonLastPost(
              context, since, config.label, config.freshMs, config.staleMs),
          )
        ) {
          repainted = true;
        }
      }
    }
    return repainted;
  }

  return {
    removeWorldPackage,
    installDistrictPackage,
    installWorldPackage,
    updateSocialBanners,
    updateSocialBannerTimers,
  };
}

export async function build({
  THREE,
  ctx,
  initialInstalledWorldPackages,
  initialRemovedWorldPackages,
  installWorldPackage,
  interactive,
  landmarkObjects,
  placeBillboardOnIsland,
  registerWorldElement,
  sceneSlice,
  world,
}) {
  // Decorative landscaping lives on the Office island but owns no walkable
  // surface or collision metadata. The doorway and its full bridge-width
  // approach remain completely clear.
  const officeLandscaping = new THREE.Group();
  ctx.officeLandscaping = officeLandscaping;
  officeLandscaping.name = "forkmesh-office-landscaping";
  officeLandscaping.position.set(
    OFFICE_ISLAND_CENTER[0],
    OFFICE_ISLAND_CENTER[1],
    OFFICE_ISLAND_CENTER[2],
  );
  const treeTrunkMaterial = makeMaterial(THREE, "#513824", {
    roughness: 0.92,
  });
  const treeCanopyMaterials = ["#174f34", "#226947", "#2d7a50"].map(
    (color) => makeMaterial(THREE, color, { roughness: 0.88 }),
  );
  [
    ["front-left", 0, 12], ["back-left", -11, 12], ["back-left", 11, 13],
    ["front-right", 0, 12], ["back-right", -11, 13], ["back-right", 11, 12],
    ["back", -17, 30], ["back", 0, 33], ["back", 17, 28],
  ].forEach(([side, along, distance], index) => {
    const { x, z } = officeHexWallPoint(side, along, -distance);
    const tree = new THREE.Group();
    tree.name = `forkmesh-office-landscape-tree-${index + 1}`;
    const trunk = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.55, 0.75, 5.2, 9),
      treeTrunkMaterial,
    );
    trunk.position.y = 2.6;
    tree.add(trunk);
    const lowerCanopy = new THREE.Mesh(
      resources.primitive(THREE, "DodecahedronGeometry", 3.8, 0),
      treeCanopyMaterials[index % treeCanopyMaterials.length],
    );
    lowerCanopy.position.y = 6.5;
    lowerCanopy.scale.set(1.1, 0.92, 1.1);
    tree.add(lowerCanopy);
    const upperCanopy = new THREE.Mesh(
      resources.primitive(THREE, "DodecahedronGeometry", 2.8, 0),
      treeCanopyMaterials[(index + 1) % treeCanopyMaterials.length],
    );
    upperCanopy.position.set(0.35, 9.2, -0.2);
    tree.add(upperCanopy);
    tree.position.set(x, CONTINUOUS_CITY_SURFACE_Y, z);
    officeLandscaping.add(tree);
  });
  const bushMaterial = makeMaterial(THREE, "#246b42", { roughness: 0.9 });
  [
    ["front", -18, 7], ["front", -13.5, 7], ["front", -9, 7],
    ["front", 9, 7], ["front", 13.5, 7], ["front", 18, 7],
    ["front-left", -12, 5], ["front-left", 0, 5], ["front-left", 12, 5],
    ["front-right", -12, 5], ["front-right", 0, 5], ["front-right", 12, 5],
    ["back-left", 0, 5], ["back-right", 0, 5],
  ].forEach(([side, along, distance], index) => {
    const { x, z } = officeHexWallPoint(side, along, -distance);
    const bush = new THREE.Mesh(
      resources.primitive(THREE, "DodecahedronGeometry", 2.25, 0),
      bushMaterial,
    );
    bush.name = `forkmesh-office-landscape-bush-${index + 1}`;
    bush.position.set(x, 1.7, z);
    bush.scale.set(1.45, 0.78, 1);
    officeLandscaping.add(bush);
  });
  const flowerStemMaterial = makeMaterial(THREE, "#3d8b50", {
    roughness: 0.9,
  });
  const flowerColors = ["#f28fb8", "#ffd76f", "#a99cff", "#f58e6d"];
  [-20, -16, -12, -8, 8, 12, 16, 20].forEach((along, index) => {
    const { x, z } = officeHexWallPoint("front", along, -12);
    const flower = new THREE.Group();
    flower.name = `forkmesh-office-landscape-flower-${index + 1}`;
    const stem = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.08, 0.1, 1.15, 7),
      flowerStemMaterial,
    );
    stem.position.y = 0.72;
    flower.add(stem);
    const bloom = new THREE.Mesh(
      resources.primitive(THREE, "IcosahedronGeometry", 0.48, 0),
      makeSharedMaterial(THREE, flowerColors[index % flowerColors.length], {
        roughness: 0.68,
        emissive: flowerColors[index % flowerColors.length],
        emissiveIntensity: 0.08,
      }),
    );
    bloom.position.y = 1.42;
    flower.add(bloom);
    flower.position.set(x, CONTINUOUS_CITY_SURFACE_Y, z);
    officeLandscaping.add(flower);
  });
  setShadows(officeLandscaping);
  const officeHighFidelity =
    landmarkObjects.get("office")?.userData?.officeHighFidelity;
  if (officeHighFidelity) {
    officeLandscaping.position.set(0, 0, 0);
    officeHighFidelity.add(officeLandscaping);
  } else {
    world.add(officeLandscaping);
  }
  registerWorldElement(
    "office-landscaping", "Office landscaping", "Scenery", officeLandscaping,
  );
  await sceneSlice();
  const mastodonKiosk = createMastodonKiosk(THREE, interactive);
  world.add(mastodonKiosk);
  registerWorldElement(
    "mastodon-kiosk", "Mastodon kiosk", "Boards & kiosks", mastodonKiosk,
  );
  await sceneSlice();
  const blogBanner = createSocialBanner(
    THREE, interactive, BLOG_BANNER_OPTIONS);
  world.add(blogBanner);
  registerWorldElement(
    "social-banners", "Blog banner", "Boards & kiosks", blogBanner,
  );
  await sceneSlice();
  // Mastodon, the social-board packages, and the blog share the social circle.
  // Explicit order values let a removed package return to the same slot when
  // reinstalled instead of jumping to the end of the ring.
  placeBillboardOnIsland(mastodonKiosk, {
    circle: "social",
    feed: "mastodon",
    order: 100,
  });
  placeBillboardOnIsland(blogBanner, {
    circle: "social",
    feed: "social",
    order: 130,
  });
  const blogBannerRecord = {
    group: blogBanner,
    options: BLOG_BANNER_OPTIONS,
    feedId: "blog",
    snapshot: null,
  };
  const socialBanners = [blogBannerRecord];
  ctx.socialBanners = socialBanners;
  const socialBannerTimerRecords = [blogBannerRecord];
  ctx.socialBannerTimerRecords = socialBannerTimerRecords;
  // Every bundled entry carries its manifest summary up front; a package that
  // is off by default keeps its code behind `load()` so an uninstalled district
  // never joins the initial module graph.
  const worldPackageModules = new Map(
    BUNDLED_WORLD_PACKAGES.map((entry) => [entry.manifest?.id, entry]),
  );
  ctx.worldPackageModules = worldPackageModules;
  const removedWorldPackages = new Set(
    (Array.isArray(initialRemovedWorldPackages)
      ? initialRemovedWorldPackages
      : []
    ).map((id) => String(id || "")),
  );
  const optedInWorldPackages = new Set(
    (Array.isArray(initialInstalledWorldPackages)
      ? initialInstalledWorldPackages
      : []
    ).map((id) => String(id || "")),
  );
  const installedWorldPackages = new Map();
  ctx.installedWorldPackages = installedWorldPackages;
  const pendingWorldPackageInstalls = new Map();
  ctx.pendingWorldPackageInstalls = pendingWorldPackageInstalls;
  // spaceId → { packageId, floorY, radius, ceiling, label, remoteHorizon }
  const packageSpaces = new Map();
  ctx.packageSpaces = packageSpaces;
  // A district needs the avatar and the built world; installs requested
  // before construction finishes wait here.
  const deferredDistrictInstalls = [];
  ctx.deferredDistrictInstalls = deferredDistrictInstalls;

  BUNDLED_WORLD_PACKAGES.forEach((entry) => {
    const packageId = String(entry.manifest?.id || "");
    const wanted =
      entry.installedByDefault === false
        ? optedInWorldPackages.has(packageId)
        : !removedWorldPackages.has(packageId);
    if (wanted) void installWorldPackage(packageId);
  });
  const mastodonKioskWheelTargets = [];
  mastodonKiosk.traverse((child) => {
    if (child.isMesh) mastodonKioskWheelTargets.push(child);
  });
  return {
    officeHighFidelity,
    mastodonKiosk,
    mastodonKioskWheelTargets,
  };
}
