import { hashNumber } from "../../world-avatar-face.js";
import { BADGE_DETAIL_SAMPLE_MS } from "../avatar-badges.js";
import { updatePlayerScreenLabel, updateScreenLabel } from "../labels.js";
import { clamp } from "../object-utils.js";
import {
  AVATAR_HIGHLIGHT_SAMPLE_MS,
  MOVEMENT_INPUT_DELAY_THRESHOLD_MS,
  MOVEMENT_INPUT_LOG_COOLDOWN_MS,
  PREVIEW_RENDER_FRAME_INTERVAL_MS,
  REMOTE_LABEL_MAX_DISTANCE_SQ,
  RENDER_FRAME_EARLY_TOLERANCE_MS,
  RENDER_FRAME_INTERVAL_MS,
  RENDER_STALL_LOG_COOLDOWN_MS,
  RENDER_STALL_THRESHOLD_MS,
  SCENE_LOD_SAMPLE_MS,
  SHADOW_MAP_BUSY_RETRY_MS,
  SHADOW_MAP_DEFERRED_RETRY_MS,
  SHADOW_MAP_STALL_COOLDOWN_MS,
  SHADOW_MAP_UPDATE_MS,
  VISUAL_ANIMATION_FAR_MS,
  WORLD_RENDER_WORKLOAD_BUDGET,
} from "../quality.js";
import { animateWeather } from "../weather.js";

export function functions({
  THREE,
  reducedMotion,
  ctx,
  fns,
}) {
  // Measured display refresh interval (every animation callback, drawn or
  // not). The cadence gate accepts a callback up to half of it early, so the
  // loop draws on the callback nearest each 60Hz deadline instead of skipping
  // one that arrived a hair early.
  let lastAnimationCallbackAt = 0;
  let lastRenderStallLogAt = -Infinity;
  let suppressedRenderStalls = 0;
  let lastMovementInputDelayLogAt = -Infinity;
  let suppressedMovementInputDelays = 0;
  let nextEnvironmentCheckAt = 0;
  let nextSceneLodAt = 0;
  let nextShadowMapUpdateAt = 0;
  let nextBadgeDetailAt = 0;
  let nextProximityUpdateAt = 0;
  let nextScreenLabelUpdateAt = 0;


  function scheduleMovementInputWarning(inputToFrameMs) {
    if (ctx.movementInputWarningTimer || ctx.disposed) return;
    const snapshot = {
      inputToFrameMs: Math.round(inputToFrameMs),
      space: ctx.currentSpace,
      scene: ctx.officeSceneMode,
      suppressed: suppressedMovementInputDelays,
    };
    suppressedMovementInputDelays = 0;
    ctx.movementInputWarningTimer = window.setTimeout(() => {
      ctx.movementInputWarningTimer = 0;
      if (ctx.disposed) return;
      console.warn("[ForkMesh World] Movement input delayed", {
        observedAt: new Date().toISOString(),
        ...snapshot,
      });
    }, 0);
  }

  function scheduleRenderStallWarning(rawFrameMs, refreshedShadowMap = false) {
    if (ctx.renderStallWarningTimer || ctx.disposed) return;
    const renderCalls = Number(ctx.renderer.info?.render?.calls) || 0;
    const renderedTriangles = Number(ctx.renderer.info?.render?.triangles) || 0;
    let component = `${ctx.currentSpace} scene`;
    let codeArea = "world-scene animate() update/render pipeline";
    if (refreshedShadowMap) {
      component = "shadow map refresh";
      codeArea = "renderer.render(scene, camera) shadow pass";
    } else if (ctx.pinchActive || ctx.primaryPointerId !== null) {
      component = "camera controls";
      codeArea = "world-scene pointer/pinch camera update";
    } else if (
      renderCalls >= WORLD_RENDER_WORKLOAD_BUDGET.drawCalls ||
      renderedTriangles >= WORLD_RENDER_WORKLOAD_BUDGET.triangles
    ) {
      component = "Three.js renderer workload";
      codeArea = "renderer.render(scene, camera)";
    } else if (ctx.officeSceneMode !== "town") {
      component = `office ${ctx.officeSceneMode} scene`;
      codeArea = "world-scene office movement and environment updates";
    }
    const snapshot = {
      frameMs: Math.round(rawFrameMs),
      estimatedMissedFrames: Math.max(
        0,
        Math.round(rawFrameMs / 16.67) - 1,
      ),
      likelyCause: { component, codeArea },
      space: ctx.currentSpace,
      scene: ctx.officeSceneMode,
      renderCalls,
      renderedTriangles,
      interactiveObjects: ctx.interactive.length,
      animatedObjects: ctx.animated.length,
      suppressed: suppressedRenderStalls,
    };
    suppressedRenderStalls = 0;
    ctx.renderStallWarningTimer = window.setTimeout(() => {
      ctx.renderStallWarningTimer = 0;
      if (ctx.disposed) return;
      const drawingBuffer = ctx.renderer.getDrawingBufferSize(
        ctx.diagnosticsDrawingBuffer,
      );
      console.warn(
        `[ForkMesh World] Render stall detected; we think it was ${component} (${codeArea})`,
        {
          observedAt: new Date().toISOString(),
          ...snapshot,
          drawingBuffer: [drawingBuffer.x, drawingBuffer.y],
          pixelRatio: ctx.renderer.getPixelRatio(),
        },
      );
    }, 0);
  }

  function animateRepositoryPortalPulse(group, time) {
    const halo = group.userData.repositoryHalo;
    const marker = group.userData.repositoryOrbitMarker;
    if (halo?.userData?.repositoryHaloScale) {
      const pulse =
        halo.userData.repositoryHaloScale *
        (1 + Math.sin(time * 0.0032) * 0.055);
      halo.scale.setScalar(pulse);
    }
    if (marker?.userData?.repositoryOrbitRadius) {
      const angle = time * 0.0024;
      const radius = marker.userData.repositoryOrbitRadius;
      marker.position.x = Math.cos(angle) * radius;
      marker.position.y = Math.sin(angle) * radius;
    }
  }

  function animateRepositoryPortalBirth(group, key, time) {
    const bornAt = ctx.repositoryPortalBornAt.get(key);
    if (!bornAt) return;
    const progress = clamp((time - bornAt) / 1050, 0, 1);
    if (progress <= 0) {
      group.scale.setScalar(0.015);
      return;
    }
    // Overshoot once, then settle into the ring like a portal locking
    // onto its perimeter coordinate.
    const back = 1.70158;
    const shifted = progress - 1;
    const scale =
      1 + (back + 1) * shifted ** 3 + back * shifted ** 2;
    group.scale.setScalar(Math.max(0.015, scale));
    group.rotation.z =
      Math.sin(progress * Math.PI) * (1 - progress) * 0.24;
    if (progress >= 1) {
      group.scale.setScalar(1);
      group.rotation.z = 0;
      ctx.repositoryPortalBornAt.delete(key);
    }
  }

  // Motes circling a portal whose size tree is still being assembled. They
  // fade in with the portal, twinkle while the map builds, and fade out on
  // their own if the map never arrives, so a stalled mirror is never left
  // wearing a permanent celebration.
  function animateRepositoryPortalSparkle(group, key, time) {
    const sparkle = group.userData.repositorySparkle;
    if (!sparkle) return;
    const seeds = sparkle.userData.repositorySparkleSeeds;
    const until = sparkle.userData.repositorySparkleUntil || 0;
    const remaining = until - time;
    if (remaining <= 0 || !seeds) {
      sparkle.parent?.remove(sparkle);
      sparkle.geometry.dispose();
      sparkle.material.dispose();
      group.userData.repositorySparkle = null;
      ctx.repositorySparkleUntil.delete(key);
      return;
    }
    sparkle.visible = true;
    const born = ctx.repositoryPortalBornAt.get(key);
    const rising = born ? clamp((time - born) / 1050, 0, 1) : 1;
    // Fade out over the last second so the shimmer never cuts off.
    sparkle.material.opacity =
      0.9 * rising * clamp(remaining / 1000, 0, 1);
    const positions = sparkle.geometry.attributes.position;
    const array = positions.array;
    for (let index = 0; index < array.length; index += 3) {
      const radius = seeds[index];
      const phase = seeds[index + 1];
      const speed = seeds[index + 2];
      const angle = phase + time * 0.0011 * speed;
      // Breathe the orbit so the motes look like they are still settling
      // onto a map that has not finished resolving.
      const breath = 1 + Math.sin(time * 0.0021 + phase) * 0.16;
      array[index] = Math.cos(angle) * radius * breath;
      array[index + 1] = Math.sin(angle) * radius * breath;
      array[index + 2] = Math.sin(time * 0.0035 + phase) * 0.12;
    }
    positions.needsUpdate = true;
  }

  function animate(time) {
    if (!ctx.running || ctx.disposed) return;
    const callbackGap = time - lastAnimationCallbackAt;
    lastAnimationCallbackAt = time;
    if (callbackGap > 0 && callbackGap < 100) {
      ctx.displayFrameIntervalMs += (callbackGap - ctx.displayFrameIntervalMs) * 0.1;
    }
    // A fixed sub-millisecond allowance dropped every vsync that landed a
    // little early — a 33ms hitch on an otherwise smooth 60Hz panel — and
    // held a 144Hz panel near 48fps with uneven pacing.
    const earlyToleranceMs = Math.max(
      RENDER_FRAME_EARLY_TOLERANCE_MS,
      Math.min(ctx.displayFrameIntervalMs, ctx.renderFrameIntervalMs) * 0.5,
    );
    if (
      ctx.nextRenderFrameAt > 0 &&
      time + earlyToleranceMs < ctx.nextRenderFrameAt
    ) {
      return;
    }
    ctx.nextRenderFrameAt =
      ctx.nextRenderFrameAt <= 0 ||
      time - ctx.nextRenderFrameAt > ctx.renderFrameIntervalMs * 2
        ? time + ctx.renderFrameIntervalMs
        : ctx.nextRenderFrameAt + ctx.renderFrameIntervalMs;
    const frameWorkStartedAt = performance.now();
    ctx.worldProfiler.beginFrame();
    ctx.renderStalls.beginFrame();
    if (time >= nextEnvironmentCheckAt) {
      nextEnvironmentCheckAt = time + 15_000;
      const localNow = new Date();
      const minuteOfDay =
        ctx.daylightMode === "day"
          ? 12 * 60
          : ctx.daylightMode === "night"
            ? 0
            : localNow.getHours() * 60 + localNow.getMinutes();
      if (minuteOfDay !== ctx.localDaylightMinute) fns.updateWorldEnvironment();
    }
    if (time >= nextSceneLodAt) {
      nextSceneLodAt = time + SCENE_LOD_SAMPLE_MS;
      fns.updateSceneLevelOfDetail();
    }
    if (
      ctx.activeAvatarSelection &&
      time >= ctx.nextAvatarHighlightAt
    ) {
      ctx.nextAvatarHighlightAt = time + AVATAR_HIGHLIGHT_SAMPLE_MS;
      fns.refreshAvatarSelectionHighlight();
    }
    if (time >= nextBadgeDetailAt) {
      nextBadgeDetailAt = time + BADGE_DETAIL_SAMPLE_MS;
      fns.updateAvatarBadgeDetail();
    }
    const rawFrameMs = Math.max(0, time - ctx.lastFrame);
    const delta = clamp(rawFrameMs / 1000, 0, 0.05);
    ctx.lastFrame = time;
    fns.updateEngineeringDebugPanel(time, delta);
    if (ctx.movementInputStartedAt > 0) {
      const inputToFrameMs = Math.max(0, time - ctx.movementInputStartedAt);
      ctx.diagnosticsLastMovementInputMs = inputToFrameMs;
      ctx.diagnosticsWorstMovementInputMs = Math.max(
        ctx.diagnosticsWorstMovementInputMs,
        inputToFrameMs,
      );
      if (
        inputToFrameMs >= MOVEMENT_INPUT_DELAY_THRESHOLD_MS &&
        time - lastMovementInputDelayLogAt >=
          MOVEMENT_INPUT_LOG_COOLDOWN_MS
      ) {
        lastMovementInputDelayLogAt = time;
        scheduleMovementInputWarning(inputToFrameMs);
      } else if (inputToFrameMs >= MOVEMENT_INPUT_DELAY_THRESHOLD_MS) {
        suppressedMovementInputDelays += 1;
      }
      ctx.movementInputStartedAt = 0;
    }
    ctx.diagnosticsLongestFrameMs = Math.max(ctx.diagnosticsLongestFrameMs, rawFrameMs);
    ctx.diagnosticsWorstFrameSinceLoadMs = Math.max(
      ctx.diagnosticsWorstFrameSinceLoadMs,
      rawFrameMs,
    );
    if (rawFrameMs > 34) ctx.diagnosticsLongFrames += 1;
    // Per-frame duration samples for the p95 reading. When no debug surface
    // is polling getDiagnostics the buffer is recycled in place instead of
    // growing without bound.
    if (ctx.diagnosticsFrameTimes.length >= 600) ctx.diagnosticsFrameTimes.length = 0;
    ctx.diagnosticsFrameTimes.push(rawFrameMs);
    // rawFrameMs includes time spent waiting for requestAnimationFrame and is
    // useful for FPS diagnostics, but it cannot identify renderer work. Stall
    // attribution is performed after render from actual main-thread work time.
    ctx.worldProfiler.mark("housekeeping");
    if (!reducedMotion && fns.worldElementEnabled("animations")) {
      ctx.worldProfiler.runCallbacks(ctx.frameAnimated, time, delta);
    }
    ctx.worldProfiler.mark("frame-animations");
    fns.updateOfficeElevator(time);
    // District rides (lifts, hyperloops) move the avatar themselves; the walk
    // step yields while any of them reports a ride in progress. This runs
    // every frame regardless of the animations switch so a ride always ends.
    let districtRiding = false;
    for (let index = 0; index < ctx.districtPackages.length; index += 1) {
      const record = ctx.districtPackages[index];
      record.frame.time = time;
      record.frame.delta = delta;
      const state = record.instance.update?.(record.frame);
      if (state?.riding) districtRiding = true;
    }
    if (ctx.officeSceneMode === "town" && !districtRiding) {
      fns.walkPlayer(delta, time);
    } else if (ctx.officeSceneMode === "lobby") {
      fns.walkOfficeLobbyPlayer(delta, time);
    } else if (ctx.officeSceneMode === "meeting") {
      fns.walkOfficeParticipant(delta, time);
    }
    fns.updateRepositoryRingExpansion(delta);
    fns.syncEnclosureSceneVisibility();
    fns.updateOfficeReceptionGuide(time);
    fns.updateOfficeAttendanceClock(time);
    const aquariumAnimationActive =
      !reducedMotion &&
      ctx.officeSceneMode === "lobby" &&
      ctx.officeCurrentFloorId === "lobby";
    if (ctx.officeAquarium) {
      ctx.officeAquarium.setFishBodiesVisible(ctx.officeSceneMode !== "town");
      ctx.officeAquarium.setAnimationActive(aquariumAnimationActive);
      ctx.officeAquarium.updateFeeding(time, aquariumAnimationActive);
    }
    ctx.worldProfiler.mark("simulation");
    // Presence and bots remain part of one live simulation while a remote
    // Office is active. The visibility sweep draws same-parcel visitors and a
    // mainland proxy, but hidden populations still advance so returning to
    // Town never reveals frozen state. Elements toggles retain exact control
    // over the cost of each population.
    if (fns.worldElementEnabled("remote-avatars")) fns.updateRemotePlayers(delta, time);
    if (fns.worldElementEnabled("forkbot")) fns.updateForkbot(delta, time);
    if (fns.worldElementEnabled("agent-npcs")) {
      fns.updateAgentBots(delta, time);
      fns.updateDesktopAgentBotMotion(delta, time);
    }
    ctx.worldProfiler.mark("avatars");
    const repositoryLoadingTail = ctx.world.userData.repositorySizeLoadingTail;
    if (repositoryLoadingTail?.visible) {
      repositoryLoadingTail.rotation.z = time * 0.008;
      const spark = repositoryLoadingTail.children.at(-1);
      if (spark?.material?.emissive) {
        spark.material.emissiveIntensity = 1.45 + Math.sin(time * 0.02) * 0.75;
      }
    }
    if (!reducedMotion) {
      for (const { group, key } of ctx.repositoryPortals.values()) {
        animateRepositoryPortalBirth(group, key, time);
        if (fns.worldElementEnabled("repository-portals")) {
          animateRepositoryPortalPulse(group, time);
          animateRepositoryPortalSparkle(group, key, time);
        }
      }
      // Node beacons hold a steady colour and size — no pulse — so a status
      // reads the same in a screenshot as it does live. Only degraded and
      // healing nodes carry a sweep, and it turns rather than fades, so the
      // colour itself stays legible in a still frame.
      fns.updateNodeMachines(time);
      if (fns.worldElementEnabled("node-cabinets")) {
        ctx.nodeInfrastructure.forEach((cabinet) => {
          const sweep = cabinet.userData?.beaconSweep;
          if (sweep) sweep.rotation.y = time * 0.0038;
          const actionPulse = cabinet.userData?.actionPulseMaterial;
          if (actionPulse) {
            const base = cabinet.userData?.actionPulseAttention ? 0.62 : 0.44;
            actionPulse.opacity = base + (Math.sin(time * 0.0065) + 1) * 0.16;
          }
        });
      }
      if (fns.worldElementEnabled("directory-bots")) {
        ctx.botAgents.forEach((robot, id) => {
          const phase =
            robot.userData.hoverPhase ??
            (robot.userData.hoverPhase = hashNumber(id) * 0.0001);
          robot.position.y =
            0.38 + Math.sin(time * 0.0017 + phase) * 0.16;
          robot.userData.core.rotation.y = time * 0.0011 + phase;
        });
      }
    }
    for (let index = ctx.emoteSprites.length - 1; index >= 0; index -= 1) {
      const flight = ctx.emoteSprites[index];
      const progress = Math.min(
        1,
        (time - flight.startedAt) / flight.duration,
      );
      const fadeStart = flight.fadeStart ?? 0.65;
      // Most speakers are direct World children, while Noah is nested inside
      // the translated Office reception group. World coordinates keep the
      // shared talk-bubble UX attached correctly in both cases.
      flight.avatar.getWorldPosition(flight.sprite.position);
      flight.sprite.position.y +=
        (flight.baseHeight ?? 4.7) + progress * (flight.rise ?? 1.1);
      flight.sprite.material.opacity =
        1 - Math.max(0, progress - fadeStart) / (1 - fadeStart);
      if (progress >= 1) {
        ctx.world.remove(flight.sprite);
        flight.sprite.material?.map?.dispose?.();
        flight.sprite.material?.dispose?.();
        ctx.emoteSprites.splice(index, 1);
      }
    }
    fns.updateAvatarVoice?.(delta, time);
    for (let index = ctx.rewardFlights.length - 1; index >= 0; index -= 1) {
      const flight = ctx.rewardFlights[index];
      const elapsed = time - flight.startedAt;
      const complete = elapsed / flight.duration;
      flight.particles.forEach((particle, particleIndex) => {
        const stagger = particleIndex * 0.018;
        const progress = clamp((complete - stagger) / (1 - stagger), 0, 1);
        particle.position.lerpVectors(flight.start, flight.target, progress);
        particle.position.y +=
          Math.sin(progress * Math.PI) * (3.2 + (particleIndex % 4) * 0.18);
        particle.position.x += Math.sin(particleIndex * 2.1) * 0.18;
        particle.position.z += Math.cos(particleIndex * 1.7) * 0.18;
        particle.rotation.y = progress * Math.PI * 8 + particleIndex;
      });
      if (complete >= 1.1) {
        ctx.world.remove(flight.group);
        flight.group.traverse((child) => {
          child.geometry?.dispose?.();
          child.material?.dispose?.();
        });
        ctx.rewardFlights.splice(index, 1);
      }
    }
    for (
      let index = ctx.repositoryCodeLandings.length - 1;
      index >= 0;
      index -= 1
    ) {
      const effect = ctx.repositoryCodeLandings[index];
      const elapsed = time - effect.startedAt;
      const landedFor = Math.max(0, elapsed - effect.flightDuration);
      const fade =
        landedFor <= effect.sizzleDuration - 2_000
          ? 1
          : clamp(
              (effect.sizzleDuration - landedFor) / 2_000,
              0,
              1,
            );
      effect.fileTiles.forEach((state, fileIndex) => {
        const tileElapsed = elapsed - state.delay;
        const flightProgress = clamp(
          tileElapsed / effect.flightDuration,
          0,
          1,
        );
        const easedFlight = 1 - (1 - flightProgress) ** 3;
        const settle = clamp(
          (tileElapsed - effect.flightDuration) / effect.settleDuration,
          0,
          1,
        );
        const easedSettle = 1 - (1 - settle) ** 3;
        const springSettle =
          1 + 2.70158 * (settle - 1) ** 3 + 1.70158 * (settle - 1) ** 2;
        if (flightProgress < 1) {
          state.tile.position.lerpVectors(
            state.start,
            state.impact,
            easedFlight,
          );
          state.tile.position.y +=
            Math.sin(flightProgress * Math.PI) *
            (8 + fileIndex * 0.7);
          state.tile.scale.setScalar(
            effect.tileScale * (1 - easedFlight * 0.6),
          );
        } else {
          state.tile.position.lerpVectors(
            state.impact,
            state.target,
            easedSettle,
          );
          if (settle >= 1 && !reducedMotion) {
            state.tile.position.x +=
              Math.sin(time * 0.0022 + state.phase) * 0.035;
            state.tile.position.y +=
              Math.cos(time * 0.0019 + state.phase) * 0.03;
          }
          state.tile.scale.setScalar(
            effect.tileScale * (0.4 + 0.6 * springSettle),
          );
        }
        state.tile.quaternion.copy(ctx.camera.quaternion);
        state.tile.material.opacity = fade;
        if (state.leader) {
          const leaderSpan = state.leaderSpan.subVectors(
            state.tile.position,
            state.impact,
          );
          const leaderLength = leaderSpan.length();
          state.leader.visible = settle > 0 && leaderLength > 0.001;
          if (state.leader.visible) {
            state.leader.position
              .copy(state.impact)
              .addScaledVector(leaderSpan, 0.5);
            state.leader.quaternion.setFromUnitVectors(
              state.leaderAxis,
              leaderSpan.divideScalar(leaderLength),
            );
            state.leader.scale.set(1, leaderLength, 1);
            state.leader.material.opacity = fade * 0.9 * easedSettle;
          }
        }
        const shadow = effect.shadows[fileIndex];
        if (shadow?.visible) {
          shadow.position.x =
            state.start.x +
            (state.impact.x - state.start.x) * easedFlight;
          shadow.position.z =
            state.start.z +
            (state.impact.z - state.start.z) * easedFlight;
          shadow.material.opacity = 0.34 * (1 - flightProgress);
          shadow.scale.setScalar(0.75 + flightProgress * 0.4);
        }
      });
      effect.highlights.forEach((highlight) => {
        const litFor = elapsed - effect.flightDuration - highlight.delay;
        if (litFor <= 0) {
          highlight.glow.material.opacity = 0;
          highlight.rim.material.opacity = 0;
          return;
        }
        const flash = Math.max(0, 1 - litFor / 900);
        const breathe = reducedMotion
          ? 0.24
          : 0.22 + Math.sin(litFor * 0.0045) * 0.09;
        highlight.glow.material.opacity =
          fade * Math.min(1, breathe + flash * 0.45);
        const ripple = reducedMotion ? 0 : (litFor % 1600) / 1600;
        highlight.rim.scale.set(1 + ripple * 0.16, 1 + ripple * 0.16, 1);
        highlight.rim.material.opacity =
          fade * (1 - ripple) * (0.55 + flash * 0.45);
      });
      effect.collisionRing.quaternion.copy(ctx.camera.quaternion);
      if (landedFor > 0) {
        const collisionPulse = clamp(landedFor / 620, 0, 1);
        effect.collisionRing.scale.setScalar(1 + collisionPulse * 2.8);
        effect.collisionRing.material.opacity =
          (1 - collisionPulse) * 0.94;
      }
      if (effect.lightning) {
        const lightningProgress = clamp(landedFor / 760, 0, 1);
        effect.lightning.userData.material.opacity =
          landedFor > 0
            ? (1 - lightningProgress) *
              (0.62 + Math.abs(Math.sin(time * 0.071)) * 0.38)
            : 0;
      }
      const sparkleAttribute =
        effect.sparkles.geometry.getAttribute("position");
      if (landedFor > 0 && sparkleAttribute) {
        const values = sparkleAttribute.array;
        for (
          let sparkleIndex = 0;
          sparkleIndex < values.length / 3;
          sparkleIndex += 1
        ) {
          const offset = sparkleIndex * 3;
          const pulse =
            1 +
            Math.sin(time * 0.015 + sparkleIndex * 2.17) * 0.22;
          values[offset] = effect.sparkleSeeds[offset] * pulse;
          values[offset + 1] =
            effect.sparkleSeeds[offset + 1] +
            Math.sin(time * 0.024 + sparkleIndex) * 0.22;
          values[offset + 2] =
            effect.sparkleSeeds[offset + 2] * pulse;
        }
        sparkleAttribute.needsUpdate = true;
        effect.sparkles.material.opacity =
          fade * (0.45 + Math.abs(Math.sin(time * 0.028)) * 0.55);
      }
      if (landedFor >= effect.sizzleDuration) {
        ctx.world.remove(effect.group);
        effect.group.traverse((child) => {
          child.material?.map?.dispose?.();
          child.geometry?.dispose?.();
          child.material?.dispose?.();
        });
        ctx.repositoryCodeLandings.splice(index, 1);
      }
    }
    for (let index = ctx.pushSurges.length - 1; index >= 0; index -= 1) {
      const surge = ctx.pushSurges[index];
      const progress = Math.min(
        1,
        (time - surge.startedAt) / surge.duration,
      );
      const fade = 1 - progress;
      // The ring races out to roughly yard scale while the column burns down.
      surge.ring.scale.setScalar(1 + progress * 34);
      surge.ring.material.opacity = 0.8 * fade;
      surge.beam.scale.x = 1 + progress * 0.7;
      surge.beam.scale.z = surge.beam.scale.x;
      surge.beam.material.opacity = 0.85 * fade * fade;
      if (progress >= 1) {
        ctx.world.remove(surge.group);
        surge.group.traverse((child) => {
          child.geometry?.dispose?.();
          child.material?.dispose?.();
        });
        ctx.pushSurges.splice(index, 1);
      }
    }
    for (let index = ctx.serveFlights.length - 1; index >= 0; index -= 1) {
      const flight = ctx.serveFlights[index];
      const elapsed = time - flight.startedAt;
      // Staggered launches wait on the pad, hidden, until their turn.
      if (elapsed < 0) {
        flight.group.visible = false;
        continue;
      }
      flight.group.visible = true;
      const progress = Math.min(1, elapsed / flight.duration);
      // Hard off the cabinet, easing out as it climbs away.
      const eased = 1 - (1 - progress) ** 2.4;
      flight.group.position.y = flight.baseY + eased * flight.climb;
      flight.group.rotation.y += flight.spin * delta;
      flight.group.scale.setScalar(1 - eased * 0.6);
      const fade = progress < 0.55 ? 1 : 1 - (progress - 0.55) / 0.45;
      flight.materials.forEach((material) => {
        material.opacity = Math.max(0, fade);
      });
      if (progress >= 1) {
        ctx.world.remove(flight.group);
        flight.group.traverse((child) => {
          child.geometry?.dispose?.();
          child.material?.dispose?.();
        });
        ctx.serveFlights.splice(index, 1);
      }
    }
    ctx.worldProfiler.mark("effects");
    fns.updateCamera(delta);
    fns.updateDroneRace(delta, time);
    if (fns.worldElementEnabled("sky")) {
      ctx.worldSky.tick(
        Date.now(),
        ctx.camera.position,
        ctx.camera.getWorldDirection(ctx.skyViewDirection),
      );
    }
    // The sky rides the camera; the weather field rides the avatar. Both have
    // to move with the visitor for a personal environment preset to be visible
    // anywhere other than the middle of the map.
    if (fns.worldElementEnabled("weather")) fns.syncWeatherField();
    fns.tickStoreElements(time);
    ctx.worldProfiler.mark("camera");
    // Spatial scans and DOM-adjacent controls do not need monitor refresh
    // cadence. Bounding them to 12.5Hz removes repeated portal walks and
    // layout writes while movement and WebGL rendering remain full-rate.
    if (time >= nextProximityUpdateAt) {
      nextProximityUpdateAt = time + 80;
      fns.updateBillboardPads();
      if (ctx.officeSceneMode === "town") {
        fns.nearestLandmark();
        fns.updateRepositoryPortalLabels();
        fns.updateRepositoryRecordBoardProximity();
      } else {
        ctx.repositoryRecordBoardNearby = false;
      }
      fns.updateOfficeAquariumProximity(time, ctx.viewportRect);
    }
    fns.updateOfficeSlidingDoors(time, delta);
    ctx.worldProfiler.mark("proximity");
    fns.updateOfficeLogoReflection(time);
    ctx.worldProfiler.mark("mirror");
    if (!reducedMotion && time >= ctx.nextVisualAnimationAt) {
      const visualFrameMs = ctx.farSceneDetail
        ? VISUAL_ANIMATION_FAR_MS
        : ctx.qualityProfile.visualAnimationMs;
      const visualDelta = clamp(
        (time - ctx.lastVisualAnimationAt) / 1000,
        0,
        0.05,
      );
      ctx.lastVisualAnimationAt = time;
      // Preserve cadence instead of drifting by a display frame whenever a
      // requestAnimationFrame timestamp lands just past the deadline.
      ctx.nextVisualAnimationAt =
        ctx.nextVisualAnimationAt <= 0 ||
        time - ctx.nextVisualAnimationAt > visualFrameMs * 2
          ? time + visualFrameMs
          : ctx.nextVisualAnimationAt + visualFrameMs;
      if (fns.worldElementEnabled("animations")) {
        ctx.worldProfiler.runCallbacks(ctx.animated, time, visualDelta);
      }
      if (ctx.weather && fns.worldElementEnabled("weather")) {
        animateWeather(ctx.weather.rain, time, visualDelta, "rain", ctx.weather.anchor);
        animateWeather(ctx.weather.snow, time, visualDelta, "snow", ctx.weather.anchor);
      }
    }
    // resize() owns the only layout read. Reading the canvas bounds here,
    // after label style writes from the preceding frame, forced a synchronous
    // layout on every animation tick and was especially visible on first input.
    ctx.worldProfiler.mark("ambient");
    const rect = ctx.viewportRect;
    // main removed the floating landmark labels (adhoc #243); only the player
    // and remote name plates remain, and they are a town-scene concern.
    if (fns.worldElementEnabled("screen-labels") && time >= nextScreenLabelUpdateAt) {
      nextScreenLabelUpdateAt = time + 34;
      if (ctx.officeSceneMode === "town") {
        if (
          ctx.cameraMode === "first-person" ||
          !fns.worldElementEnabled("player-avatar")
        ) {
          ctx.playerLabel.style.opacity = "0";
          ctx.playerLabel.style.visibility = "hidden";
        } else {
          updatePlayerScreenLabel(
            ctx.player,
            ctx.playerLabel,
            ctx.camera,
            rect.width,
            rect.height,
            ctx.player.userData.emojiStatusSprite ? 5.7 : 4.5,
            ctx.screenLabelPosition,
          );
        }
        if (fns.worldElementEnabled("remote-avatars")) {
          ctx.player.getWorldPosition(ctx.screenLabelOrigin);
          ctx.remotePlayers.forEach((avatar, id) => {
            updatePlayerScreenLabel(
              avatar,
              ctx.remoteLabels.get(id),
              ctx.camera,
              rect.width,
              rect.height,
              avatar.userData.emojiStatusSprite ? 5.35 : 4.2,
              ctx.screenLabelPosition,
              ctx.screenLabelOrigin,
              REMOTE_LABEL_MAX_DISTANCE_SQ,
            );
          });
        } else {
          ctx.remoteLabels.forEach((element) => {
            if (element.style.visibility !== "hidden") {
              element.style.visibility = "hidden";
            }
          });
        }
        ctx.officeParticipantLabels.forEach((element) => {
          if (element.style.visibility !== "hidden") {
            element.style.visibility = "hidden";
          }
        });
        ctx.officeBubbles.forEach((element) => {
          if (element.style.visibility !== "hidden") {
            element.style.visibility = "hidden";
          }
        });
      } else {
        ctx.playerLabel.style.visibility = "hidden";
        ctx.remoteLabels.forEach((element) => {
          if (element.style.visibility !== "hidden") {
            element.style.visibility = "hidden";
          }
        });
        ctx.officeParticipants.forEach((avatar, id) => {
          const participantFloorId =
            String(avatar.userData?.officeFloorId || "marketing");
          const sameVisibleFloor =
            !ctx.officeElevatorRide &&
            participantFloorId === ctx.officeCurrentFloorId;
          const label = ctx.officeParticipantLabels.get(id);
          const bubble = ctx.officeBubbles.get(id);
          if (!sameVisibleFloor) {
            if (label) {
              label.style.opacity = "0";
              label.style.visibility = "hidden";
            }
            if (bubble) {
              bubble.style.opacity = "0";
              bubble.style.visibility = "hidden";
            }
            return;
          }
          updateScreenLabel(
            THREE,
            avatar,
            label,
            ctx.camera,
            rect.width,
            rect.height,
            4.2,
            ctx.screenLabelPosition,
          );
          if (bubble) {
            updateScreenLabel(
              THREE,
              avatar,
              bubble,
              ctx.camera,
              rect.width,
              rect.height,
              5.1,
              ctx.screenLabelPosition,
            );
            const bubbleHalfWidth = Math.min(
              136,
              Math.max(84, rect.width / 2 - 12),
            );
            const bubbleLeft =
              Number.parseFloat(bubble.style.left) || rect.width / 2;
            const bubbleTop = Number.parseFloat(bubble.style.top) || 72;
            bubble.style.left = `${clamp(
              bubbleLeft,
              bubbleHalfWidth,
              rect.width - bubbleHalfWidth,
            )}px`;
            bubble.style.top = `${clamp(
              bubbleTop,
              68,
              rect.height - 18,
            )}px`;
          }
        });
      }
    }
    ctx.worldProfiler.mark("labels");
    let refreshedShadowMap = false;
    if (
      ctx.renderer.shadowMap.enabled &&
      ctx.sun.shadow.intensity > 0 &&
      time >= nextShadowMapUpdateAt
    ) {
      const shadowRefreshBusy =
        ctx.wasWalking ||
        ctx.officeWasMoving ||
        ctx.officeLobbyPlayerMoving ||
        ctx.primaryPointerId !== null ||
        ctx.pinchActive;
      if (shadowRefreshBusy) {
        // A full shadow pass is the largest recurring GPU spike in this
        // scene. Let input settle before paying it so walking and camera
        // gestures never absorb the hitch.
        nextShadowMapUpdateAt = time + SHADOW_MAP_BUSY_RETRY_MS;
      } else {
        nextShadowMapUpdateAt = time + SHADOW_MAP_UPDATE_MS;
        ctx.renderer.shadowMap.needsUpdate = true;
        refreshedShadowMap = true;
      }
    }
    const deferredShadowDrawsBefore = ctx.shaderCompileGuard.deferredShadowDraws;
    fns.ensurePointLightCount();
    ctx.scene.updateMatrixWorld();
    ctx.settledCuller.refreshIfDue(time);
    ctx.worldProfiler.mark("matrices");
    ctx.scene.matrixWorldAutoUpdate = false;
    ctx.worldProfiler.beginGpu();
    const renderStartedAt = performance.now();
    try {
      if (!(ctx.firstViewHoldUntil > time)) {
        ctx.firstViewHoldUntil = 0;
        if (ctx.constructionCanvasPaints.length) fns.releaseConstructionCanvases();
        fns.updateCompactDistrictView();
        ctx.settledCuller.render(ctx.renderer, ctx.camera);
      }
    } finally {
      ctx.worldProfiler.endGpu();
      ctx.scene.matrixWorldAutoUpdate = true;
    }
    ctx.worldProfiler.mark("render");
    ctx.worldProfiler.endFrame();
    ctx.programPacer.tick();
    ctx.stripedUploads.step(ctx.programPacer.busy() ? 1 : undefined);
    const shadowDrawsDeferred =
      ctx.shaderCompileGuard.deferredShadowDraws !== deferredShadowDrawsBefore;
    if (shadowDrawsDeferred) {
      nextShadowMapUpdateAt = time + SHADOW_MAP_DEFERRED_RETRY_MS;
    }
    const frameWorkMs = Math.max(0, performance.now() - frameWorkStartedAt);
    ctx.renderStalls.endFrame(frameWorkMs, performance.now() - renderStartedAt);
    if (
      frameWorkMs >= RENDER_STALL_THRESHOLD_MS &&
      document.visibilityState === "visible"
    ) {
      if (refreshedShadowMap && !shadowDrawsDeferred) {
        nextShadowMapUpdateAt = Math.max(
          nextShadowMapUpdateAt,
          time + SHADOW_MAP_STALL_COOLDOWN_MS,
        );
      }
      if (time - lastRenderStallLogAt >= RENDER_STALL_LOG_COOLDOWN_MS) {
        lastRenderStallLogAt = time;
        scheduleRenderStallWarning(frameWorkMs, refreshedShadowMap);
      } else {
        suppressedRenderStalls += 1;
      }
    }
    ctx.diagnosticsFrameCount = Math.min(
      1_000_000,
      ctx.diagnosticsFrameCount + 1,
    );
    ctx.diagnosticsFrameWorkTotalMs = Math.min(
      3_600_000,
      ctx.diagnosticsFrameWorkTotalMs + frameWorkMs,
    );
    ctx.diagnosticsRendererCalls = Math.max(
      0,
      Math.min(10_000_000, Number(ctx.renderer.info?.render?.calls) || 0),
    );
    ctx.diagnosticsRendererTriangles = Math.max(
      0,
      Math.min(1_000_000_000, Number(ctx.renderer.info?.render?.triangles) || 0),
    );
    ctx.diagnosticsRendererLines = Math.max(
      0,
      Math.min(100_000_000, Number(ctx.renderer.info?.render?.lines) || 0),
    );
    ctx.diagnosticsRendererPoints = Math.max(
      0,
      Math.min(100_000_000, Number(ctx.renderer.info?.render?.points) || 0),
    );
    // Last, so the window it measures is a whole frame of real work.
    fns.sampleAdaptiveQuality(time);
  }

  function setPaused(paused) {
    if (ctx.running === !paused) return;
    ctx.running = !paused;
    // Frames the loop never presented are not evidence about this device, and
    // the gap across a pause is not a slow window.
    ctx.qualitySampleStartedAt = 0;
    ctx.qualitySampleFrames = 0;
    if (ctx.running) {
      ctx.lastFrame = performance.now();
      ctx.nextRenderFrameAt = 0;
      ctx.lastVisualAnimationAt = ctx.lastFrame;
      ctx.nextVisualAnimationAt = 0;
      // Browsers may discard a WebGL backing store while a tab, iframe, or
      // rapidly scrolled surface is occluded. Paint once synchronously before
      // waiting for the next animation callback so re-entry never exposes the
      // canvas clear colour as a blank frame.
      if (!(ctx.firstViewHoldUntil > performance.now())) {
        fns.updateCompactDistrictView();
        ctx.renderer.render(ctx.scene, ctx.camera);
      }
      ctx.renderer.setAnimationLoop(animate);
    } else {
      ctx.renderer.setAnimationLoop(null);
    }
  }

  function setPreviewCadence(preview) {
    const interval = preview
      ? PREVIEW_RENDER_FRAME_INTERVAL_MS
      : RENDER_FRAME_INTERVAL_MS;
    if (interval === ctx.renderFrameIntervalMs) return;
    ctx.renderFrameIntervalMs = interval;
    // Leaving preview must not wait out a deadline measured against the
    // slower interval, so the next callback presents immediately.
    ctx.nextRenderFrameAt = 0;
    // The cadence the ladder compares against just moved. A window that
    // straddles both intervals measures neither, so start a fresh one.
    ctx.qualitySampleStartedAt = 0;
    ctx.qualitySampleFrames = 0;
  }

  return {
    animate,
    setPaused,
    setPreviewCadence,
  };
}
