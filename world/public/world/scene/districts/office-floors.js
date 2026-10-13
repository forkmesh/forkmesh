import {
  OFFICE_ELEVATOR_CENTER_X,
  OFFICE_ELEVATOR_CENTER_Z,
  OFFICE_FLOORS,
  OFFICE_FLOOR_HEIGHT,
  OFFICE_ISLAND_CENTER,
  OFFICE_LOUNGE,
  officeFloorY,
  officeHexContains,
  officeHexOutline,
  officeHexWallPoint,
} from "../../world-office-tower.js";
import { BATCH_TEXTURE_SLOTS } from "../avatar-controls.js";
import {
  disposeBatchAtlas,
  mergeObjectBatchesSteps,
  refreshBatchAtlas,
} from "../batching.js";
import { repaintCanvasTexture, wordTexture } from "../canvas.js";
import {
  FIRST_PERSON_EYE_HEIGHT,
  OFFICE_AQUARIUM_BOUNDS,
  officeAquariumLocalPoint,
  OFFICE_ELEVATOR_CUT_MARGIN,
  OFFICE_ELEVATOR_HALF_DEPTH,
  OFFICE_ELEVATOR_HALF_WIDTH,
  OFFICE_LOBBY_SURFACE_Y,
} from "../gameplay-constants.js";
import { makeOfficeWallPlacard } from "../labels.js";
import { SYSTEM_CAPACITY_ENGINEERING_POSITION } from "../layout.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import {
  clamp,
  disposeOwnedGeometry,
  disposeOwnedMaterial,
  removeInteractiveObject,
} from "../object-utils.js";
import { createAquariumControlPanel } from "../aquarium-panel.js";
import { createOfficeMarineAquariumSliced } from "../office-aquarium.js";
import {
  createEngineeringDebugPanel,
  drawEngineeringDebug,
} from "../office-infrastructure.js";
import * as resources from "../resources.js";

const OFFICE_FAR_PROXY_SLICE_MS = 8;
const OFFICE_FAR_ATLAS_SCALE = 0.5;
const OFFICE_AQUARIUM_SLICE_MS = 8;
const OFFICE_AQUARIUM_RELEASE_MARGIN = 80;
const OFFICE_AQUARIUM_RELEASE_MS = 15_000;
const OFFICE_AQUARIUM_DISPOSED_TEXTURE_SLOTS = BATCH_TEXTURE_SLOTS.filter(
  (slot) => slot !== "envMap",
);

export function functions({
  THREE,
  reducedMotion,
  ctx,
  fns,
}) {
  const officeFarBatchMaterials = new Map();
  let officeFarProxyIdle = 0;
  let officeFarAtlases = [];

  const officeInteriorAccents = [
    "#67efb1",
    "#78d6ff",
    "#f2c96d",
    "#ff8fb8",
    "#a89cff",
    "#7ce3d2",
    "#ff9b72",
    "#9fdb79",
    "#d8b47a",
    "#a7dfff",
  ];
  const officeDeferredFloorDisplays = new Map();
  let aquariumControlsNearby = false;
  const AQUARIUM_NEAR_DISTANCE = 4;
  const aquariumLocalScratch = { x: 0, z: 0 };
  const aquariumViewBox = new THREE.Box3();
  const aquariumViewCenter = new THREE.Vector3();
  const aquariumViewSize = new THREE.Vector3();
  const aquariumViewMatrix = new THREE.Matrix4();
  const aquariumViewFrustum = new THREE.Frustum();
  let aquariumViewBoxReady = false;
  let engineeringDebugFrames = 0;
  let engineeringDebugLongestFrameMs = 0;
  let officeAquariumBuild = null;
  let officeAquariumAnimated = [];
  let officeAquariumFarSince = 0;
  let officeAquariumUsers = null;


  function beginOfficeFarProxy() {
    const office = ctx.landmarkObjects.get("office");
    const high = office?.userData?.officeHighFidelity;
    const low = office?.userData?.officeLowPoly;
    if (!high || !low) return null;
    ctx.officeFarProxyDirty = false;
    const townVisibility = new Map([[ctx.officeInterior, true]]);
    ctx.officeFloorGroups.forEach((floorGroup) => townVisibility.set(floorGroup, true));
    ctx.officeAquarium?.group?.traverse((object) => {
      if (object.userData?.farProxySkip === true) townVisibility.set(object, false);
    });
    const steps = mergeObjectBatchesSteps(THREE, high, {
      name: "forkmesh-office-far",
      visibility: townVisibility,
      materials: officeFarBatchMaterials,
      atlasScale: OFFICE_FAR_ATLAS_SCALE,
    });
    return { office, low, steps };
  }
  function finishOfficeFarProxy({ office, low }, proxy) {
    low.children.slice().forEach((child) => {
      low.remove(child);
      if (child.userData?.mergedBatchKey) child.geometry?.dispose();
    });
    low.add(...proxy.children);
    officeFarAtlases.forEach(disposeBatchAtlas);
    officeFarAtlases = proxy.userData.atlases;
    ctx.officeFarSharedTextures = proxy.userData.sharedTextures;
    office.userData.officeDistantDrawCount = proxy.userData.drawCount;
    return true;
  }
  function rebuildOfficeFarProxy() {
    const build = beginOfficeFarProxy();
    if (!build) return false;
    let step = build.steps.next();
    while (!step.done) step = build.steps.next();
    return finishOfficeFarProxy(build, step.value);
  }
  async function rebuildOfficeFarProxySliced(pause, { warm = false } = {}) {
    const build = beginOfficeFarProxy();
    if (!build) return false;
    let step = build.steps.next();
    while (!step.done) {
      await pause();
      if (ctx.disposed) return false;
      step = build.steps.next();
    }
    if (warm) await fns.warmBeforeReveal(step.value);
    if (ctx.disposed) return false;
    return finishOfficeFarProxy(build, step.value);
  }

  function idleSlicePause(sliceMs, timeoutMs) {
    let sliceStartedAt = performance.now();
    return () =>
      performance.now() - sliceStartedAt < sliceMs
        ? null
        : new Promise((resolve) => {
            const resume = () => {
              sliceStartedAt = performance.now();
              resolve();
            };
            if (typeof window.requestIdleCallback === "function") {
              window.requestIdleCallback(resume, { timeout: timeoutMs });
            } else {
              window.setTimeout(resume, 16);
            }
          });
  }

  function scheduleOfficeFarProxyRebuild() {
    if (officeFarProxyIdle || ctx.disposed) return;
    const run = () => {
      if (ctx.disposed) {
        officeFarProxyIdle = 0;
        return;
      }
      const pause = idleSlicePause(OFFICE_FAR_PROXY_SLICE_MS, 500);
      void rebuildOfficeFarProxySliced(pause, { warm: true }).finally(() => {
        officeFarProxyIdle = 0;
      });
    };
    officeFarProxyIdle =
      typeof window.requestIdleCallback === "function"
        ? window.requestIdleCallback(run, { timeout: 2000 })
        : window.setTimeout(run, 200);
  }

  function syncOfficeDistanceLevel(detailed) {
    const lod = ctx.landmarkObjects.get("office")?.userData?.officeDistanceLod;
    if (!lod) return;
    const [highLevel, lowLevel] = lod.levels.map((level) => level.object);
    const wasDetailed = highLevel.visible;
    if (!detailed && (ctx.officeFarProxyDirty || !lowLevel.children.length)) {
      rebuildOfficeFarProxy();
    } else if (!detailed && wasDetailed) {
      scheduleOfficeFarProxyRebuild();
    }
    if (!detailed) officeFarAtlases.forEach(refreshBatchAtlas);
    highLevel.visible = detailed;
    lowLevel.visible = !detailed;
    lod._currentLevel = detailed ? 0 : 1;
    ctx.world.userData.officeDistanceLevel = detailed ? "high" : "far";
  }
  function officeFloorFinishMaterial(index = 0, lobby = false) {
    const accent =
      officeInteriorAccents[index % officeInteriorAccents.length];
    const solidColors = ["#243c35", "#293b3f", "#3d3a2d", "#3e3038"];
    return makeMaterial(
      THREE,
      lobby
        ? "#29473d"
        : solidColors[index % solidColors.length],
      {
        emissive: accent,
        emissiveIntensity: lobby ? 0.045 : 0.025,
        metalness: 0.04,
        roughness: 0.9,
      },
    );
  }
  function officeCeilingFinishMaterial(index = 0, lobby = false) {
    const solidColors = ["#31433d", "#334247", "#454235", "#473842"];
    return makeMaterial(
      THREE,
      lobby
        ? "#354a43"
        : solidColors[index % solidColors.length],
      {
        emissive: lobby ? "#16332a" : "#17231f",
        emissiveIntensity: 0.035,
        metalness: 0.02,
        roughness: 0.94,
        transparent: false,
        opacity: 1,
      },
    );
  }

  // Canvas-backed boards and placards are some of the most expensive interior
  // decorations. Keep their own visibility state inside an identity wrapper,
  // then draw that wrapper only after the visitor enters the matching floor.
  // Lobby displays remain immediate; floor shells and furniture are untouched.
  function addOfficeDeferredFloorDisplay(parent, display, floorId) {
    if (!parent || !display || !floorId || floorId === "lobby") {
      parent?.add?.(display);
      return display;
    }
    const gate = new THREE.Group();
    gate.name = `${display.name || `forkmesh-office-${floorId}-display`}-floor-gate`;
    gate.userData.officeFloorDeferredDisplay = true;
    gate.userData.officeFloorId = floorId;
    gate.visible = false;
    gate.add(display);
    parent.add(gate);
    const displays = officeDeferredFloorDisplays.get(floorId) || [];
    displays.push(gate);
    officeDeferredFloorDisplays.set(floorId, displays);
    return display;
  }

  function syncOfficeDeferredFloorDisplays() {
    let visibleCount = 0;
    officeDeferredFloorDisplays.forEach((displays, floorId) => {
      const visible =
        ctx.officeSceneMode !== "town" && floorId === ctx.officeCurrentFloorId;
      displays.forEach((display) => {
        display.visible = visible;
        if (visible) visibleCount += 1;
      });
    });
    ctx.world.userData.officeDeferredFloorDisplayCount =
      [...officeDeferredFloorDisplays.values()].reduce(
        (count, displays) => count + displays.length,
        0,
      );
    ctx.world.userData.officeDeferredFloorVisibleCount = visibleCount;
  }
  function officeSlabOutline() {
    const [east, frontRight, frontLeft, west, backLeft, backRight] =
      officeHexOutline();
    return [
      east,
      frontRight,
      frontLeft,
      west,
      backLeft,
      [ctx.elevatorCutMinX, backLeft[1]],
      [ctx.elevatorCutMinX, ctx.elevatorCutMaxZ],
      [ctx.elevatorCutMaxX, ctx.elevatorCutMaxZ],
      [ctx.elevatorCutMaxX, backRight[1]],
      backRight,
    ];
  }
  function addOfficeSlab(parent, material, kind, y) {
    const slab = new THREE.Mesh(
      resources.prism(THREE, officeSlabOutline(), OFFICE_LOBBY_SURFACE_Y),
      material,
    );
    slab.name =
      `forkmesh-office-${kind}-slab-` +
      `${parent.userData.officeFloorId || "lobby"}`;
    slab.position.y = y;
    slab.receiveShadow = true;
    parent.add(slab);
    return slab;
  }
  function addOfficeFloorSurface(parent, material) {
    return addOfficeSlab(parent, material, "floor", 0);
  }
  function addOfficeCeilingSurface(parent, material) {
    return addOfficeSlab(
      parent,
      material,
      "ceiling",
      OFFICE_FLOOR_HEIGHT - OFFICE_LOBBY_SURFACE_Y,
    );
  }
  function addOfficeFloorAtmosphere(
    floorGroup,
    floorId,
    floorIndex = 0,
  ) {
    if (!floorGroup || floorId === "rooftop") return;
    const accent =
      officeInteriorAccents[
        floorIndex % officeInteriorAccents.length
      ];
    const upholstery = makeMaterial(THREE, accent, {
      emissive: accent,
      emissiveIntensity: 0.12,
      roughness: 0.72,
    });
    const frame = makeSharedMaterial(THREE, "#1d302c", {
      metalness: 0.42,
      roughness: 0.44,
    });
    const fixtureGeometry = resources.primitive(THREE, "BoxGeometry", 5.2, 0.07, 0.34);
    const fixtureMaterial = resources.material(THREE, {
      color: floorIndex % 2 ? "#dff4ff" : "#fff1d6",
      toneMapped: false,
    }, "MeshBasicMaterial");
    const fixturePoints = [-18, 0, 18].flatMap((z) =>
      [-30, -18, -6, 6, 18, 30]
        .filter((x) => officeHexContains(x, z, 6))
        .map((x) => [x, z]),
    );
    const fixtureGrid = new THREE.InstancedMesh(
      fixtureGeometry,
      fixtureMaterial,
      fixturePoints.length,
    );
    fixtureGrid.name = `forkmesh-office-${floorId}-ceiling-light-grid`;
    const fixtureTransform = new THREE.Object3D();
    let fixtureIndex = 0;
    fixturePoints.forEach(([x, z]) => {
      fixtureTransform.position.set(
        x,
        OFFICE_FLOOR_HEIGHT - 0.3,
        z,
      );
      fixtureTransform.updateMatrix();
      fixtureGrid.setMatrixAt(fixtureIndex, fixtureTransform.matrix);
      fixtureIndex += 1;
    });
    fixtureGrid.instanceMatrix.needsUpdate = true;
    floorGroup.add(fixtureGrid);
    for (const [lightIndex, x] of [-16, 16].entries()) {
      const light = new THREE.PointLight(
        lightIndex ? "#b9e9ff" : "#fff0cf",
        1.45,
        30,
        1.8,
      );
      light.name = `forkmesh-office-${floorId}-ambient-light-${lightIndex + 1}`;
      light.position.set(x, OFFICE_FLOOR_HEIGHT - 1.1, 0);
      light.castShadow = false;
      floorGroup.add(light);
    }
    for (const [chairIndex, x] of [
      -OFFICE_LOUNGE.seatOffsetX,
      OFFICE_LOUNGE.seatOffsetX,
    ].entries()) {
      const chairId = `${floorId}-lounge-${chairIndex + 1}`;
      const chair = new THREE.Group();
      chair.name = `forkmesh-office-${chairId}`;
      const seatTopY = 1.35;
      const seat = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 4.8, 0.34, 3.4),
        upholstery,
      );
      seat.name = `${chair.name}-seat`;
      seat.position.y = seatTopY - 0.17;
      const back = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 4.8, 2.6, 0.38),
        upholstery,
      );
      back.name = `${chair.name}-back`;
      back.position.set(0, 2.25, -1.5);
      const base = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 3.6, 0.9, 2.5),
        frame,
      );
      base.name = `${chair.name}-base`;
      base.position.y = 0.48;
      chair.add(base, seat, back);
      chair.position.set(OFFICE_LOUNGE.x + x, 0, OFFICE_LOUNGE.z);
      chair.rotation.y = x < 0 ? Math.PI / 5 : -Math.PI / 5;
      chair.userData.officeChairId = chairId;
      chair.userData.officeFloorId = floorId;
      chair.userData.officeSeatTopY = seatTopY;
      chair.traverse((child) => {
        if (!child.isMesh) return;
        child.userData.officeChairId = chairId;
        child.userData.officeFloorId = floorId;
        child.userData.interactive = "office-chair";
        ctx.interactive.push(child);
      });
      ctx.officeChairs.set(chairId, chair);
      floorGroup.add(chair);
    }
    const coffeeTable = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 3.2, 3.2, 0.32, 24),
      makeSharedMaterial(THREE, "#5f4938", {
        metalness: 0.08,
        roughness: 0.58,
      }),
    );
    coffeeTable.name = `forkmesh-office-${floorId}-lounge-table`;
    coffeeTable.position.set(OFFICE_LOUNGE.x, 1.1, OFFICE_LOUNGE.z);
    floorGroup.add(coffeeTable);
  }

  function aquariumSavedToggle(key, fallback) {
    try {
      const saved = window.localStorage?.getItem(key);
      if (saved === "on") return true;
      if (saved === "off") return false;
    } catch (_) {}
    return fallback;
  }

  function saveAquariumToggle(key, enabled) {
    try {
      window.localStorage?.setItem(key, enabled ? "on" : "off");
    } catch (_) {}
  }

  function syncAquariumPanel(time = performance.now()) {
    return ctx.aquariumPanel?.sync(time) || null;
  }

  function saveAquariumPanelToggle(id, value) {
    if (id === "backdrop") {
      saveAquariumToggle("forkmesh-world-aquarium-backdrop", value === true);
    } else if (id === "light") {
      saveAquariumToggle("forkmesh-world-aquarium-light", value === true);
    } else if (id === "glass-response") {
      saveAquariumToggle("forkmesh-world-aquarium-glass-curious", value === "curious");
    }
  }

  async function buildOfficeAquarium(pause) {
    const animated = [];
    const officeAquarium = await createOfficeMarineAquariumSliced(
      THREE,
      animated,
      pause,
    );
    if (ctx.disposed) {
      disposeOfficeAquariumGroup(officeAquarium.group);
      return null;
    }
    officeAquarium.group.userData.farProxySkip = true;
    officeAquarium.setBackdropOpaque(
      aquariumSavedToggle("forkmesh-world-aquarium-backdrop", true),
    );
    officeAquarium.setLightEnabled(
      aquariumSavedToggle("forkmesh-world-aquarium-light", true),
    );
    officeAquarium.setGlassResponse(
      aquariumSavedToggle("forkmesh-world-aquarium-glass-curious", true)
        ? "curious"
        : "shy",
    );
    if (officeAquariumUsers) officeAquarium.setUsers(officeAquariumUsers);
    await pause();
    const targets = [];
    const aquariumPanel = createAquariumControlPanel(THREE, officeAquarium, {
      animated,
      interactive: targets,
      reducedMotion,
      onToggle: saveAquariumPanelToggle,
    });
    fns.instanceRepeatedMeshesWithin(officeAquarium.group);
    const lights = [];
    officeAquarium.group.traverse((object) => {
      if (!object.isPointLight) return;
      object.visible = false;
      lights.push(object);
    });
    await fns.warmBeforeReveal(officeAquarium.group);
    if (ctx.disposed) {
      disposeOfficeAquariumGroup(officeAquarium.group);
      return null;
    }
    officeAquarium.group.visible =
      ctx.officeSceneMode === "town" || ctx.officeCurrentFloorId === "lobby";
    ctx.officeInterior.add(officeAquarium.group);
    lights.forEach((light) => ctx.localPointLights.add(light));
    ctx.interactive.push(...targets);
    ctx.animated.push(...animated);
    officeAquariumAnimated = animated;
    aquariumViewBoxReady = false;
    ctx.officeAquarium = officeAquarium;
    ctx.aquariumPanel = aquariumPanel;
    syncAquariumPanel();
    fns.updateLocalPointLightBudget();
    ctx.world.userData.officeAquariumResident = true;
    return officeAquarium;
  }

  function disposeOfficeAquariumGroup(group) {
    const geometries = new Set();
    const materials = new Set();
    const textures = new Set();
    group.traverse((object) => {
      if (object.isInstancedMesh) object.dispose();
      if (object.geometry) geometries.add(object.geometry);
      const list = Array.isArray(object.material)
        ? object.material
        : [object.material];
      for (const material of list) {
        if (!material || material.userData?.forkmeshSharedResource === true) continue;
        materials.add(material);
        for (const slot of OFFICE_AQUARIUM_DISPOSED_TEXTURE_SLOTS) {
          const texture = material[slot];
          if (texture?.isTexture && texture.userData?.forkmeshSharedResource !== true) {
            textures.add(texture);
          }
        }
      }
    });
    textures.forEach((texture) => texture.dispose());
    materials.forEach((material) => disposeOwnedMaterial(material));
    geometries.forEach(disposeOwnedGeometry);
  }

  function releaseOfficeAquarium() {
    const officeAquarium = ctx.officeAquarium;
    if (!officeAquarium) return false;
    const group = officeAquarium.group;
    ctx.officeAquarium = null;
    ctx.aquariumPanel = null;
    aquariumControlsNearby = false;
    aquariumViewBoxReady = false;
    for (const callback of officeAquariumAnimated) {
      const index = ctx.animated.indexOf(callback);
      if (index >= 0) ctx.animated.splice(index, 1);
    }
    officeAquariumAnimated = [];
    removeInteractiveObject(ctx.interactive, group);
    fns.releaseStaticMirrorsWithin(group);
    group.traverse((object) => {
      if (!object.isPointLight) return;
      object.visible = false;
      ctx.localPointLights.delete(object);
    });
    group.removeFromParent();
    fns.updateLocalPointLightBudget();
    disposeOfficeAquariumGroup(group);
    ctx.world.userData.officeAquariumResident = false;
    return true;
  }

  function whenOfficeAquariumReady() {
    if (ctx.officeAquarium) return Promise.resolve(ctx.officeAquarium);
    if (!officeAquariumBuild) {
      const pause = idleSlicePause(
        OFFICE_AQUARIUM_SLICE_MS,
        ctx.officeSceneMode === "town" ? 500 : 50,
      );
      officeAquariumBuild = buildOfficeAquarium(pause).finally(() => {
        officeAquariumBuild = null;
      });
    }
    return officeAquariumBuild;
  }

  function syncOfficeAquariumResidency(cameraDistance, detailDistance) {
    if (ctx.disposed) return;
    if (ctx.officeSceneMode !== "town" || cameraDistance <= detailDistance) {
      officeAquariumFarSince = 0;
      if (!ctx.officeAquarium) whenOfficeAquariumReady();
      return;
    }
    if (
      !ctx.officeAquarium ||
      officeAquariumBuild ||
      cameraDistance <= detailDistance + OFFICE_AQUARIUM_RELEASE_MARGIN
    ) {
      officeAquariumFarSince = 0;
      return;
    }
    const now = performance.now();
    if (!officeAquariumFarSince) {
      officeAquariumFarSince = now;
      return;
    }
    if (now - officeAquariumFarSince < OFFICE_AQUARIUM_RELEASE_MS) return;
    officeAquariumFarSince = 0;
    releaseOfficeAquarium();
  }

  function setOfficeAquariumUsers(members) {
    officeAquariumUsers = members;
    if (ctx.officeAquarium) ctx.officeAquarium.setUsers(members);
  }

  function aquariumInCameraView() {
    const group = ctx.officeAquarium.group;
    if (!aquariumViewBoxReady) {
      group.updateWorldMatrix(true, true);
      aquariumViewBox.setFromCenterAndSize(
        aquariumViewCenter.set(0, ctx.officeAquarium.dimensions.height / 2, 0),
        aquariumViewSize.set(
          ctx.officeAquarium.dimensions.depth + 1,
          ctx.officeAquarium.dimensions.height + 1,
          ctx.officeAquarium.dimensions.length + 1,
        ),
      );
      aquariumViewBox.applyMatrix4(group.matrixWorld);
      aquariumViewBoxReady = true;
    }
    ctx.camera.updateMatrixWorld();
    aquariumViewMatrix.multiplyMatrices(
      ctx.camera.projectionMatrix,
      ctx.camera.matrixWorldInverse,
    );
    aquariumViewFrustum.setFromProjectionMatrix(aquariumViewMatrix);
    return aquariumViewFrustum.intersectsBox(aquariumViewBox);
  }

  function feedOfficeAquarium(time = performance.now()) {
    if (!ctx.officeAquarium) return false;
    const started = ctx.officeAquarium.feed(time);
    ctx.officeAquarium.updateFeeding(time, !reducedMotion);
    syncAquariumPanel(time);
    return started;
  }

  function getOfficeAquariumState(time = performance.now()) {
    if (!ctx.officeAquarium) return null;
    ctx.officeAquarium.updateFeeding(time, !reducedMotion);
    return ctx.officeAquarium.getFeedingState(time);
  }

  function updateOfficeAquariumProximity(time) {
    if (!ctx.officeAquarium) {
      aquariumControlsNearby = false;
      return;
    }
    fns.officeAvatarLocalPosition(ctx.player, ctx.aquariumControlLocalPosition);
    const aquariumLocal = officeAquariumLocalPoint(
      ctx.aquariumControlLocalPosition.x,
      ctx.aquariumControlLocalPosition.z,
      aquariumLocalScratch,
    );
    aquariumControlsNearby =
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId === "lobby" &&
      !ctx.officeElevatorRide;
    const aquariumDistanceX = Math.max(
      OFFICE_AQUARIUM_BOUNDS.minX - aquariumLocal.x,
      0,
      aquariumLocal.x - OFFICE_AQUARIUM_BOUNDS.maxX,
    );
    const aquariumDistanceZ = Math.max(
      OFFICE_AQUARIUM_BOUNDS.minZ - aquariumLocal.z,
      0,
      aquariumLocal.z - OFFICE_AQUARIUM_BOUNDS.maxZ,
    );
    const aquariumDistance = Math.hypot(
      aquariumDistanceX,
      aquariumDistanceZ,
    );
    ctx.officeAquarium.setVisitorProximity(
      aquariumControlsNearby
        ? clamp(1 - aquariumDistance / 9, 0, 1)
        : 0,
      {
        y: ctx.aquariumControlLocalPosition.y + FIRST_PERSON_EYE_HEIGHT,
        z: aquariumLocal.z,
      },
    );
    if (!aquariumControlsNearby) {
      ctx.officeAquarium.setInView(false);
      return;
    }
    ctx.officeAquarium.setInView(
      aquariumDistance < AQUARIUM_NEAR_DISTANCE || aquariumInCameraView(),
    );
    syncAquariumPanel(time);
  }
  function updateEngineeringDebugPanel(time, delta) {
    engineeringDebugFrames += 1;
    engineeringDebugLongestFrameMs = Math.max(
      engineeringDebugLongestFrameMs,
      Math.max(0, Number(delta) || 0) * 1000,
    );
    const elapsed = Math.max(1, time - ctx.engineeringDebugSampleAt);
    if (elapsed < 1000) return;
    // Remote avatars and other live meshes may have joined since triangle
    // view was enabled. Fold them into its mesh-only layer at the same bounded
    // cadence as the monitor rather than adding another per-frame scene walk.
    if (ctx.triangleViewEnabled) fns.tagTriangleViewMeshes();
    const fps = Math.max(
      0,
      Math.min(999, (engineeringDebugFrames * 1000) / elapsed),
    );
    const memory = performance.memory;
    const heapRatio =
      memory && Number(memory.jsHeapSizeLimit) > 0
        ? Number(memory.usedJSHeapSize) / Number(memory.jsHeapSizeLimit)
        : null;
    const statusHigh = (value, warning, critical) =>
      value >= critical ? "critical" : value >= warning ? "warning" : "ok";
    const statusLow = (value, warning, critical) =>
      value <= critical ? "critical" : value <= warning ? "warning" : "ok";
    const metrics = [
      {
        label: "FRAMES / SECOND",
        value: fps.toFixed(1),
        status: statusLow(fps, 50, 30),
        note: "target 60 · sustained under 30 needs attention",
      },
      {
        label: "LONGEST FRAME",
        value: `${engineeringDebugLongestFrameMs.toFixed(1)} MS`,
        status: statusHigh(engineeringDebugLongestFrameMs, 24, 40),
        note: "orange over 24ms · red over 40ms",
      },
      {
        label: "DRAW CALLS",
        value: (Number(ctx.renderer.info?.render?.calls) || 0).toLocaleString(),
        status: statusHigh(Number(ctx.renderer.info?.render?.calls) || 0, 900, 1500),
        note: "renderer submissions in the latest frame",
      },
      {
        label: "TRIANGLES",
        value: (Number(ctx.renderer.info?.render?.triangles) || 0).toLocaleString(),
        status: statusHigh(
          Number(ctx.renderer.info?.render?.triangles) || 0,
          1_200_000,
          2_500_000,
        ),
        note: "visible geometry in the latest frame",
      },
      {
        label: "GEOMETRIES",
        value: (Number(ctx.renderer.info?.memory?.geometries) || 0).toLocaleString(),
        status: statusHigh(
          Number(ctx.renderer.info?.memory?.geometries) || 0,
          1800,
          3200,
        ),
        note: "resident WebGL geometry resources",
      },
      {
        label: "TEXTURES",
        value: (Number(ctx.renderer.info?.memory?.textures) || 0).toLocaleString(),
        status: statusHigh(
          Number(ctx.renderer.info?.memory?.textures) || 0,
          650,
          1100,
        ),
        note: "resident WebGL texture resources",
      },
      {
        label: "GPU PROGRAMS",
        value: (ctx.renderer.info?.programs?.length || 0).toLocaleString(),
        status: statusHigh(ctx.renderer.info?.programs?.length || 0, 140, 240),
        note: "compiled material programs",
      },
      {
        label: "ANIMATION CALLBACKS",
        value: (ctx.animated.length + ctx.frameAnimated.length).toLocaleString(),
        status: statusHigh(
          ctx.animated.length + ctx.frameAnimated.length,
          240,
          420,
        ),
        note: "bounded loops executed by the World frame",
      },
      {
        label: "INTERACTIVE TARGETS",
        value: ctx.interactive.length.toLocaleString(),
        status: statusHigh(ctx.interactive.length, 1800, 3200),
        note: "raycast targets · optimize dense exhibits",
      },
      {
        label: "REMOTE MEMBERS",
        value: ctx.remotePlayers.size.toLocaleString(),
        status: "ok",
        note: "live avatars represented in this client",
      },
      {
        label: "JS HEAP",
        value:
          heapRatio === null
            ? "NOT EXPOSED"
            : `${Math.round(heapRatio * 100)}%`,
        status:
          heapRatio === null ? "ok" : statusHigh(heapRatio, 0.65, 0.82),
        note: "browser-local heap against available limit",
      },
      {
        label: "PIXEL RATIO",
        value: ctx.renderer.getPixelRatio().toFixed(2),
        status: statusHigh(ctx.renderer.getPixelRatio(), 2.25, 3),
        note: "higher values increase fragment work",
      },
    ];
    const overall = metrics.some((metric) => metric.status === "critical")
      ? "critical"
      : metrics.some((metric) => metric.status === "warning")
        ? "warning"
        : "ok";
    const face = ctx.engineeringDebugPanel.userData.face;
    // A 1600×1000 monitor rebuilt from a fresh canvas every second was the
    // World's largest recurring texture churn; redraw the resident canvas
    // instead so the tick is one raster plus a same-size upload.
    repaintCanvasTexture(face.material, (context) =>
      drawEngineeringDebug(context, {
        overall,
        metrics,
        sampledAt: new Date().toLocaleTimeString([], {
          hour: "2-digit",
          minute: "2-digit",
          second: "2-digit",
        }),
      }),
    );
    ctx.engineeringDebugSampleAt = time;
    engineeringDebugFrames = 0;
    engineeringDebugLongestFrameMs = 0;
  }

  return {
    rebuildOfficeFarProxy,
    rebuildOfficeFarProxySliced,
    syncOfficeDistanceLevel,
    officeFloorFinishMaterial,
    officeCeilingFinishMaterial,
    addOfficeDeferredFloorDisplay,
    syncOfficeDeferredFloorDisplays,
    addOfficeFloorSurface,
    addOfficeCeilingSurface,
    addOfficeFloorAtmosphere,
    aquariumSavedToggle,
    saveAquariumPanelToggle,
    syncAquariumPanel,
    feedOfficeAquarium,
    getOfficeAquariumState,
    releaseOfficeAquarium,
    setOfficeAquariumUsers,
    syncOfficeAquariumResidency,
    updateOfficeAquariumProximity,
    whenOfficeAquariumReady,
    updateEngineeringDebugPanel,
  };
}

export async function build({
  THREE,
  addOfficeCeilingSurface,
  addOfficeDeferredFloorDisplay,
  addOfficeFloorAtmosphere,
  addOfficeFloorSurface,
  addOfficeFunFloorProps,
  ctx,
  interactive,
  labelLayer,
  landmarkObjects,
  officeCeilingFinishMaterial,
  officeFloorFinishMaterial,
  registerCompactDistrictRoot,
  registerWorldElement,
  sceneSlice,
  systemCapacityPlatform,
  world,
}) {
  const officeInterior = new THREE.Group();
  officeInterior.name = "forkmesh-office-interior";
  officeInterior.position.set(
    OFFICE_ISLAND_CENTER[0],
    0,
    OFFICE_ISLAND_CENTER[2],
  );
  officeInterior.visible = false;
  officeInterior.userData.officeInterior = true;
  const officeExteriorHigh =
    landmarkObjects.get("office")?.userData?.officeHighFidelity;
  if (officeExteriorHigh) {
    officeInterior.position.set(0, 0, 0);
    officeExteriorHigh.add(officeInterior);
  } else {
    world.add(officeInterior);
  }
  const officeLodWorldPosition = new THREE.Vector3();
  registerWorldElement(
    "office-interior", "Office interior", "Districts", officeInterior,
  );
  await sceneSlice();
  if (!officeExteriorHigh) {
    registerCompactDistrictRoot("office", officeInterior);
  }
  const officeLobbyFloorMaterial = officeFloorFinishMaterial(0, true);
  const officeLobbyCeilingMaterial =
    officeCeilingFinishMaterial(0, true);
  const officeFloorGroups = new Map([["lobby", officeInterior]]);
  ctx.officeFloorGroups = officeFloorGroups;
  const officeFloorAccent = makeMaterial(THREE, "#67efb1", {
    emissive: "#1a9a68",
    emissiveIntensity: 0.82,
    metalness: 0.42,
    roughness: 0.28,
  });
  const elevatorCutMinX =
    OFFICE_ELEVATOR_CENTER_X -
    OFFICE_ELEVATOR_HALF_WIDTH -
    OFFICE_ELEVATOR_CUT_MARGIN;
  ctx.elevatorCutMinX = elevatorCutMinX;
  const elevatorCutMaxX =
    OFFICE_ELEVATOR_CENTER_X +
    OFFICE_ELEVATOR_HALF_WIDTH +
    OFFICE_ELEVATOR_CUT_MARGIN;
  ctx.elevatorCutMaxX = elevatorCutMaxX;
  ctx.elevatorCutMaxZ =
    OFFICE_ELEVATOR_CENTER_Z +
    OFFICE_ELEVATOR_HALF_DEPTH +
    OFFICE_ELEVATOR_CUT_MARGIN;
  addOfficeFloorSurface(officeInterior, officeLobbyFloorMaterial);
  addOfficeCeilingSurface(
    officeInterior,
    officeLobbyCeilingMaterial,
  );
  addOfficeFloorAtmosphere(officeInterior, "lobby", 0);
  await sceneSlice("office-lobby", "Office lobby floor, ceiling & fittings");
  const aquariumControlLocalPosition = new THREE.Vector3();
  OFFICE_FLOORS.slice(1).forEach((floor, interiorIndex) => {
    const floorGroup = new THREE.Group();
    floorGroup.name = `forkmesh-office-floor-${floor.id}`;
    floorGroup.position.y = officeFloorY(floor.id);
    floorGroup.userData.officeFloorId = floor.id;
    const floorFinish = officeFloorFinishMaterial(interiorIndex + 1);
    addOfficeFloorSurface(
      floorGroup,
      floorFinish,
    );
    if (floor.id !== "rooftop") {
      addOfficeCeilingSurface(
        floorGroup,
        officeCeilingFinishMaterial(interiorIndex + 1),
      );
    }
    const sign = makeOfficeWallPlacard(
      THREE,
      `${floor.level + 1} · ${floor.label.toUpperCase()}`,
      floor.description,
      floor.id === "rooftop" ? "#a7dfff" : "#9ef7c6",
      30,
      4.2,
    );
    sign.name = `forkmesh-office-wall-placard-${floor.id}`;
    // Every team label is physically mounted to the rear glass. Plane meshes
    // keep neighboring floor names from hovering through slabs like sprites.
    const signMount = officeHexWallPoint("front-left", 0, 0.48);
    sign.position.set(
      signMount.x,
      floor.id === "rooftop" ? 4.2 : 7.3,
      signMount.z,
    );
    sign.rotation.y = signMount.rotationY;
    addOfficeDeferredFloorDisplay(floorGroup, sign, floor.id);
    addOfficeFloorAtmosphere(
      floorGroup,
      floor.id,
      interiorIndex + 1,
    );
    const lobbyPortal = new THREE.Group();
    lobbyPortal.name = `forkmesh-office-${floor.id}-lobby-portal`;
    lobbyPortal.position.set(2, 3.25, -31);
    const portalRing = new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", 2.45, 0.24, 10, 40),
      makeSharedMaterial(THREE, "#58a6ff", {
        emissive: "#1f6feb",
        emissiveIntensity: 1.7,
        metalness: 0.5,
        roughness: 0.22,
      }),
    );
    const portalFace = new THREE.Mesh(
      resources.primitive(THREE, "CircleGeometry", 2.2, 32),
      resources.material(THREE, {
        color: "#0d419d",
        transparent: true,
        opacity: 0.52,
        side: THREE.DoubleSide,
        forceSinglePass: true,
        depthWrite: false,
      }, "MeshBasicMaterial"),
    );
    portalFace.position.z = -0.02;
    for (const surface of [portalRing, portalFace]) {
      surface.userData.interactive = "office-lobby-return-portal";
      surface.userData.officeFloorId = floor.id;
      interactive.push(surface);
    }
    lobbyPortal.add(portalRing, portalFace);
    const portalLabel = new THREE.Sprite(
      new THREE.SpriteMaterial({
        map: wordTexture(
          THREE,
          "LOBBY PORTAL",
          "CLICK TO RETURN",
          "#79c0ff",
        ),
        transparent: true,
        depthWrite: false,
      }),
    );
    portalLabel.position.set(0, 3.55, 0);
    portalLabel.scale.set(6.4, 2.4, 1);
    lobbyPortal.add(portalLabel);
    addOfficeDeferredFloorDisplay(floorGroup, lobbyPortal, floor.id);
    // Town sees every furnished floor through the glass; inside, only the
    // current story is drawn (syncOfficeFloorVisibility).
    floorGroup.visible = true;
    floorGroup.userData.officeFloorGroup = true;
    officeInterior.add(floorGroup);
    officeFloorGroups.set(floor.id, floorGroup);
  });
  await sceneSlice("office-floors", "Office upper floors");
  const engineeringFloor = officeFloorGroups.get("engineering");
  systemCapacityPlatform.position.set(
    ...SYSTEM_CAPACITY_ENGINEERING_POSITION,
  );
  systemCapacityPlatform.userData.officeFloorId = "engineering";
  addOfficeDeferredFloorDisplay(
    engineeringFloor,
    systemCapacityPlatform,
    "engineering",
  );
  const engineeringDebugPanel = createEngineeringDebugPanel(THREE);
  const debugPanelMount = officeHexWallPoint("back-left", 0, 0.48);
  engineeringDebugPanel.position.set(
    debugPanelMount.x,
    0,
    debugPanelMount.z,
  );
  engineeringDebugPanel.rotation.y = debugPanelMount.rotationY;
  addOfficeDeferredFloorDisplay(
    engineeringFloor,
    engineeringDebugPanel,
    "engineering",
  );
  await sceneSlice("office-floor-displays", "Office floor displays");
  addOfficeFunFloorProps();
  return {
    officeInterior,
    officeLodWorldPosition,
    officeFloorAccent,
    aquariumControlLocalPosition,
    engineeringDebugPanel,
  };
}
