import {
  OFFICE_ELEVATOR_CENTER_X,
  OFFICE_ELEVATOR_CENTER_Z,
  OFFICE_ELEVATOR_ROTATION_Y,
  OFFICE_FLOORS,
  OFFICE_HEX_RADIUS,
  OFFICE_LOBBY_FOUNTAIN,
  officeFloorY,
  officeHexWallPoint,
} from "../../world-office-tower.js";
import { worldQaCardPainter, worldQaCardTexture } from "../boards-tasks.js";
import { canvasTexture, repaintCanvasTexture } from "../canvas.js";
import { OFFICE_ELEVATOR_SHAFT_ROOF_HEIGHT } from "../gameplay-constants.js";
import { indexGeometry } from "../geometry-dedupe.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import {
  officeAttendanceDurationLabel,
  officeAttendanceLeaderboardRows,
} from "../office-marketing.js";
import * as resources from "../resources.js";

const LOGO_REFLECTION_FACES = 6;
const LOGO_REFLECTION_RETRY_MS = 400;
export const LOGO_REFLECTION_LAYER = 2;

export function functions({
  THREE,
  ctx,
  fns,
}) {
  const LOUNGE_FIGURE_SLICE_MS = 8;
  let qaCueTimer = 0;
  let logoReflectionDirty = true;
  let logoReflectionWasInLobby = false;
  let logoReflectionWasBusy = false;
  let logoReflectionEligibleAt = Infinity;
  let logoReflectionStep = -1;
  let logoReflectionDeferred = false;
  let logoReflectionWarm = "idle";
  const logoReflectionFrustum = new THREE.Frustum();
  const logoReflectionView = new THREE.Matrix4();
  const logoReflectionSphere = new THREE.Sphere();
  let loungeBuildGeneration = 0;
  let loungeBuildHandle = 0;

  const loungeTasks = new Map();
  let loungeTaskHandle = 0;


  function updateQaBoard(snapshot = {}) {
    ctx.qaBoardSnapshot = {
      ...snapshot,
      view: String(snapshot?.view || "cards"),
      list: Array.isArray(snapshot?.list) ? snapshot.list : [],
    };
    if (!repaintCanvasTexture(ctx.qaBoardFace.material, worldQaCardPainter(ctx.qaBoardSnapshot))) {
      const previous = ctx.qaBoardFace.material.map;
      ctx.qaBoardFace.material.map = worldQaCardTexture(THREE, ctx.qaBoardSnapshot);
      ctx.qaBoardFace.material.needsUpdate = true;
      previous?.dispose?.();
    }
    const failed =
      ctx.qaBoardSnapshot?.current?.verdict === "fail" ||
      (ctx.qaBoardSnapshot?.view === "detail" &&
        ctx.qaBoardSnapshot?.current?.taskQaStatus === "failed");
    ctx.qaBoardFrame.material = failed
      ? makeSharedMaterial(THREE, "#9f1d27", {
          roughness: 0.82,
          emissive: "#4a0007",
          emissiveIntensity: 0.55,
        })
      : makeSharedMaterial(THREE, "#332318", { roughness: 0.82 });
    ctx.qaSwipeCues.visible = ctx.qaBoardSnapshot.view === "cards";
  }

  function showQaSwipeCues(visible, linger = false) {
    if (qaCueTimer) {
      clearTimeout(qaCueTimer);
      qaCueTimer = 0;
    }
    // These large red/green/grey directions stay visible for every tester.
    // Grabbing the card enlarges them briefly without hiding them afterward.
    ctx.qaSwipeCues.visible = true;
    ctx.qaSwipeCues.scale.setScalar(visible ? 1.08 : 1);
    if (visible && linger) {
      qaCueTimer = setTimeout(() => {
        ctx.qaSwipeCues.visible = true;
        ctx.qaSwipeCues.scale.setScalar(1);
        qaCueTimer = 0;
      }, 1800);
    }
  }

  function resetQaCardTransform() {
    ctx.qaBoardFace.position.set(0, 0, 0.21);
    ctx.qaBoardFace.rotation.z = 0;
  }

  function officeAttendanceTexture(snapshot = {}, openElapsedMs = 0) {
    const members = officeAttendanceLeaderboardRows(snapshot, openElapsedMs);
    return canvasTexture(THREE, 1600, 800, (context) => {
      context.fillStyle = "#071714";
      context.fillRect(0, 0, 1600, 800);
      context.strokeStyle = "#79efb5";
      context.lineWidth = 14;
      context.strokeRect(8, 8, 1584, 784);
      context.fillStyle = "#9ef7c6";
      context.font = '900 48px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("OFFICE LEADERBOARD", 42, 62);
      context.fillStyle = "#79a996";
      context.font = '800 27px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("#", 42, 112);
      context.fillText("MEMBER", 116, 112);
      context.fillText("LONGEST STAY", 850, 112);
      context.fillText("STATUS", 1135, 112);
      context.fillText("FLOOR", 1370, 112);
      context.strokeStyle = "rgba(121,239,181,0.28)";
      context.lineWidth = 2;
      context.beginPath();
      context.moveTo(38, 130);
      context.lineTo(1560, 130);
      context.stroke();
      context.font = '700 27px "ForkMesh Mono", ui-monospace, monospace';
      members.forEach((member, index) => {
        const y = 164 + index * 28;
        if (index % 2 === 0) {
          context.fillStyle = "rgba(158,247,198,0.045)";
          context.fillRect(30, y - 20, 1530, 30);
        }
        context.fillStyle = index < 3 ? "#f7c96b" : "#79a996";
        context.fillText(String(index + 1).padStart(2, "0"), 42, y);
        context.fillStyle = "#e9fff6";
        context.fillText(
          String(member?.account || "Contributor").slice(0, 28),
          116,
          y,
        );
        context.fillStyle = index < 3 ? "#f7c96b" : "#a9d7c4";
        context.fillText(
          officeAttendanceDurationLabel(member?.durationMs),
          850,
          y,
        );
        context.fillStyle = member?.present ? "#9ef7c6" : "#6f9d8b";
        context.fillText(
          member?.present ? "IN OFFICE" : "AWAY",
          1135,
          y,
        );
        context.fillStyle = member?.present ? "#9ef7c6" : "#6f9d8b";
        context.fillText(
          member?.present
            ? String(member?.floor || "Lobby").slice(0, 13)
            : "—",
          1370,
          y,
        );
      });
      context.fillStyle = "#83bba7";
      context.font = '650 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        members.length
          ? "One row per member · longest authenticated Office stay"
          : "No ranked members yet · signed-in members clock in automatically",
        42,
        760,
      );
    });
  }

  function officeClockInTexture(snapshot = {}, openElapsedMs = 0) {
    const visits = Array.isArray(snapshot?.visits)
      ? snapshot.visits.slice(0, 20)
      : [];
    const liveElapsedMs = Math.max(0, Number(openElapsedMs) || 0);
    return canvasTexture(THREE, 1600, 800, (context) => {
      const format = (value) => {
        const timestamp = Number(value);
        return Number.isFinite(timestamp) && timestamp > 0
          ? new Date(timestamp).toLocaleString([], {
              month: "short",
              day: "numeric",
              hour: "2-digit",
              minute: "2-digit",
            })
          : "—";
      };
      context.fillStyle = "#071714";
      context.fillRect(0, 0, 1600, 800);
      context.strokeStyle = "#77d9ff";
      context.lineWidth = 14;
      context.strokeRect(8, 8, 1584, 784);
      context.fillStyle = "#9eefff";
      context.font = '900 48px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("OFFICE CLOCK · LAST 20 PUNCHES", 42, 62);
      context.fillStyle = "#79a9b5";
      context.font = '800 27px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("USER", 42, 112);
      context.fillText("IN", 420, 112);
      context.fillText("OUT", 790, 112);
      context.fillText("FLOOR", 1195, 112);
      context.fillText("TOTAL", 1435, 112);
      context.strokeStyle = "rgba(119,217,255,0.28)";
      context.lineWidth = 2;
      context.beginPath();
      context.moveTo(38, 130);
      context.lineTo(1560, 130);
      context.stroke();
      context.font = '700 27px "ForkMesh Mono", ui-monospace, monospace';
      visits.forEach((visit, index) => {
        const y = 164 + index * 28;
        if (index % 2 === 0) {
          context.fillStyle = "rgba(119,217,255,0.045)";
          context.fillRect(30, y - 20, 1530, 30);
        }
        context.fillStyle = "#e9fbff";
        context.fillText(
          String(visit?.account || "Contributor").slice(0, 22),
          42,
          y,
        );
        context.fillStyle = "#a9d2dc";
        context.fillText(format(visit?.inAt), 420, y);
        context.fillStyle = visit?.outAt ? "#a9d2dc" : "#f7c96b";
        context.fillText(
          visit?.outAt ? format(visit.outAt) : "IN BUILDING",
          790,
          y,
        );
        context.fillStyle = visit?.outAt ? "#6f929b" : "#9eefff";
        context.fillText(
          visit?.outAt
            ? "—"
            : String(visit?.floor || "Lobby").slice(0, 18),
          1195,
          y,
        );
        const durationMs =
          visit?.durationMs === null || visit?.durationMs === undefined
            ? null
            : Number(visit.durationMs) +
              (visit?.inAt && !visit?.outAt ? liveElapsedMs : 0);
        context.fillStyle = visit?.outAt ? "#a9d2dc" : "#f7c96b";
        context.fillText(
          officeAttendanceDurationLabel(durationMs),
          1435,
          y,
        );
      });
      context.fillStyle = "#83aeb8";
      context.font = '650 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        visits.length
          ? "One row per punch · authenticated Office check-ins"
          : "No punches yet · signed-in members clock in automatically",
        42,
        760,
      );
    });
  }
  function logoReflectionRoots() {
    const roots = [ctx.player];
    const office = ctx.landmarkObjects.get("office");
    if (office) roots.push(office);
    return roots;
  }
  function markLogoReflectionLayer() {
    ctx.scene.traverse((object) => {
      if (object.isLight) object.layers.enable(LOGO_REFLECTION_LAYER);
    });
    const stack = logoReflectionRoots().slice();
    while (stack.length) {
      const object = stack.pop();
      if (object.userData?.logoReflectionSkip === true) continue;
      if ((object.layers.mask & 1) !== 0) object.layers.enable(LOGO_REFLECTION_LAYER);
      for (let index = 0; index < object.children.length; index += 1) {
        stack.push(object.children[index]);
      }
    }
  }
  function renderLogoReflectionFace(face) {
    const renderer = ctx.renderer;
    const reflectionCamera = ctx.reflectionCamera;
    const target = reflectionCamera.renderTarget;
    if (reflectionCamera.coordinateSystem !== renderer.coordinateSystem) {
      reflectionCamera.coordinateSystem = renderer.coordinateSystem;
      reflectionCamera.updateCoordinateSystem();
    }
    const previousTarget = renderer.getRenderTarget();
    const previousFace = renderer.getActiveCubeFace();
    const previousLevel = renderer.getActiveMipmapLevel();
    const xrEnabled = renderer.xr.enabled;
    const mipmaps = target.texture.generateMipmaps;
    const deferredBefore = ctx.shaderCompileGuard.deferredDraws;
    const matrixAutoUpdate = ctx.scene.matrixWorldAutoUpdate;
    const shadowUpdate = renderer.shadowMap.needsUpdate;
    renderer.xr.enabled = false;
    target.texture.generateMipmaps = mipmaps && face === LOGO_REFLECTION_FACES - 1;
    ctx.scene.matrixWorldAutoUpdate = false;
    renderer.shadowMap.needsUpdate = false;
    try {
      renderer.setRenderTarget(target, face, reflectionCamera.activeMipmapLevel);
      renderer.render(ctx.scene, reflectionCamera.children[face]);
    } finally {
      ctx.scene.matrixWorldAutoUpdate = matrixAutoUpdate;
      renderer.shadowMap.needsUpdate = shadowUpdate;
      target.texture.generateMipmaps = mipmaps;
      renderer.setRenderTarget(previousTarget, previousFace, previousLevel);
      renderer.xr.enabled = xrEnabled;
    }
    if (face === LOGO_REFLECTION_FACES - 1) target.texture.needsPMREMUpdate = true;
    return ctx.shaderCompileGuard.deferredDraws !== deferredBefore;
  }
  function logoCubeOnScreen() {
    const cube = ctx.chromeCube;
    if (!cube) return true;
    ctx.camera.updateMatrixWorld();
    logoReflectionView.multiplyMatrices(
      ctx.camera.projectionMatrix,
      ctx.camera.matrixWorldInverse,
    );
    logoReflectionFrustum.setFromProjectionMatrix(logoReflectionView);
    cube.getWorldPosition(logoReflectionSphere.center);
    logoReflectionSphere.radius = 6;
    return logoReflectionFrustum.intersectsSphere(logoReflectionSphere);
  }
  function updateOfficeLogoReflection(time) {
    const inLobby =
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId === "lobby" &&
      !ctx.officeElevatorRide;
    if (!inLobby) {
      logoReflectionWasInLobby = false;
      logoReflectionWasBusy = false;
      logoReflectionDirty = true;
      logoReflectionEligibleAt = Infinity;
      logoReflectionStep = -1;
      return;
    }
    if (!logoReflectionWasInLobby) {
      logoReflectionWasInLobby = true;
      if (logoReflectionWarm === "idle") {
        logoReflectionWarm = "pending";
        void fns.warmLogoReflection().then(() => {
          logoReflectionWarm = "ready";
        });
      }
      logoReflectionDirty = true;
      logoReflectionEligibleAt = time + ctx.logoReflectionSettleMs;
      logoReflectionStep = -1;
      return;
    }
    const busy =
      ctx.officeLobbyPlayerMoving ||
      ctx.primaryPointerId !== null ||
      ctx.pinchActive;
    if (busy) {
      logoReflectionDirty = true;
      logoReflectionWasBusy = true;
      logoReflectionEligibleAt = time + ctx.logoReflectionSettleMs;
      logoReflectionStep = -1;
      return;
    }
    if (logoReflectionWasBusy) {
      logoReflectionWasBusy = false;
      logoReflectionEligibleAt = time + ctx.logoReflectionSettleMs;
      return;
    }
    if (logoReflectionStep < 0) {
      if (!(logoReflectionDirty && time >= logoReflectionEligibleAt)) return;
      if (logoReflectionWarm === "pending" || !logoCubeOnScreen()) return;
      logoReflectionStep = 0;
      logoReflectionDeferred = false;
      markLogoReflectionLayer();
    }
    if (logoReflectionStep === LOGO_REFLECTION_FACES) {
      logoReflectionStep = -1;
      if (logoReflectionDeferred) {
        logoReflectionEligibleAt = time + LOGO_REFLECTION_RETRY_MS;
        return;
      }
      if (ctx.skyEnvironmentPmrem) {
        ctx.logoReflectionEnvironment = ctx.skyEnvironmentPmrem.fromCubemap(
          ctx.reflectionTarget.texture,
          ctx.logoReflectionEnvironment,
        );
        applyOfficeLogoEnvironment();
      }
      logoReflectionDirty = false;
      logoReflectionEligibleAt = Infinity;
      ctx.reflectionCamera.userData.logoCaptureCount += 1;
      ctx.reflectionCamera.userData.logoCapturedPlayer = true;
      ctx.reflectionCamera.userData.logoCapturedPlayerId =
        String(ctx.player.userData?.id || ctx.player.name || "local-player");
      return;
    }
    const cubeWasVisible = ctx.chromeCube.visible;
    const playerWasVisible = ctx.player.visible;
    const selfWorkBadge = ctx.player.userData?.selfWorkBadge;
    const selfWorkBadgeWasVisible = selfWorkBadge?.visible;
    ctx.chromeCube.visible = false;
    ctx.player.visible = true;
    if (selfWorkBadge) selfWorkBadge.visible = false;
    ctx.player.updateWorldMatrix(true, true);
    ctx.reflectionCamera.updateWorldMatrix(true, true);
    try {
      if (renderLogoReflectionFace(logoReflectionStep)) logoReflectionDeferred = true;
    } finally {
      if (selfWorkBadge) {
        selfWorkBadge.visible = selfWorkBadgeWasVisible;
      }
      ctx.player.visible = playerWasVisible;
      ctx.chromeCube.visible = cubeWasVisible;
    }
    logoReflectionStep += 1;
  }

  function applyOfficeLogoEnvironment() {
    const map = ctx.logoEnvironmentMap();
    ctx.chrome.envMap = map;
    ctx.darkChrome.envMap = map;
    ctx.logoPanelChrome.envMap = map;
  }
  // A restored context rebuilt the sky and lost the filtered capture.
  function resetOfficeLogoEnvironment() {
    ctx.logoReflectionEnvironment?.dispose();
    ctx.logoReflectionEnvironment = null;
    applyOfficeLogoEnvironment();
    logoReflectionDirty = true;
  }
  function queueLoungeTask(name, task) {
    loungeTasks.delete(name);
    loungeTasks.set(name, task);
    if (loungeTaskHandle) return;
    const step = () => {
      loungeTaskHandle = 0;
      if (ctx.disposed) {
        loungeTasks.clear();
        return;
      }
      const next = loungeTasks.entries().next().value;
      if (!next) return;
      loungeTasks.delete(next[0]);
      next[1]();
      if (loungeTasks.size) loungeTaskHandle = window.setTimeout(step, 0);
    };
    loungeTaskHandle = window.setTimeout(step, 0);
  }

  function scheduleLoungeFigures(jobs, dress, slotAt) {
    loungeBuildGeneration += 1;
    const generation = loungeBuildGeneration;
    if (loungeBuildHandle) {
      if (typeof window.cancelIdleCallback === "function") {
        window.cancelIdleCallback(loungeBuildHandle);
      } else {
        window.clearTimeout(loungeBuildHandle);
      }
      loungeBuildHandle = 0;
    }
    const focus = ctx.player?.position;
    const distance = new Map(
      jobs.map((job) => {
        const slot = slotAt(job.index);
        return [
          job,
          slot && focus
            ? Math.hypot(
                ctx.campfire.position.x + slot.x - focus.x,
                ctx.campfire.position.z + slot.z - focus.z,
              )
            : Infinity,
        ];
      }),
    );
    const queue = jobs
      .slice()
      .sort((a, b) => distance.get(a) - distance.get(b) || a.index - b.index);
    const run = () => {
      const started = performance.now();
      let dressed = 0;
      while (queue.length) {
        if (dressed > 0 && performance.now() - started >= LOUNGE_FIGURE_SLICE_MS) {
          break;
        }
        dress(queue.shift());
        dressed += 1;
      }
      return queue.length > 0;
    };
    const step = () => {
      loungeBuildHandle = 0;
      if (ctx.disposed || generation !== loungeBuildGeneration) return;
      if (!run()) return;
      loungeBuildHandle =
        typeof window.requestIdleCallback === "function"
          ? window.requestIdleCallback(step, { timeout: 120 })
          : window.setTimeout(step, 16);
    };
    if (run()) {
      loungeBuildHandle =
        typeof window.requestIdleCallback === "function"
          ? window.requestIdleCallback(step, { timeout: 120 })
          : window.setTimeout(step, 16);
    }
  }

  return {
    updateQaBoard,
    showQaSwipeCues,
    resetQaCardTransform,
    officeAttendanceTexture,
    officeClockInTexture,
    updateOfficeLogoReflection,
    logoReflectionRoots,
    markLogoReflectionLayer,
    resetOfficeLogoEnvironment,
    queueLoungeTask,
    scheduleLoungeFigures,
  };
}

export async function build({
  THREE,
  animated,
  compactRenderer,
  ctx,
  interactive,
  officeAttendanceTexture,
  officeClockInTexture,
  officeFloorAccent,
  officeInterior,
  sceneSlice,
}) {
  // Shared, bounded attendance leaderboard on the left lobby wall.
  // Authenticated punches are server-timestamped and ranked by each member's
  // longest completed or currently active Office stay.
  const officeAttendanceBoard = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 27, 13.2),
    resources.uniqueMaterial(THREE, { toneMapped: false }, "MeshBasicMaterial"),
  );
  officeAttendanceBoard.name = "forkmesh-office-attendance";
  const attendanceMount = officeHexWallPoint("front-left", 0, 0.5);
  officeAttendanceBoard.position.set(attendanceMount.x, 7.1, attendanceMount.z);
  officeAttendanceBoard.rotation.y = attendanceMount.rotationY;
  officeInterior.add(officeAttendanceBoard);
  officeAttendanceBoard.material.map = officeAttendanceTexture();

  // Preserve the individual punch ledger alongside the member leaderboard.
  // It lives on the opposite lobby wall at the same scale, so the two views
  // remain independently readable instead of competing for one canvas.
  const officeClockInBoard = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 27, 13.2),
    resources.uniqueMaterial(THREE, { toneMapped: false }, "MeshBasicMaterial"),
  );
  officeClockInBoard.name = "forkmesh-office-clock-in";
  const clockInMount = officeHexWallPoint("front-right", 0, 0.5);
  officeClockInBoard.position.set(clockInMount.x, 7.1, clockInMount.z);
  officeClockInBoard.rotation.y = clockInMount.rotationY;
  officeInterior.add(officeClockInBoard);
  officeClockInBoard.material.map = officeClockInTexture();

  // A true reflective capture, throttled while the visitor is inside. The
  // sculpture follows the supplied chrome FM cube: branch graph on top, F/M
  // reliefs on its sides, slowly rotating over a lit fountain.
  await sceneSlice("office-boards", "Office attendance & clock-in boards");
  const logoFountain = new THREE.Group();
  logoFountain.name = "forkmesh-office-logo-fountain";
  logoFountain.position.set(
    OFFICE_LOBBY_FOUNTAIN.x,
    0,
    OFFICE_LOBBY_FOUNTAIN.z,
  );
  const logoBasin = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 8.2, 8.8, 1.1, 48),
    makeSharedMaterial(THREE, "#d6e2e9", {
      metalness: 0.88,
      roughness: 0.12,
    }),
  );
  logoBasin.position.y = 0.55;
  logoFountain.add(logoBasin);
  const logoWater = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 7.55, 7.55, 0.18, 48),
    makeSharedMaterial(THREE, "#6fd9ff", {
      transparent: true,
      opacity: 0.72,
      emissive: "#1a789d",
      emissiveIntensity: 0.55,
      metalness: 0.15,
      roughness: 0.08,
    }),
  );
  logoWater.position.y = 1.12;
  logoFountain.add(logoWater);
  const fountainGlow = new THREE.PointLight("#91eaff", 4.2, 25, 1.7);
  fountainGlow.position.set(0, 4.8, 0);
  logoFountain.add(fountainGlow);
  await sceneSlice();
  const reflectionTarget = new THREE.WebGLCubeRenderTarget(
    compactRenderer ? 128 : 256,
    {
      generateMipmaps: true,
      minFilter: THREE.LinearMipmapLinearFilter,
    },
  );
  // Be explicit about reflection rather than relying on the render target's
  // library default. The same live cube map then drives the mirrored letters
  // and panels without ever turning into a refracted/inside-out view.
  reflectionTarget.texture.mapping = THREE.CubeReflectionMapping;
  reflectionTarget.texture.name = "forkmesh-office-logo-live-reflection";
  const reflectionCamera = new THREE.CubeCamera(
    0.2,
    OFFICE_HEX_RADIUS * 2,
    reflectionTarget,
  );
  reflectionCamera.name = "forkmesh-office-logo-reflection-camera";
  reflectionCamera.userData.logoCaptureCount = 0;
  reflectionCamera.userData.logoCapturePolicy = "dirty-idle-once";
  reflectionCamera.position.y = 5.1;
  reflectionCamera.layers.set(LOGO_REFLECTION_LAYER);
  logoFountain.add(reflectionCamera);
  // Standard materials only take filtered (PMREM) environments, so three.js
  // filters a live cube target itself the first time a material using it is
  // compiled. The idle shader warm-up compiled the Office from Town, which made
  // it filter an empty target synchronously behind the warm-up's own queue of
  // shaders: a 500-600ms frame. Where the sky environment exists, each capture
  // is filtered here with the sky's generator instead (the chrome reflects the
  // sky until the first one), so the chrome always holds a finished map. Both
  // maps share a size, so switching between them never recompiles the chrome.
  let logoReflectionEnvironment = null;
  const logoEnvironmentMap = () =>
    logoReflectionEnvironment?.texture ||
    ctx.skyEnvironment?.texture ||
    reflectionTarget.texture;
  const chrome = resources.uniqueMaterial(THREE, {
    // A neutral mid-silver base leaves headroom for the live cube map. Nearly
    // white metal clipped those reflected lobby greens and dark window bands
    // into a flat white surface under ACES tone mapping.
    color: "#dbe4ef",
    metalness: 0.94,
    roughness: 0.055,
    emissive: "#0b1417",
    emissiveIntensity: 0.14,
    clearcoat: 1,
    clearcoatRoughness: 0.02,
    envMap: logoEnvironmentMap(),
    envMapIntensity: 1.45,
  }, "MeshPhysicalMaterial");
  const darkChrome = resources.uniqueMaterial(THREE, {
    color: "#05070b",
    metalness: 1,
    roughness: 0.06,
    clearcoat: 1,
    envMap: logoEnvironmentMap(),
    envMapIntensity: 1.5,
  }, "MeshPhysicalMaterial");
  const logoInnerFace = resources.uniqueMaterial(THREE, {
    color: "#030807",
    metalness: 0.18,
    roughness: 0.72,
  }, "MeshStandardMaterial");
  const logoPanelChrome = chrome.clone();
  logoPanelChrome.color.set("#f0f5ff");
  logoPanelChrome.metalness = 0.8;
  logoPanelChrome.roughness = 0.07;
  logoPanelChrome.envMapIntensity = 1.3;
  logoPanelChrome.emissive.set("#111c20");
  logoPanelChrome.emissiveIntensity = 0.16;
  const chromeCube = new THREE.Group();
  chromeCube.name = "forkmesh-reflective-fm-cube";
  // The resting view presents the F/M corner together. Motion-enabled clients
  // then continue from this pose around the same world-vertical axis.
  chromeCube.rotation.y = 0;
  // Only this outer mount animates. Its Y rotation is the single vertical
  // spindle through the fountain; the inner edge-balanced tilt never changes.
  const chromeMark = new THREE.Group();
  chromeMark.name = "forkmesh-reflective-fm-cube-fixed-tilt";
  chromeCube.add(chromeMark);
  const logoHalfSize = 2.92;
  const logoSupportTopY = 2.4;
  // Align the original lower body diagonal with world-down. The selected
  // corner therefore remains exactly over the sole support while the outer
  // mount spins around the world-vertical axis.
  const logoLowerCorner = new THREE.Vector3(-1, -1, -1).normalize();
  chromeMark.quaternion.setFromUnitVectors(
    logoLowerCorner,
    new THREE.Vector3(0, -1, 0),
  );
  chromeCube.position.y = logoSupportTopY + logoHalfSize * Math.sqrt(3);
  reflectionCamera.position.y = chromeCube.position.y;
  const logoPiece = (
    parent,
    width,
    height,
    depth,
    x,
    y,
    z,
    rotationZ = 0,
  ) => {
    const piece = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", width, height, depth),
      // Every face group is authored with local +Z pointing out of the cube.
      // Only that public face is mirror-bright. Matte sides and inward backs
      // keep the repeated opposite F/M marks from flashing through the hollow
      // center as fragmented chrome strokes.
      [
        logoInnerFace,
        logoInnerFace,
        logoInnerFace,
        logoInnerFace,
        chrome,
        logoInnerFace,
      ],
    );
    piece.position.set(x, y, z);
    piece.rotation.z = rotationZ;
    parent.add(piece);
    return piece;
  };
  const logoLetterStroke = 1.04;

  const addFLogoFace = (z, rotationY) => {
    const face = new THREE.Group();
    face.name =
      `forkmesh-reflective-f-face-${z > 0 ? "front" : "back"}`;
    face.position.z = z;
    face.rotation.y = rotationY;
    logoPiece(face, logoLetterStroke, 4.9, 0.48, -1.64, -0.04, 0);
    logoPiece(face, 4.35, logoLetterStroke, 0.48, 0.12, 2.0, 0);
    logoPiece(face, 3.45, logoLetterStroke, 0.48, -0.33, 0.22, 0);
    chromeMark.add(face);
  };
  addFLogoFace(2.68, 0);
  addFLogoFace(-2.68, Math.PI);

  const addMLogoFace = (x, rotationY) => {
    const face = new THREE.Group();
    face.name =
      `forkmesh-reflective-m-face-${x > 0 ? "right" : "left"}`;
    face.position.x = x;
    face.rotation.y = rotationY;
    logoPiece(face, logoLetterStroke, 4.9, 0.48, -1.72, -0.04, 0);
    logoPiece(face, logoLetterStroke, 4.9, 0.48, 1.72, -0.04, 0);
    const addDiagonal = (fromX, fromY, toX, toY) => {
      const dx = toX - fromX;
      const dy = toY - fromY;
      logoPiece(
        face,
        logoLetterStroke,
        Math.hypot(dx, dy),
        0.48,
        (fromX + toX) / 2,
        (fromY + toY) / 2,
        0,
        Math.atan2(-dx, dy),
      );
    };
    addDiagonal(-1.72, 2.0, 0, 0.08);
    addDiagonal(0, 0.08, 1.72, 2.0);
    chromeMark.add(face);
  };
  addMLogoFace(2.68, Math.PI / 2);
  addMLogoFace(-2.68, -Math.PI / 2);
  await sceneSlice();

  // Rounded mirrored top and bottom plates match the supplied cube silhouette.
  // The branch graph is built into the Shape as seven negative-space paths:
  // four circular nodes and three connecting slots. Extruding that Shape
  // leaves true holes through both chrome plates, including reflective inner
  // bevels, instead of placing dark decals over otherwise solid metal.
  const logoPanelShape = new THREE.Shape();
  const panelSize = 5.72;
  const panelHalf = panelSize / 2;
  const panelRadius = 0.34;
  logoPanelShape.moveTo(-panelHalf + panelRadius, -panelHalf);
  logoPanelShape.lineTo(panelHalf - panelRadius, -panelHalf);
  logoPanelShape.quadraticCurveTo(
    panelHalf,
    -panelHalf,
    panelHalf,
    -panelHalf + panelRadius,
  );
  logoPanelShape.lineTo(panelHalf, panelHalf - panelRadius);
  logoPanelShape.quadraticCurveTo(
    panelHalf,
    panelHalf,
    panelHalf - panelRadius,
    panelHalf,
  );
  logoPanelShape.lineTo(-panelHalf + panelRadius, panelHalf);
  logoPanelShape.quadraticCurveTo(
    -panelHalf,
    panelHalf,
    -panelHalf,
    panelHalf - panelRadius,
  );
  logoPanelShape.lineTo(-panelHalf, -panelHalf + panelRadius);
  logoPanelShape.quadraticCurveTo(
    -panelHalf,
    -panelHalf,
    -panelHalf + panelRadius,
    -panelHalf,
  );
  logoPanelShape.closePath();
  const topNodes = [
    [-1.8, -1.3],
    [0, -1.8],
    [1.7, -0.4],
    [0.2, 1.7],
  ];
  const topBranches = [
    [[-1.8, -1.3], [0, -1.8]],
    [[0, -1.8], [1.7, -0.4]],
    [[0, -1.8], [0.2, 1.7]],
  ];
  const logoNodeCutoutRadius = 0.5;
  topNodes.forEach(([x, z]) => {
    const hole = new THREE.Path();
    hole.absarc(x, z, logoNodeCutoutRadius, 0, Math.PI * 2, false);
    logoPanelShape.holes.push(hole);
  });
  topBranches.forEach(([[x1, z1], [x2, z2]]) => {
    const dx = x2 - x1;
    const dz = z2 - z1;
    const length = Math.hypot(dx, dz);
    const ux = dx / length;
    const uz = dz / length;
    const halfWidth = 0.16;
    const nx = -uz * halfWidth;
    const nz = ux * halfWidth;
    // A hairline of chrome between independently triangulated holes avoids
    // overlapping Shape holes (which Earcut cannot represent reliably) while
    // remaining visually continuous beneath the panel's bevel.
    const trim = logoNodeCutoutRadius + 0.018;
    const sx = x1 + ux * trim;
    const sz = z1 + uz * trim;
    const ex = x2 - ux * trim;
    const ez = z2 - uz * trim;
    const hole = new THREE.Path();
    hole.moveTo(sx + nx, sz + nz);
    hole.lineTo(ex + nx, ez + nz);
    hole.lineTo(ex - nx, ez - nz);
    hole.lineTo(sx - nx, sz - nz);
    hole.closePath();
    logoPanelShape.holes.push(hole);
  });
  let logoPanelGeometry = new THREE.ExtrudeGeometry(logoPanelShape, {
    depth: 0.36,
    steps: 1,
    curveSegments: 16,
    bevelEnabled: true,
    bevelSegments: 2,
    bevelSize: 0.08,
    bevelThickness: 0.08,
  });
  logoPanelGeometry.translate(0, 0, -0.18);
  logoPanelGeometry.rotateX(Math.PI / 2);
  logoPanelGeometry = indexGeometry(THREE, logoPanelGeometry);
  await sceneSlice();
  for (const faceY of [-2.68, 2.68]) {
    const panel = new THREE.Mesh(logoPanelGeometry, logoPanelChrome);
    panel.name =
      `forkmesh-reflective-fm-panel-${faceY > 0 ? "top" : "bottom"}`;
    panel.position.y = faceY;
    panel.userData.logoThroughCutouts = logoPanelShape.holes.length;
    chromeMark.add(panel);
  }
  const logoContactPoint = new THREE.Mesh(
    resources.primitive(THREE, "OctahedronGeometry", 0.2, 0),
    chrome,
  );
  logoContactPoint.name = "forkmesh-reflective-fm-cube-contact-point";
  logoContactPoint.position.set(
    -logoHalfSize,
    -logoHalfSize,
    -logoHalfSize,
  );
  chromeMark.add(logoContactPoint);
  const logoSupport = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.2, 0.28, 1.28, 20),
    darkChrome,
  );
  logoSupport.name = "forkmesh-reflective-fm-cube-support";
  logoSupport.position.y = logoSupportTopY - 0.64;
  logoFountain.add(logoSupport);
  logoFountain.add(chromeCube);
  for (const [x, y, z, intensity] of [
    [-7, 9, 5, 4.8],
    [6, 7, -5, 3.8],
    [0, 12, 2, 3.2],
  ]) {
    const highlight = new THREE.PointLight(
      x < 0 ? "#dff7ff" : "#ffffff",
      intensity,
      28,
      1.45,
    );
    highlight.position.set(x, y, z);
    logoFountain.add(highlight);
  }
  officeInterior.add(logoFountain);
  // One cube-map refresh renders the scene six times. Treat it as an idle
  // snapshot, not a recurring animation: entry and motion only mark the map
  // dirty, then one capture runs after the visitor has settled. This keeps the
  // Office and the local avatar legible in the chrome without a one-second
  // render spike or visible reflection flash while walking.
  const logoReflectionSettleMs = compactRenderer ? 1400 : 900;
  reflectionCamera.userData.logoCaptureSettleMs = logoReflectionSettleMs;
  animated.push((time) => {
    for (let node = logoFountain; node; node = node.parent) {
      if (!node.visible) return;
    }
    chromeCube.rotation.y = time * 0.00022;
    logoWater.rotation.y = -time * 0.00012;
  });

  // One physical selector rides inside the panoramic car. Access colors are
  // updated from the server-derived grant set; locked buttons stay visible
  // but cannot initiate travel.
  await sceneSlice("office-logo-fountain", "Office logo fountain");
  const officeElevatorButtons = [];
  const officeElevatorShaft = new THREE.Group();
  officeElevatorShaft.name = "forkmesh-office-glass-elevator-shaft";
  officeElevatorShaft.position.set(
    OFFICE_ELEVATOR_CENTER_X,
    0,
    OFFICE_ELEVATOR_CENTER_Z,
  );
  officeElevatorShaft.rotation.y = OFFICE_ELEVATOR_ROTATION_Y;
  const elevatorShaftFrame = makeMaterial(THREE, "#8ccfbd", {
    metalness: 0.82,
    roughness: 0.2,
  });
  const elevatorCarGlass = makeMaterial(THREE, "#c9fff3", {
    transparent: true,
    opacity: 0.13,
    metalness: 0,
    roughness: 0.5,
    depthWrite: false,
  });
  const elevatorShaftRoofY = officeFloorY("rooftop");
  const elevatorShaftTopY =
    elevatorShaftRoofY + OFFICE_ELEVATOR_SHAFT_ROOF_HEIGHT;
  for (const [x, z] of [
    [-5, -4],
    [5, -4],
    [-5, 4],
    [5, 4],
  ]) {
    const beam = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.24, elevatorShaftTopY, 0.24),
      elevatorShaftFrame,
    );
    beam.position.set(x, elevatorShaftTopY / 2, z);
    officeElevatorShaft.add(beam);
  }
  const elevatorShaftCap = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 10.5, 0.32, 8.5),
    elevatorShaftFrame,
  );
  elevatorShaftCap.name = "forkmesh-office-elevator-shaft-cap";
  elevatorShaftCap.position.y = elevatorShaftTopY + 0.16;
  officeElevatorShaft.add(elevatorShaftCap);

  const airConditioner = new THREE.Group();
  airConditioner.name = "forkmesh-office-elevator-air-conditioner";
  airConditioner.position.set(1.1, elevatorShaftTopY + 0.32, 0);
  const airConditionerCasing = makeMaterial(THREE, "#c9d3d1", {
    metalness: 0.58,
    roughness: 0.4,
  });
  const airConditionerDark = makeMaterial(THREE, "#26302f", {
    metalness: 0.4,
    roughness: 0.55,
  });
  const airConditionerBody = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 5.6, 2.1, 4.2),
    airConditionerCasing,
  );
  airConditionerBody.position.y = 1.15;
  airConditioner.add(airConditionerBody);
  for (const [x, z] of [
    [-2.55, -1.85],
    [2.55, -1.85],
    [-2.55, 1.85],
    [2.55, 1.85],
  ]) {
    const foot = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.4, 0.1, 0.4),
      airConditionerDark,
    );
    foot.position.set(x, 0.05, z);
    airConditioner.add(foot);
  }
  for (const x of [-1.35, 1.35]) {
    const grille = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 1.05, 1.05, 0.08, 24),
      airConditionerDark,
    );
    grille.position.set(x, 2.24, 0);
    airConditioner.add(grille);
    const hub = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.22, 0.22, 0.12, 12),
      airConditionerCasing,
    );
    hub.position.set(x, 2.3, 0);
    airConditioner.add(hub);
    for (let blade = 0; blade < 3; blade += 1) {
      const fanBlade = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 0.8, 0.04, 0.26),
        airConditionerCasing,
      );
      const angle = blade * Math.PI * 2 / 3;
      fanBlade.position.set(
        x + Math.cos(angle) * 0.55,
        2.3,
        Math.sin(angle) * 0.55,
      );
      fanBlade.rotation.y = -angle;
      airConditioner.add(fanBlade);
    }
  }
  for (let slat = 0; slat < 5; slat += 1) {
    for (const z of [-2.12, 2.12]) {
      const vent = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 4.8, 0.08, 0.06),
        airConditionerDark,
      );
      vent.position.set(0, 0.55 + slat * 0.3, z);
      airConditioner.add(vent);
    }
  }
  officeElevatorShaft.add(airConditioner);

  const shaftLadder = new THREE.Group();
  shaftLadder.name = "forkmesh-office-elevator-shaft-ladder";
  shaftLadder.position.set(-5.34, elevatorShaftRoofY, -1.8);
  const shaftLadderHeight = OFFICE_ELEVATOR_SHAFT_ROOF_HEIGHT + 1.3;
  for (const z of [-0.62, 0.62]) {
    const ladderRail = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.12, shaftLadderHeight, 0.12),
      airConditionerDark,
    );
    ladderRail.position.set(0, shaftLadderHeight / 2, z);
    shaftLadder.add(ladderRail);
  }
  for (let y = 0.7; y < OFFICE_ELEVATOR_SHAFT_ROOF_HEIGHT + 0.3; y += 0.8) {
    const rung = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.1, 0.1, 1.24),
      airConditionerDark,
    );
    rung.position.y = y;
    shaftLadder.add(rung);
  }
  officeElevatorShaft.add(shaftLadder);
  // The moving car supplies the only glass envelope. Full-height shaft panes
  // created a second parallel layer a few inches away, so their transparent
  // sort order changed whenever the rider turned the camera.
  officeInterior.add(officeElevatorShaft);

  const officeElevatorCar = new THREE.Group();
  officeElevatorCar.name = "forkmesh-office-glass-elevator-car";
  officeElevatorCar.position.set(
    OFFICE_ELEVATOR_CENTER_X,
    0,
    OFFICE_ELEVATOR_CENTER_Z,
  );
  officeElevatorCar.rotation.y = OFFICE_ELEVATOR_ROTATION_Y;
  const elevatorCarFloor = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 9.4, 0.32, 7.4),
    makeSharedMaterial(THREE, "#203b35", {
      metalness: 0.64,
      roughness: 0.32,
    }),
  );
  elevatorCarFloor.position.y = 0.16;
  officeElevatorCar.add(elevatorCarFloor);
  const elevatorCarCeiling = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 9.4, 0.22, 7.4),
    elevatorShaftFrame,
  );
  elevatorCarCeiling.position.y = 7.15;
  officeElevatorCar.add(elevatorCarCeiling);
  for (const [geometry, x, z] of [
    [resources.primitive(THREE, "BoxGeometry", 0.14, 6.8, 7.25), -4.62, 0],
    [resources.primitive(THREE, "BoxGeometry", 0.14, 6.8, 7.25), 4.62, 0],
    [resources.primitive(THREE, "BoxGeometry", 9.25, 6.8, 0.14), 0, 3.55],
  ]) {
    const pane = new THREE.Mesh(geometry, elevatorCarGlass);
    pane.position.set(x, 3.65, z);
    officeElevatorCar.add(pane);
  }
  const officeElevatorDoors = [-1, 1].map((side) => {
    const door = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 4.45, 6.65, 0.12),
      makeSharedMaterial(THREE, "#d8fff6", {
        transparent: true,
        opacity: 0.24,
        metalness: 0,
        roughness: 0.48,
        depthWrite: false,
      }),
    );
    door.position.set(side * 4.36, 3.62, -3.56);
    officeElevatorCar.add(door);
    return door;
  });
  const elevatorCabinLight = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 5.8, 0.08, 0.3),
    officeFloorAccent,
  );
  elevatorCabinLight.position.set(0, 6.92, -1.2);
  officeElevatorCar.add(elevatorCabinLight);
  officeInterior.add(officeElevatorCar);

  const elevatorPanelGeometry = resources.primitive(THREE, "BoxGeometry", 5.4, 5.2, 0.4);
  const elevatorPanelMaterial = makeMaterial(THREE, "#172224", {
    metalness: 0.8,
    roughness: 0.2,
  });
  const elevatorButtonGeometry = resources.primitive(THREE, "BoxGeometry", 1.62, 0.78, 0.28);
  const elevatorButtonMaterials = new Map(
    OFFICE_FLOORS.map((destination) => {
      const texture = canvasTexture(THREE, 256, 144, (context) => {
        context.fillStyle = "#06140f";
        context.fillRect(0, 0, 256, 144);
        context.strokeStyle = "#9ef7c6";
        context.lineWidth = 12;
        context.strokeRect(7, 7, 242, 130);
        context.fillStyle = "#effff8";
        context.textAlign = "center";
        context.textBaseline = "middle";
        context.font =
          '900 72px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(String(destination.level + 1), 128, 48);
        context.fillStyle = "#9ef7c6";
        context.font =
          '800 23px "ForkMesh Mono", ui-monospace, monospace';
        const teamLabel = (
          destination.team ||
          (destination.id === "rooftop" ? "ROOF" : destination.label)
        )
          .replaceAll("-", " ")
          .toUpperCase();
        context.fillText(teamLabel, 128, 107, 226);
      });
      return [
        destination.id,
        resources.uniqueMaterial(THREE, {
          // Fail closed. setOfficeAccess owns the unlocked colours, and it only
          // ever runs from a real office entry, so a panel that started green
          // advertised travel the server had not granted to anyone yet.
          color: "#3b3034",
          map: texture,
          emissive: "#6e2737",
          emissiveIntensity: 0.2,
          metalness: 0.46,
          roughness: 0.35,
        }, "MeshStandardMaterial"),
      ];
    }),
  );
  const elevatorPanel = new THREE.Group();
  elevatorPanel.name = "forkmesh-office-elevator-cabin-panel";
  // Keep the controls on the right-side wall. The upper-corner cabin camera
  // looks diagonally across these large labels and through the exterior-facing
  // opening, so riders can choose a floor without staring back into the Office.
  elevatorPanel.position.set(4.34, 3.3, -0.6);
  elevatorPanel.rotation.y = -Math.PI / 2;
  const panelBody = new THREE.Mesh(
    elevatorPanelGeometry,
    elevatorPanelMaterial,
  );
  elevatorPanel.add(panelBody);
  OFFICE_FLOORS.forEach((destination, index) => {
    const button = new THREE.Mesh(
      elevatorButtonGeometry,
      elevatorButtonMaterials.get(destination.id),
    );
    const column = index % 2;
    const rowFromBottom = Math.floor(index / 2);
    // Conventional lift ordering: floor 1 starts at the lower-left, rises
    // left-to-right, and the rooftop ends at the top.
    button.position.set(
      (column - 0.5) * 2.35,
      -1.7 + rowFromBottom * 0.82,
      0.3,
    );
    button.name = `forkmesh-office-elevator-button-${destination.id}`;
    button.userData.interactive = "office-elevator-floor";
    button.userData.officeFloorAllowed = false;
    button.userData.officeFloorId = destination.id;
    button.userData.officeFloorLevel = destination.level;
    button.userData.officeFloorNumber = destination.level + 1;
    button.userData.officeFloorTeamLabel = (
      destination.team ||
      (destination.id === "rooftop" ? "ROOF" : destination.label)
    )
      .replaceAll("-", " ")
      .toUpperCase();
    interactive.push(button);
    elevatorPanel.add(button);
    officeElevatorButtons.push(button);
  });
  officeElevatorCar.add(elevatorPanel);
  return {
    officeAttendanceBoard,
    officeClockInBoard,
    reflectionTarget,
    reflectionCamera,
    logoEnvironmentMap,
    chrome,
    darkChrome,
    logoPanelChrome,
    chromeCube,
    logoReflectionSettleMs,
    officeElevatorButtons,
    officeElevatorCar,
    officeElevatorDoors,
    get logoReflectionEnvironment() {
      return logoReflectionEnvironment;
    },
    set logoReflectionEnvironment(value) {
      logoReflectionEnvironment = value;
    },
  };
}
