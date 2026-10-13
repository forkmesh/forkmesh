import { hashNumber } from "../../world-avatar-face.js";
import {
  AGENT_BOT_STATUS_IDLE_VISUAL,
  AGENT_BOT_STATUS_VISUALS,
  DESKTOP_AGENT_DROP_HEIGHT,
  DESKTOP_AGENT_DROP_MS,
  DESKTOP_AGENT_GRID_SPACING,
  FORKBOT_ECHO_MS,
  FORKBOT_EXCITED_SPEED,
  FORKBOT_GREETING_RANGE,
  FORKBOT_GREETING_TIMEOUT_MS,
  FORKBOT_HOME,
  FORKBOT_PEER_ID,
  FORKBOT_SPEED,
  FORKBOT_THINKING_TIMEOUT_MS,
  FORKBOT_WANDER_RADIUS,
  createDesktopAgentRobot,
  desktopAgentBallMaterial,
  desktopAgentBodyMaterial,
  desktopAgentModel,
  desktopAgentStatus,
  drawForkbotScreen,
} from "../agent-bots.js";
import {
  GAIT_LEG_SWING,
  animateAvatarActivity,
  applyAvatarLookDirection,
  applyLegPitch,
  applySeatedLegPose,
  seatedAvatarY,
} from "../avatars.js";
import { makeLabelSprite } from "../labels.js";
import { WORLD_RADIUS } from "../layout.js";
import { clamp, disposeObject3D } from "../object-utils.js";

export function functions({
  THREE,
  identity,
  reducedMotion,
  ctx,
  fns,
}) {
  // A remote visitor parked this close to a seat's rest spot is riding it.
  const SWING_REST_CLAIM_DISTANCE_SQ = 0.81;
  const REMOTE_SETTLED_DISTANCE_SQ = 1e-8;
  const REMOTE_SETTLED_HEADING = 1e-5;
  let forkbotNextWanderAt = 0;
  let forkbotGreeting = null;

  // Scratch vectors reused across frames — this is the hottest loop in the
  // scene and fresh allocations here turn straight into GC pressure.
  const swingRestScratch = [];


  function updateRemotePlayers(delta, time) {
    // Swing occupancy is re-derived every frame from where visitors stand: a
    // remote visitor parked on a seat's rest spot is riding that swing, which
    // is what caps the ride at three people and lets a local click on a taken
    // seat be refused.
    const swingRestPoints = swingRestScratch;
    ctx.swingStates.forEach((swing, seatIndex) => {
      swing.remoteId = "";
      swingRestPoints[seatIndex] = swing.anchor.getWorldPosition(
        swingRestPoints[seatIndex] || new THREE.Vector3(),
      );
    });
    swingRestPoints.length = ctx.swingStates.length;
    const positionBlend = 1 - Math.pow(0.002, delta);
    const turnBlend = 1 - Math.pow(0.01, delta);
    ctx.remoteDetailMatrix.multiplyMatrices(
      ctx.camera.projectionMatrix,
      ctx.camera.matrixWorldInverse,
    );
    ctx.remoteDetailFrustum.setFromProjectionMatrix(ctx.remoteDetailMatrix);
    ctx.remotePlayers.forEach((avatar, remoteId) => {
      if (
        avatar.position.distanceToSquared(avatar.userData.targetPosition) >
        REMOTE_SETTLED_DISTANCE_SQ
      ) {
        avatar.position.lerp(avatar.userData.targetPosition, positionBlend);
      }
      let headingDelta = avatar.userData.targetHeading - avatar.rotation.y;
      headingDelta = Math.atan2(Math.sin(headingDelta), Math.cos(headingDelta));
      if (Math.abs(headingDelta) > REMOTE_SETTLED_HEADING) {
        avatar.rotation.y += headingDelta * turnBlend;
      }
      const detailDelta = fns.remoteAvatarDetailDelta(avatar, time, delta);
      const animateDetail = detailDelta >= 0;
      if (animateDetail) {
        const lookBlend =
          detailDelta === delta ? turnBlend : 1 - Math.pow(0.01, detailDelta);
        let lookYawDelta = avatar.userData.targetLookYaw - avatar.userData.lookYaw;
        lookYawDelta = Math.atan2(Math.sin(lookYawDelta), Math.cos(lookYawDelta));
        avatar.userData.lookYaw += lookYawDelta * lookBlend;
        avatar.userData.lookPitch +=
          (avatar.userData.targetLookPitch - avatar.userData.lookPitch) *
          lookBlend;
        applyAvatarLookDirection(
          avatar,
          avatar.userData.lookYaw,
          avatar.userData.lookPitch,
          false,
        );
      }
      const walking = avatar.position.distanceToSquared(avatar.userData.targetPosition) > 0.01;
      let riddenSwing = -1;
      if (!walking) {
        for (let seatIndex = 0; seatIndex < ctx.swingStates.length; seatIndex += 1) {
          if (ctx.swingRide?.index === seatIndex) continue;
          if (ctx.swingStates[seatIndex].remoteId) continue;
          const rest = swingRestPoints[seatIndex];
          const dx = avatar.userData.targetPosition.x - rest.x;
          const dz = avatar.userData.targetPosition.z - rest.z;
          if (dx * dx + dz * dz < SWING_REST_CLAIM_DISTANCE_SQ) {
            riddenSwing = seatIndex;
            ctx.swingStates[seatIndex].remoteId = remoteId;
            break;
          }
        }
      }
      if (riddenSwing >= 0) {
        const swing = ctx.swingStates[riddenSwing];
        const seatTop = swing.seat.getWorldPosition(ctx.swingSeatScratch);
        avatar.position.set(
          seatTop.x,
          seatedAvatarY(seatTop.y + ctx.SWING_SEAT_HALF_THICKNESS, avatar.scale.x),
          seatTop.z,
        );
        // Same yaw-then-pitch order as the local rider so the lean stays
        // front-to-back along the ropes instead of rolling sideways.
        avatar.rotation.order = "YXZ";
        avatar.rotation.y = ctx.swingSet.rotation.y;
        avatar.rotation.x = swing.pivot.rotation.x;
        avatar.userData.leftArm.rotation.x = ctx.SWING_ARM_HOLD_PITCH;
        avatar.userData.rightArm.rotation.x = ctx.SWING_ARM_HOLD_PITCH;
        applySeatedLegPose(avatar);
        if (animateDetail) {
          animateAvatarActivity(avatar, time, detailDelta, reducedMotion);
        }
        return;
      }
      avatar.rotation.x = 0;
      if (!animateDetail) return;
      const recentlyActiveInLounge =
        avatar.userData.loungeActivity === "recent";
      const gait = walking
        ? Math.sin(time * 0.009 + avatar.userData.phase) * 0.42
        : recentlyActiveInLounge
          ? Math.sin(time * 0.012 + avatar.userData.phase) * 0.18
          : 0;
      // Members occupying a stool in the campfire circle sit like local bench
      // sitters do once they arrive at their seat.
      const seatedAtCampfire =
        (avatar.userData.campfireSeated === true ||
          Boolean(avatar.userData.loungeActivity)) &&
        !walking;
      avatar.userData.leftArm.rotation.x = gait;
      avatar.userData.rightArm.rotation.x = -gait;
      if (seatedAtCampfire) {
        applySeatedLegPose(avatar);
      } else {
        applyLegPitch(avatar, -gait * GAIT_LEG_SWING, gait * GAIT_LEG_SWING);
      }
      if (recentlyActiveInLounge && !walking && !reducedMotion) {
        avatar.position.y =
          avatar.userData.targetPosition.y +
          Math.abs(Math.sin(time * 0.004 + avatar.userData.phase)) * 0.055;
      }
      animateAvatarActivity(avatar, time, detailDelta, reducedMotion);
    });
  }

  // ForkBot wanders the Town Square on its own; when world.js reports a
  // visitor's first movement or mouse activity (greetForkbot) it walks over
  // and floats a welcome bubble instead of picking the next wander spot. A
  // chat mention (exciteForkbot) outranks both: the droid rushes to whoever
  // spoke while its chest screen echoes the line and then thinks out loud.
  function updateForkbot(delta, time) {
    const data = ctx.forkbot.userData;
    let target = ctx.forkbotWanderTarget;
    let speed = FORKBOT_SPEED;
    if (ctx.forkbotExcitement) {
      target = ctx.forkbotExcitement.avatar.position;
      speed = FORKBOT_EXCITED_SPEED;
      const waited = performance.now() - ctx.forkbotExcitement.startedAt;
      if (!ctx.forkbotExcitement.thinking && waited >= FORKBOT_ECHO_MS) {
        ctx.forkbotExcitement.thinking = true;
      }
      if (ctx.forkbotExcitement.thinking) {
        const dotPhase = Math.floor(time / 400) % 3;
        if (dotPhase !== ctx.forkbotExcitement.dotPhase) {
          ctx.forkbotExcitement.dotPhase = dotPhase;
          paintForkbotScreen({
            message: ctx.forkbotExcitement.message,
            thinking: true,
            dotPhase,
          });
        }
      }
      // The reply normally clears this state (showChatBubble); the timeout
      // only covers an unavailable bot so the dots don't run forever.
      if (waited >= FORKBOT_THINKING_TIMEOUT_MS) {
        settleForkbot(time);
        target = ctx.forkbotWanderTarget;
      }
    } else if (forkbotGreeting) {
      target = ctx.player.position;
      const waited = performance.now() - forkbotGreeting.startedAt;
      const reach = ctx.forkbot.position.distanceTo(ctx.player.position);
      if (
        reach <= FORKBOT_GREETING_RANGE ||
        waited >= FORKBOT_GREETING_TIMEOUT_MS
      ) {
        fns.showChatBubble(FORKBOT_PEER_ID, forkbotGreeting.message);
        forkbotGreeting = null;
        // Linger beside the visitor for a moment before wandering off.
        ctx.forkbotWanderTarget.copy(ctx.forkbot.position);
        forkbotNextWanderAt = time + 9000;
        target = ctx.forkbotWanderTarget;
      }
    } else if (time >= forkbotNextWanderAt) {
      const angle = Math.random() * Math.PI * 2;
      const radius = 3 + Math.random() * FORKBOT_WANDER_RADIUS;
      ctx.forkbotWanderTarget.set(
        clamp(
          FORKBOT_HOME[0] + Math.cos(angle) * radius,
          -WORLD_RADIUS,
          WORLD_RADIUS,
        ),
        FORKBOT_HOME[1],
        clamp(
          FORKBOT_HOME[2] + Math.sin(angle) * radius,
          -WORLD_RADIUS,
          WORLD_RADIUS,
        ),
      );
      forkbotNextWanderAt = time + 4000 + Math.random() * 8000;
    }
    const dx = target.x - ctx.forkbot.position.x;
    const dz = target.z - ctx.forkbot.position.z;
    const distance = Math.hypot(dx, dz);
    const arrive =
      forkbotGreeting || ctx.forkbotExcitement ? FORKBOT_GREETING_RANGE * 0.8 : 0.4;
    const walking = distance > arrive;
    if (walking) {
      const step = Math.min(distance - arrive, speed * delta);
      ctx.forkbot.position.x += (dx / distance) * step;
      ctx.forkbot.position.z += (dz / distance) * step;
    }
    if (distance > 0.05) {
      data.targetHeading = Math.atan2(dx, dz);
    }
    let headingDelta = data.targetHeading - ctx.forkbot.rotation.y;
    headingDelta = Math.atan2(Math.sin(headingDelta), Math.cos(headingDelta));
    ctx.forkbot.rotation.y += headingDelta * (1 - Math.pow(0.01, delta));
    if (walking) {
      data.rollingBall.rotation.x -= speed * delta * 1.8;
      data.rollingBall.rotation.z = Math.sin(time * 0.006 + data.phase) * 0.08;
    }
    // Excited hops so the mention visibly lands even from across the square.
    ctx.forkbot.position.y =
      ctx.forkbotExcitement && !reducedMotion
        ? FORKBOT_HOME[1] +
          Math.abs(Math.sin(time * 0.012 + data.phase)) * 0.16
        : FORKBOT_HOME[1];
  }

  function updateAgentBots(delta, time) {
    for (const state of ctx.agentBotStates.values()) {
      const { avatar, config } = state;
      if (!ctx.agentBotAccessAllowed || !avatar.visible) continue;
      const status = String(state.status || "security_pending");
      const statusVisual =
        AGENT_BOT_STATUS_VISUALS.get(status) || AGENT_BOT_STATUS_IDLE_VISUAL;
      if (state.statusLight.userData.statusColor !== statusVisual.color) {
        state.statusLight.userData.statusColor = statusVisual.color;
        state.statusLight.material.color.set(statusVisual.color);
        state.statusLight.material.emissive.set(statusVisual.color);
      }
      const pulse = reducedMotion
        ? 0.5
        : (Math.sin(time * statusVisual.speed + avatar.userData.phase) + 1) / 2;
      state.statusLight.material.emissiveIntensity =
        statusVisual.floor + pulse * statusVisual.range;
      const active = status === "running" || Boolean(state.excitement);
      state.eye.material.emissiveIntensity = active
        ? 2.1 + (Math.sin(time * 0.025) + 1) * 0.9
        : 1.2 + (Math.sin(time * 0.007) + 1) * 0.55;
      let target = state.target;
      let speed = FORKBOT_SPEED * 0.9;
      if (state.completion) {
        target = state.completion.target;
        speed = FORKBOT_EXCITED_SPEED;
      } else if (state.excitement) {
        target = state.excitement.avatar.position;
        speed = FORKBOT_EXCITED_SPEED;
        if (
          performance.now() - state.excitement.startedAt >=
          FORKBOT_THINKING_TIMEOUT_MS
        ) {
          state.excitement = null;
          state.target.copy(avatar.position);
          state.nextWanderAt = time + 9000;
          target = state.target;
        }
      } else if (time >= state.nextWanderAt) {
        const angle = Math.random() * Math.PI * 2;
        const radius = 3 + Math.random() * (FORKBOT_WANDER_RADIUS * 0.78);
        state.target.set(
          clamp(config.home[0] + Math.cos(angle) * radius, -WORLD_RADIUS, WORLD_RADIUS),
          config.home[1],
          clamp(config.home[2] + Math.sin(angle) * radius, -WORLD_RADIUS, WORLD_RADIUS),
        );
        state.nextWanderAt = time + 5000 + Math.random() * 8500;
      }
      const dx = target.x - avatar.position.x;
      const dz = target.z - avatar.position.z;
      const distance = Math.hypot(dx, dz);
      const arrive = state.excitement ? FORKBOT_GREETING_RANGE * 0.8 : 0.75;
      const walking = distance > arrive;
      if (walking) {
        const step = Math.min(distance - arrive, speed * delta);
        avatar.position.x += (dx / distance) * step;
        avatar.position.z += (dz / distance) * step;
      }
      if (distance > 0.05) {
        avatar.userData.targetHeading = Math.atan2(dx, dz);
      }
      let headingDelta = avatar.userData.targetHeading - avatar.rotation.y;
      headingDelta = Math.atan2(Math.sin(headingDelta), Math.cos(headingDelta));
      avatar.rotation.y += headingDelta * (1 - Math.pow(0.01, delta));
      if (walking) {
        avatar.userData.rollingBall.rotation.x -= speed * delta * 1.8;
      }
      const dropProgress = Math.min(
        1,
        Math.max(0, (performance.now() - state.spawnedAt) / 1150),
      );
      if (dropProgress < 1) {
        // A newly authorized session visibly arrives in the World. Ease the
        // fall and add a tiny decaying bounce; no physics object or light is
        // allocated, keeping a 50-agent fleet inexpensive.
        const remaining = 1 - dropProgress;
        avatar.position.y =
          config.home[1] + remaining * remaining * 7 +
          Math.abs(Math.sin(dropProgress * Math.PI * 3)) * remaining * 0.34;
      } else {
        avatar.position.y =
          (state.excitement || state.completion) && !reducedMotion
            ? config.home[1] +
              Math.abs(Math.sin(time * 0.011 + avatar.userData.phase)) * 0.12
            : config.home[1];
      }
      if (state.completion && !walking) {
        const taskLabel = state.completion.taskKey.startsWith("issue:")
          ? state.completion.taskKey.replace(/^issue:/, "")
          : state.completion.taskKey.replace(/^task:/, "");
        fns.showAvatarChatBubble(
          avatar,
          `Completed ${taskLabel}. Sending it to QA.`,
        );
        state.completion = null;
        state.target.copy(avatar.position);
        state.nextWanderAt = time + 9000;
      }
    }
  }

  function worldSpeakerAvatar(peerId) {
    return peerId === identity.id
      ? ctx.player
      : ctx.remotePlayers.get(String(peerId || "")) ||
        ctx.loungeMembers.get(String(peerId || ""));
  }

  function exciteAgentBot(peerId, botId, text) {
    if (!ctx.agentBotAccessAllowed) return false;
    const state = fns.agentBotState(botId);
    const avatar = worldSpeakerAvatar(peerId);
    const message = String(text || "").replace(/\s+/g, " ").trim().slice(0, 90);
    if (!state || !avatar || !message) return false;
    state.completion = null;
    state.excitement = {
      avatar,
      message,
      startedAt: performance.now(),
    };
    return true;
  }

  function completeAgentTask(botId, taskKey) {
    if (!ctx.agentBotAccessAllowed) return false;
    const state = fns.agentBotState(botId);
    const key = String(taskKey || "");
    if (!state || !key) return false;
    state.excitement = null;
    state.completion = {
      taskKey: key,
      target: new THREE.Vector3(
        ctx.worldQaBoard.position.x,
        state.config.home[1],
        ctx.worldQaBoard.position.z + 2.5,
      ),
    };
    return true;
  }

  function showAgentTaskBubble(botId, title) {
    if (!ctx.agentBotAccessAllowed) return false;
    // The task board names one general bot, so an unknown id speaks through
    // whichever bot avatar is present rather than staying silent.
    const state =
      fns.agentBotState(botId) ||
      ctx.agentBotStates.values().next().value;
    const taskTitle = String(title || "")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, 110);
    if (!state || !taskTitle) return false;
    state.excitement = null;
    state.completion = null;
    state.target.copy(state.avatar.position);
    state.nextWanderAt = performance.now() + 9000;
    return fns.showAvatarChatBubble(
      state.avatar,
      `New organization task: ${taskTitle}`,
    );
  }

  function setAgentBotAccess(allowed) {
    ctx.agentBotAccessAllowed = allowed === true;
    for (const [botId, avatar] of ctx.agentBots.entries()) {
      avatar.visible = ctx.agentBotAccessAllowed;
      const state = ctx.agentBotStates.get(botId);
      avatar.traverse((child) => {
        if (!child.isMesh) return;
        child.userData.agentBotChat = ctx.agentBotAccessAllowed
          ? state?.config?.id || ""
          : "";
      });
      if (!state || ctx.agentBotAccessAllowed) continue;
      state.excitement = null;
      state.completion = null;
      state.target.copy(avatar.position);
    }
    const deskData = ctx.world.userData.repositoryRecordDeskData;
    if (deskData) {
      fns.updateRepositoryRecordDesk(deskData.selection, deskData.records);
    }
    const activityData = ctx.world.userData.repositoryActivityData;
    if (activityData) {
      fns.updateRepositoryActivity(activityData.activity, activityData.selection);
    }
    return ctx.agentBotAccessAllowed;
  }

  function paintDesktopAgentBot(state) {
    const { avatar, session } = state;
    const appearance = desktopAgentModel(session.model, session.provider);
    const status = desktopAgentStatus(session.status);
    avatar.userData.body.material = desktopAgentBodyMaterial(THREE, appearance.shell);
    avatar.userData.rollingBall.material = desktopAgentBallMaterial(THREE, appearance.shell);
    const modelKey = `${appearance.label}|${appearance.icon}`;
    if (avatar.userData.modelKey !== modelKey) {
      avatar.userData.portrait.material.map?.dispose?.();
      avatar.userData.portrait.material.map =
        ctx.desktopAgentTextureLoader.load(appearance.icon);
      avatar.userData.portrait.material.needsUpdate = true;
      avatar.userData.modelKey = modelKey;
    }
    avatar.userData.statusLight.material.color.set(status.color);
    avatar.userData.statusLight.material.emissive.set(status.color);
    const effort = String(session.strength || "default").slice(0, 16);
    const labelKey = `${appearance.label}|${effort}|${status.label}`;
    if (avatar.userData.labelKey === labelKey) return;
    if (avatar.userData.label) {
      avatar.remove(avatar.userData.label);
      disposeObject3D(avatar.userData.label);
    }
    const label = makeLabelSprite(
      THREE,
      appearance.label,
      `${effort} effort · ${status.label}`,
      status.color,
    );
    label.name = "desktop-agent-status-label";
    label.scale.set(2.55, 0.85, 1);
    label.position.y = 2.86;
    avatar.add(label);
    avatar.userData.label = label;
    avatar.userData.labelKey = labelKey;
  }

  function defaultDesktopAgentTarget(index, total) {
    const columns = Math.max(1, Math.ceil(Math.sqrt(total)));
    const row = Math.floor(index / columns);
    const column = index % columns;
    return new THREE.Vector3(
      FORKBOT_HOME[0] + 7 +
        (column - (columns - 1) / 2) * DESKTOP_AGENT_GRID_SPACING,
      FORKBOT_HOME[1],
      FORKBOT_HOME[2] + 5 + row * DESKTOP_AGENT_GRID_SPACING,
    );
  }

  function updateDesktopAgentBots(sessions = []) {
    const safe = (Array.isArray(sessions) ? sessions : [])
      .filter((session) =>
        String(session?.taskId || "").trim() &&
        String(session?.id || "").trim(),
      )
      .slice(0, 24);
    const seen = new Set();
    const now = performance.now();
    safe.forEach((session, index) => {
      const key = String(session.taskId).trim().slice(0, 64);
      seen.add(key);
      let state = ctx.desktopAgentBots.get(key);
      if (!state) {
        const avatar = createDesktopAgentRobot(
          THREE,
          session,
          ctx.desktopAgentTextureLoader,
        );
        const target = defaultDesktopAgentTarget(index, safe.length);
        state = {
          avatar,
          session: { ...session },
          target,
          dropAt: now + index * 90,
          phase: hashNumber(key) * 0.0001,
        };
        avatar.position.set(
          target.x,
          target.y + DESKTOP_AGENT_DROP_HEIGHT,
          target.z,
        );
        ctx.desktopAgentBots.set(key, state);
        ctx.world.add(avatar);
        const registeredAvatar = avatar;
        fns.registerWorldElement(
          "agent-npcs",
          "Desktop agent bots",
          "Avatars & bots",
          avatar,
          () => ctx.desktopAgentBots.get(key)?.avatar === registeredAvatar,
        );
      } else {
        state.session = { ...session };
      }
      paintDesktopAgentBot(state);
    });
    for (const [key, state] of ctx.desktopAgentBots.entries()) {
      if (seen.has(key)) continue;
      ctx.world.remove(state.avatar);
      disposeObject3D(state.avatar);
      ctx.desktopAgentBots.delete(key);
    }
    return ctx.desktopAgentBots.size;
  }

  function summonDesktopAgentBots() {
    const states = Array.from(ctx.desktopAgentBots.values());
    if (!states.length) return 0;
    const columns = Math.max(1, Math.ceil(Math.sqrt(states.length)));
    const rows = Math.ceil(states.length / columns);
    const heading = ctx.player.rotation.y;
    const forward = new THREE.Vector3(Math.sin(heading), 0, Math.cos(heading));
    const right = new THREE.Vector3(Math.cos(heading), 0, -Math.sin(heading));
    const center = ctx.player.position.clone().addScaledVector(forward, 6.5);
    const now = performance.now();
    states.forEach((state, index) => {
      const row = Math.floor(index / columns);
      const column = index % columns;
      state.target.copy(center)
        .addScaledVector(
          right,
          (column - (columns - 1) / 2) * DESKTOP_AGENT_GRID_SPACING,
        )
        .addScaledVector(
          forward,
          (row - (rows - 1) / 2) * DESKTOP_AGENT_GRID_SPACING,
        );
      state.target.y = ctx.currentFloorY;
      state.dropAt = now + index * 110;
      state.avatar.position.set(
        state.target.x,
        state.target.y + DESKTOP_AGENT_DROP_HEIGHT,
        state.target.z,
      );
      state.avatar.visible = true;
    });
    return states.length;
  }

  function updateDesktopAgentBotMotion(delta, time) {
    const now = performance.now();
    for (const state of ctx.desktopAgentBots.values()) {
      const { avatar, target } = state;
      if (state.dropAt) {
        const elapsed = now - state.dropAt;
        if (elapsed < 0) {
          avatar.visible = false;
          continue;
        }
        avatar.visible = true;
        const progress = reducedMotion
          ? 1
          : clamp(elapsed / DESKTOP_AGENT_DROP_MS, 0, 1);
        const eased = 1 - Math.pow(1 - progress, 3);
        avatar.position.x = target.x;
        avatar.position.z = target.z;
        avatar.position.y =
          target.y + (1 - eased) * DESKTOP_AGENT_DROP_HEIGHT +
          Math.sin(progress * Math.PI * 3) * (1 - progress) * 0.55;
        if (progress >= 1) state.dropAt = 0;
      } else {
        const dx = target.x - avatar.position.x;
        const dz = target.z - avatar.position.z;
        const distance = Math.hypot(dx, dz);
        if (distance > 0.03) {
          const step = Math.min(distance, FORKBOT_SPEED * delta);
          avatar.position.x += (dx / distance) * step;
          avatar.position.z += (dz / distance) * step;
          avatar.userData.rollingBall.rotation.x -= step * 1.8;
          avatar.rotation.y = Math.atan2(dx, dz);
        }
        avatar.position.y = target.y +
          (reducedMotion ? 0 : Math.sin(time * 0.0025 + state.phase) * 0.035);
      }
      const light = avatar.userData.statusLight;
      const speed = state.session.status === "running" ? 0.014 : 0.007;
      light.material.emissiveIntensity =
        0.65 + (Math.sin(time * speed + state.phase) + 1) * 1.15;
    }
  }

  function paintForkbotScreen(state) {
    const canvas = ctx.forkbotScreenTexture.image;
    drawForkbotScreen(canvas.getContext("2d"), canvas, state);
    ctx.forkbotScreenTexture.needsUpdate = true;
  }

  // adhoc #369: any live speaker mentioning ForkBot in chat (world.js
  // handleWorldChatMessage) pulls the droid over to them. The chest screen
  // echoes the line straight away; once the echo has had a beat the thinking
  // dots run (updateForkbot) until the reply is broadcast into the room.
  function exciteForkbot(peerId, text) {
    const message = String(text || "").replace(/\s+/g, " ").trim().slice(0, 90);
    if (!message) return false;
    const avatar = worldSpeakerAvatar(peerId);
    if (!avatar) return false;
    forkbotGreeting = null;
    ctx.forkbotExcitement = {
      avatar,
      message,
      startedAt: performance.now(),
      thinking: false,
      dotPhase: 0,
    };
    paintForkbotScreen({ message, thinking: false, dotPhase: 0 });
    return true;
  }

  // Back to the idle wordmark, lingering beside the speaker for a moment
  // before the next wander pick.
  function settleForkbot(time) {
    ctx.forkbotExcitement = null;
    paintForkbotScreen(null);
    ctx.forkbotWanderTarget.copy(ctx.forkbot.position);
    forkbotNextWanderAt = time + 9000;
  }

  function greetForkbot(text) {
    const message = String(text || "")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, 140);
    if (!message || forkbotGreeting) return false;
    forkbotGreeting = { message, startedAt: performance.now() };
    return true;
  }

  return {
    updateRemotePlayers,
    updateForkbot,
    updateAgentBots,
    exciteAgentBot,
    completeAgentTask,
    showAgentTaskBubble,
    setAgentBotAccess,
    updateDesktopAgentBots,
    summonDesktopAgentBots,
    updateDesktopAgentBotMotion,
    exciteForkbot,
    settleForkbot,
    greetForkbot,
  };
}
