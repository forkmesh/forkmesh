import { droneCourseGates } from "../districts/quadcopters.js";
import {
  advanceRace,
  clearGhostTrack,
  copyGhostTrack,
  createGhostTrack,
  createRaceState,
  formatLapTime,
  ghostTrackRecord,
  loadGhostTrack,
  raceElapsedMs,
  racePresence,
  recordGhostSample,
  remoteRace,
  resetRace,
  sampleGhostTrack,
  validLapMs,
} from "../drone-race.js";
import {
  DRONE_RACE_GUIDE_DISTANCE,
  DRONE_RACE_REMOTE_STALE_MS,
  DRONE_RACE_STORAGE_KEY,
} from "../gameplay-constants.js";
import { quadcopterFlightPitch, quadcopterFlightVector } from "../movement.js";

const COMPASS_WIDTH = 260;
const COMPASS_PX_PER_DEGREE = 2;
const RIVAL_ROWS = 4;
const PASSED_RING = "#27465a";
const NEXT_RING = "#ffe066";
const NEXT_RING_FLASH = "#ffffff";
const ARROW_UPCOMING = "#79c0ff";
const ARROW_NEXT = "#ffd23f";
const ARROW_GUIDE = "#ffb000";
const COMPASS_LABELS = { 0: "N", 45: "NE", 90: "E", 135: "SE", 180: "S", 225: "SW", 270: "W", 315: "NW" };

export function functions({
  THREE,
  labelLayer,
  reducedMotion,
  ctx,
}) {
  const { rings, start } = droneCourseGates();
  const targetCount = rings.length + 1;
  const race = createRaceState(rings.length);
  const recording = createGhostTrack();
  const ghostTrack = createGhostTrack();
  const from = new THREE.Vector3();
  const to = new THREE.Vector3();
  const velocity = new THREE.Vector3();
  const instant = new THREE.Vector3();
  const aim = new THREE.Vector3();
  const projected = new THREE.Vector3();
  const guideTarget = new THREE.Vector3();
  const guideDirection = new THREE.Vector3();
  const guideSide = new THREE.Vector3();
  const guideLift = new THREE.Vector3();
  const worldUp = new THREE.Vector3(0, 1, 0);
  const guideScale = new THREE.Vector3(1.4, 1.4, 1.4);
  const strongScale = new THREE.Vector3(1.6, 1.6, 1.6);
  const hiddenScale = new THREE.Vector3(0, 0, 0);
  const scratchMatrix = new THREE.Matrix4();
  const armMatrix = new THREE.Matrix4();
  const scratchColor = new THREE.Color();
  const upcomingColor = new THREE.Color(ARROW_UPCOMING);
  const nextColor = new THREE.Color(ARROW_NEXT);
  const guideColor = new THREE.Color(ARROW_GUIDE);
  const passedRingColor = new THREE.Color(PASSED_RING);
  const nextRingColor = new THREE.Color(NEXT_RING);
  const nextRingFlash = new THREE.Color(NEXT_RING_FLASH);
  const ghostPose = { x: 0, y: 0, z: 0, yaw: 0 };
  const arrowStates = new Uint8Array(targetCount);
  const shown = {};
  let course = null;
  let active = false;
  let storedLoaded = false;
  let hasFrom = false;
  let lastTarget = -1;
  let lastRingProgress = -1;
  let lastEvent = "";
  let message = "";
  let messageUntil = 0;
  let nextHudTextAt = 0;
  let nextRivalsAt = 0;
  let hud = null;

  function raceCourse() {
    if (!course) course = ctx.world.getObjectByName("forkmesh-drone-race-course") || null;
    return course?.userData.race ? course : null;
  }

  function loadStored() {
    if (storedLoaded) return;
    storedLoaded = true;
    try {
      const record = JSON.parse(
        window.localStorage.getItem(DRONE_RACE_STORAGE_KEY) || "null",
      );
      race.bestMs = validLapMs(record?.bestMs);
      if (race.bestMs) loadGhostTrack(ghostTrack, record.ghost);
    } catch {
      race.bestMs = 0;
      clearGhostTrack(ghostTrack);
    }
  }

  function saveStored() {
    try {
      window.localStorage.setItem(
        DRONE_RACE_STORAGE_KEY,
        JSON.stringify({ bestMs: race.bestMs, ghost: ghostTrackRecord(ghostTrack) }),
      );
    } catch {}
  }

  function buildHud() {
    if (hud || !labelLayer) return hud;
    const root = document.createElement("div");
    root.className = "world-drone-hud";
    root.hidden = true;
    const ticks = [];
    for (let degree = -360; degree < 720; degree += 15) {
      const heading = ((degree % 360) + 360) % 360;
      const label = COMPASS_LABELS[heading];
      const x = (degree + 360) * COMPASS_PX_PER_DEGREE;
      ticks.push(
        label
          ? `<span class="world-drone-hud__tick is-major" style="left:${x}px">${label}</span>`
          : `<span class="world-drone-hud__tick" style="left:${x}px"></span>`,
      );
    }
    root.innerHTML = `
      <div class="world-drone-hud__compass" aria-hidden="true">
        <div class="world-drone-hud__strip">${ticks.join("")}</div>
        <span class="world-drone-hud__caret"></span>
      </div>
      <output class="world-drone-hud__heading" aria-label="Heading">000°</output>
      <span class="world-drone-hud__reticle" aria-hidden="true"></span>
      <span class="world-drone-hud__vector" aria-hidden="true"></span>
      <div class="world-drone-hud__race" role="status" aria-live="off">
        <strong data-drone-rings>Rings 0/${rings.length}</strong>
        <span data-drone-lap>Lap --:--.--</span>
        <span data-drone-best>Best --:--.--</span>
      </div>
      <p class="world-drone-hud__message" data-drone-message role="status" aria-live="polite"></p>
      <ol class="world-drone-hud__rivals" data-drone-rivals hidden aria-label="Other racers"></ol>`;
    labelLayer.append(root);
    const rivals = root.querySelector("[data-drone-rivals]");
    const rivalRows = Array.from({ length: RIVAL_ROWS }, () => {
      const row = document.createElement("li");
      row.hidden = true;
      rivals.append(row);
      return row;
    });
    hud = {
      root,
      strip: root.querySelector(".world-drone-hud__strip"),
      heading: root.querySelector(".world-drone-hud__heading"),
      reticle: root.querySelector(".world-drone-hud__reticle"),
      vector: root.querySelector(".world-drone-hud__vector"),
      rings: root.querySelector("[data-drone-rings]"),
      lap: root.querySelector("[data-drone-lap]"),
      best: root.querySelector("[data-drone-best]"),
      message: root.querySelector("[data-drone-message]"),
      rivals,
      rivalRows,
    };
    return hud;
  }

  function setText(key, element, text) {
    if (shown[key] === text) return;
    shown[key] = text;
    element.textContent = text;
  }

  function setTransform(key, element, text) {
    if (shown[key] === text) return;
    shown[key] = text;
    element.style.transform = text;
  }

  function currentTarget() {
    return race.running && race.nextRing < rings.length ? race.nextRing : rings.length;
  }

  function targetGate(index) {
    return index < rings.length ? rings[index] : start;
  }

  function paintRings(time) {
    const { ringMeshes, ringColors } = course.userData;
    const target = currentTarget();
    const progress = race.running ? race.nextRing : 0;
    const flash = reducedMotion ? 0 : 0.5 + 0.5 * Math.sin(time * 0.012);
    for (const mesh of ringMeshes) {
      if (progress !== lastRingProgress) {
        for (let index = 0; index < rings.length; index += 1) {
          if (index < progress) scratchColor.copy(passedRingColor);
          else scratchColor.set(ringColors[index % ringColors.length]);
          mesh.setColorAt(index, scratchColor);
        }
      }
      if (target < rings.length) {
        scratchColor.copy(nextRingColor).lerp(nextRingFlash, flash);
        mesh.setColorAt(target, scratchColor);
      }
      mesh.instanceColor.needsUpdate = true;
    }
    lastRingProgress = progress;
  }

  function restoreRings() {
    const { ringMeshes, ringColors } = course.userData;
    for (const mesh of ringMeshes) {
      if (mesh.userData.idleMaterial) mesh.material = mesh.userData.idleMaterial;
      for (let index = 0; index < rings.length; index += 1) {
        mesh.setColorAt(index, scratchColor.set(ringColors[index % ringColors.length]));
      }
      mesh.instanceColor.needsUpdate = true;
    }
    lastRingProgress = -1;
  }

  function setChevron(arrows, chevron, matrix) {
    const { armMatrices } = arrows.userData;
    for (let arm = 0; arm < armMatrices.length; arm += 1) {
      armMatrix.multiplyMatrices(matrix, armMatrices[arm]);
      arrows.setMatrixAt(chevron * armMatrices.length + arm, armMatrix);
    }
  }

  function tintChevron(arrows, chevron, color) {
    const arms = arrows.userData.armMatrices.length;
    for (let arm = 0; arm < arms; arm += 1) {
      arrows.setColorAt(chevron * arms + arm, color);
    }
  }

  function layoutArrows(arrows) {
    const target = currentTarget();
    if (target === lastTarget) return;
    lastTarget = target;
    const { baseMatrices, perTarget } = arrows.userData;
    for (let targetIndex = 0; targetIndex < targetCount; targetIndex += 1) {
      const passed = race.running && targetIndex < race.nextRing;
      const state = targetIndex === target ? 2 : passed ? 0 : 1;
      arrowStates[targetIndex] = state;
      for (let step = 0; step < perTarget; step += 1) {
        const chevron = targetIndex * perTarget + step;
        scratchMatrix.copy(baseMatrices[chevron]);
        if (state === 2) scratchMatrix.scale(strongScale);
        if (state === 0) scratchMatrix.scale(hiddenScale);
        setChevron(arrows, chevron, scratchMatrix);
      }
    }
    arrows.instanceMatrix.needsUpdate = true;
  }

  function animateArrows(arrows, time, position) {
    const { perTarget, guideChevron } = arrows.userData;
    for (let targetIndex = 0; targetIndex < targetCount; targetIndex += 1) {
      const state = arrowStates[targetIndex];
      if (state === 0) continue;
      const base = state === 2 ? nextColor : upcomingColor;
      for (let step = 0; step < perTarget; step += 1) {
        const wave = reducedMotion
          ? 1
          : 0.5 + 0.5 * Math.sin(time * 0.007 - step * 1.3);
        scratchColor.copy(base).multiplyScalar(0.45 + 0.55 * wave);
        tintChevron(arrows, targetIndex * perTarget + step, scratchColor);
      }
    }
    const gate = targetGate(currentTarget());
    guideTarget.set(gate.center[0], gate.center[1], gate.center[2]);
    guideDirection.subVectors(guideTarget, position);
    const distance = guideDirection.length();
    if (distance < DRONE_RACE_GUIDE_DISTANCE + 4) {
      scratchMatrix.makeScale(0, 0, 0);
    } else {
      guideDirection.divideScalar(distance);
      guideSide.crossVectors(guideDirection, worldUp);
      if (guideSide.lengthSq() < 1e-6) guideSide.set(1, 0, 0);
      guideSide.normalize();
      guideLift.crossVectors(guideSide, guideDirection).normalize();
      scratchMatrix
        .makeBasis(guideSide, guideDirection, guideLift)
        .scale(guideScale)
        .setPosition(
          position.x + guideDirection.x * DRONE_RACE_GUIDE_DISTANCE,
          position.y + 3.2 + guideDirection.y * DRONE_RACE_GUIDE_DISTANCE,
          position.z + guideDirection.z * DRONE_RACE_GUIDE_DISTANCE,
        );
    }
    setChevron(arrows, guideChevron, scratchMatrix);
    const pulse = reducedMotion ? 1 : 0.75 + 0.25 * Math.sin(time * 0.01);
    tintChevron(arrows, guideChevron, scratchColor.copy(guideColor).multiplyScalar(pulse));
    arrows.instanceMatrix.needsUpdate = true;
    arrows.instanceColor.needsUpdate = true;
  }

  function showMessage(text, time, duration) {
    message = text;
    messageUntil = time + duration;
  }

  function handleEvent(event, time) {
    if (!event) return;
    lastEvent = event;
    if (event === "start" || event === "restart" || event === "finish") {
      if (event === "finish" && race.newBest) {
        copyGhostTrack(ghostTrack, recording);
        saveStored();
      }
      clearGhostTrack(recording);
    }
    if (event === "start") {
      showMessage(
        ghostTrack.count > 1 ? "Lap started · race your ghost" : "Lap started · fly through ring 1",
        time,
        2600,
      );
    } else if (event === "restart") {
      showMessage("Lap restarted · fly through ring 1", time, 2600);
    } else if (event === "finish") {
      showMessage(
        race.newBest
          ? `Lap ${formatLapTime(race.lastLapMs)} · New best!`
          : `Lap ${formatLapTime(race.lastLapMs)} · Best ${formatLapTime(race.bestMs)}`,
        time,
        6000,
      );
    } else if (event === "ring") {
      showMessage(`Ring ${race.nextRing}/${rings.length}`, time, 1200);
    } else if (event === "out-of-order") {
      showMessage(`Out of order · fly through ring ${race.nextRing + 1}`, time, 2000);
    } else if (event === "wrong-way") {
      showMessage("Wrong way", time, 1600);
    }
  }

  function updateGhost(ghost, time) {
    const elapsed = raceElapsedMs(race, time);
    const visible = race.running && sampleGhostTrack(ghostTrack, elapsed, ghostPose);
    ghost.visible = visible;
    if (!visible) return;
    ghost.position.set(ghostPose.x, ghostPose.y, ghostPose.z);
    ghost.rotation.y = ghostPose.yaw;
  }

  function updateRivals(time) {
    if (time < nextRivalsAt) return;
    nextRivalsAt = time + 250;
    let row = 0;
    for (const avatar of ctx.remotePlayers.values()) {
      if (row >= RIVAL_ROWS) break;
      const data = avatar.userData;
      if (!data.race || time - (data.raceAt || 0) > DRONE_RACE_REMOTE_STALE_MS) continue;
      const rival = remoteRace(data.race, rings.length);
      if (!rival) continue;
      const lap = rival.lap ? rival.lap + Math.max(0, time - data.raceAt) : 0;
      const element = hud.rivalRows[row];
      element.hidden = false;
      setText(
        `rival${row}`,
        element,
        `${String(data.name || "Pilot").slice(0, 16)} · Rings ${rival.ring}/${rings.length} · ${lap ? formatLapTime(lap) : "--:--.--"}${rival.best ? ` · Best ${formatLapTime(rival.best)}` : ""}`,
      );
      row += 1;
    }
    for (let index = row; index < RIVAL_ROWS; index += 1) hud.rivalRows[index].hidden = true;
    const hidden = row === 0;
    if (hud.rivals.hidden !== hidden) hud.rivals.hidden = hidden;
  }

  function updateHud(position, delta, time) {
    quadcopterFlightVector(
      aim,
      ctx.cameraYaw,
      quadcopterFlightPitch(ctx.cameraMode, ctx.cameraPitch, ctx.firstPersonPitch),
      1,
      0,
    );
    const heading = ((Math.atan2(aim.x, -aim.z) * 180) / Math.PI + 360) % 360;
    setTransform(
      "strip",
      hud.strip,
      `translate3d(${(COMPASS_WIDTH / 2 - (heading + 360) * COMPASS_PX_PER_DEGREE).toFixed(1)}px,0,0)`,
    );
    setText("heading", hud.heading, `${String(Math.round(heading) % 360).padStart(3, "0")}°`);
    const width = ctx.viewportRect.width;
    const height = ctx.viewportRect.height;
    ctx.camera.updateMatrixWorld();
    projected.copy(aim).multiplyScalar(80).add(position).project(ctx.camera);
    const aimVisible = projected.z < 1 && Math.abs(projected.x) < 1.2 && Math.abs(projected.y) < 1.2;
    setTransform(
      "reticle",
      hud.reticle,
      aimVisible
        ? `translate3d(${((projected.x * 0.5 + 0.5) * width).toFixed(0)}px,${((-projected.y * 0.5 + 0.5) * height).toFixed(0)}px,0)`
        : "translate3d(-999px,-999px,0)",
    );
    if (delta > 0) {
      instant.subVectors(to, from).divideScalar(delta);
      velocity.lerp(instant, 0.2);
    }
    const moving = velocity.lengthSq() > 4;
    projected.copy(velocity).multiplyScalar(0.9).add(position).project(ctx.camera);
    const vectorVisible = moving && projected.z < 1 && Math.abs(projected.x) < 1.2 && Math.abs(projected.y) < 1.2;
    setTransform(
      "vector",
      hud.vector,
      vectorVisible
        ? `translate3d(${((projected.x * 0.5 + 0.5) * width).toFixed(0)}px,${((-projected.y * 0.5 + 0.5) * height).toFixed(0)}px,0)`
        : "translate3d(-999px,-999px,0)",
    );
    if (time < nextHudTextAt) return;
    nextHudTextAt = time + 100;
    setText(
      "rings",
      hud.rings,
      `Rings ${race.running ? race.nextRing : 0}/${rings.length}`,
    );
    setText(
      "lap",
      hud.lap,
      race.running ? `Lap ${formatLapTime(raceElapsedMs(race, time))}` : "Lap --:--.--",
    );
    setText(
      "best",
      hud.best,
      race.bestMs ? `Best ${formatLapTime(race.bestMs)}` : "Best --:--.--",
    );
    const text = time < messageUntil
      ? message
      : race.running
        ? ""
        : "Fly through the checkered START gate";
    setText("message", hud.message, text);
    if (hud.message.hidden !== !text) hud.message.hidden = !text;
    updateRivals(time);
  }

  function activate() {
    active = true;
    loadStored();
    resetRace(race);
    clearGhostTrack(recording);
    hasFrom = false;
    lastTarget = -1;
    lastRingProgress = -1;
    velocity.set(0, 0, 0);
    const { gate, label, arrows } = course.userData.race;
    for (const mesh of course.userData.ringMeshes) {
      if (!mesh.userData.idleMaterial) mesh.userData.idleMaterial = mesh.material;
      mesh.material = course.userData.activeRingMaterial;
    }
    label.visible = true;
    arrows.visible = true;
    gate.visible = true;
    if (buildHud()) hud.root.hidden = false;
  }

  function deactivate() {
    active = false;
    resetRace(race);
    clearGhostTrack(recording);
    const { label, arrows, ghost } = course.userData.race;
    label.visible = false;
    arrows.visible = false;
    ghost.visible = false;
    restoreRings();
    message = "";
    messageUntil = 0;
    if (hud) hud.root.hidden = true;
  }

  function updateDroneRace(delta, time) {
    if (ctx.disposed) return;
    const flying = Boolean(ctx.quadcopterRide) && ctx.officeSceneMode === "town";
    if (!flying && !active) return;
    if (!raceCourse()) return;
    if (!flying) {
      deactivate();
      return;
    }
    if (!active) activate();
    const state = ctx.quadcopterStates[ctx.quadcopterRide.index];
    if (!state) return;
    const craft = state.quadcopter;
    to.set(craft.position.x, craft.position.y + 0.6, craft.position.z);
    if (hasFrom) {
      handleEvent(advanceRace(race, rings, start, from, to, time), time);
    }
    if (race.running) {
      recordGhostSample(
        recording,
        raceElapsedMs(race, time),
        craft.position.x,
        craft.position.y,
        craft.position.z,
        craft.rotation.y,
      );
    }
    const { arrows, ghost, label } = course.userData.race;
    const gateX = to.x - start.center[0];
    const gateY = to.y - start.center[1];
    const gateZ = to.z - start.center[2];
    label.visible = gateX * gateX + gateY * gateY + gateZ * gateZ > 2500;
    paintRings(time);
    layoutArrows(arrows);
    animateArrows(arrows, time, to);
    updateGhost(ghost, time);
    if (hud) updateHud(to, delta, time);
    from.copy(to);
    hasFrom = true;
  }

  function droneRacePresence() {
    if (!active) return null;
    return racePresence(race, performance.now());
  }

  function droneRaceState() {
    const now = performance.now();
    return {
      active,
      running: race.running,
      ring: race.running ? race.nextRing : 0,
      ringCount: rings.length,
      lapMs: Math.round(raceElapsedMs(race, now)),
      lastLapMs: race.lastLapMs,
      bestMs: race.bestMs,
      laps: race.laps,
      ghost: ghostTrack.count,
      lastEvent,
    };
  }

  function droneCourseLayout() {
    return {
      start: { ...start },
      rings: rings.map((ring) => ({ ...ring })),
    };
  }

  return {
    updateDroneRace,
    droneRacePresence,
    droneRaceState,
    droneCourseLayout,
  };
}
