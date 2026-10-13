import { hashNumber } from "../../world-avatar-face.js";
import {
  FORKBOT_HOME,
  FORKBOT_PEER_ID,
  WORLD_AGENT_BOTS,
  agentBotAvatarLook,
  drawForkbotScreen,
} from "../agent-bots.js";
import { canvasArtTexture, canvasTexture } from "../canvas.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { projectAssetTexture } from "../town.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  ctx,
  fns,
}) {
  function agentBotProviderConfig(provider) {
    const wanted = String(provider || "").toLowerCase();
    return WORLD_AGENT_BOTS.find((config) =>
      wanted === config.id ||
      (wanted === "claude-code" && config.id === "claude"),
    ) || null;
  }

  function createAgentBotAvatar(session, index) {
    const providerConfig = agentBotProviderConfig(session?.provider);
    const sessionId = String(session?.id || "").trim();
    if (!providerConfig || !sessionId) return null;
    // Spread a large fleet into a shallow grid around the provider's home. A
    // session gets a deterministic slot on every refresh, so existing robots
    // never jump when an older item changes status.
    const occupiedSlots = new Set(
      [...ctx.agentBotStates.values()]
        .filter((state) => state.config.id === providerConfig.id)
        .map((state) => state.slot),
    );
    let slot = hashNumber(sessionId) % 64;
    for (let attempt = 0; occupiedSlots.has(slot) && attempt < 64; attempt += 1) {
      slot = (slot + 1) % 64;
    }
    const lane = Math.floor(slot / 8);
    const column = slot % 8;
    const direction = providerConfig.id === "codex" ? 1 : -1;
    const config = {
      ...providerConfig,
      home: [
        providerConfig.home[0] + direction * lane * 1.65,
        providerConfig.home[1],
        providerConfig.home[2] + (column - 3.5) * 1.55,
      ],
    };
    const botKey = `session:${sessionId}`;
    // The robot wears its model's portrait: emblem face plate, matching body
    // paint, and the portrait's accent light as its halo and status glow.
    const look = agentBotAvatarLook(session?.provider, session?.model);
    const avatar = new THREE.Group();
    avatar.name = `${config.id}-agent-droid-${sessionId}`;
    avatar.userData.name = `${config.label} ${
      Number(session?.localAgentId) > 0
        ? `#${Number(session.localAgentId)}`
        : `#${index + 1}`
    }`;
    avatar.userData.id = botKey;
    avatar.userData.accountStatus = "Organization agent";
    avatar.userData.phase = hashNumber(sessionId) * 0.0001;
    avatar.position.set(...config.home);
    avatar.position.y += 7 + (slot % 3) * 0.8;
    avatar.visible = false;
    const ball = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.5, 22, 15),
      makeMaterial(THREE, "#162d32", {
        metalness: 0.7,
        roughness: 0.24,
        emissive: look.shell,
        emissiveIntensity: 0.25,
      }),
    );
    ball.position.y = 0.58;
    avatar.add(ball);
    const shell = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.48, 0.54, 0.72, 18),
      makeMaterial(THREE, look.shell, {
        metalness: 0.55,
        roughness: 0.28,
        emissive: look.shell,
        emissiveIntensity: 0.2,
      }),
    );
    shell.position.y = 1.08;
    avatar.add(shell);
    // Every portrait shares the same ornate gold rim, so the armor gets thin
    // gold bands at the shell's edges regardless of model colourway.
    const trimMaterial = makeMaterial(THREE, "#d8b04a", {
      metalness: 0.9,
      roughness: 0.24,
      emissive: "#8a6a1d",
      emissiveIntensity: 0.28,
    });
    const trimTop = new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", 0.482, 0.02, 8, 36),
      trimMaterial,
    );
    trimTop.rotation.x = Math.PI / 2;
    trimTop.position.y = 1.42;
    avatar.add(trimTop);
    const trimBottom = new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", 0.538, 0.02, 8, 36),
      trimMaterial,
    );
    trimBottom.rotation.x = Math.PI / 2;
    trimBottom.position.y = 0.74;
    avatar.add(trimBottom);
    // Face plate: the model emblem itself. The texture is cached per path, so
    // a 50-robot fleet decodes at most the seven portraits; the emissive copy
    // keeps the face readable after the world switches to night.
    const faceTexture = projectAssetTexture(THREE, look.texture);
    const face = new THREE.Mesh(
      resources.primitive(THREE, "CircleGeometry", 0.26, 36),
      resources.uniqueMaterial(THREE, {
        map: faceTexture,
        emissive: "#ffffff",
        emissiveMap: faceTexture,
        emissiveIntensity: 0.72,
        roughness: 0.52,
        metalness: 0.18,
      }, "MeshStandardMaterial"),
    );
    face.name = `agent-face:${sessionId}`;
    face.userData.sharedAssetMap = true;
    face.position.set(0, 1.18, 0.548);
    avatar.add(face);
    // The halo ring around the face carries the old eye's status pulse: it is
    // registered as the state's `eye`, so updateAgentBots keeps animating the
    // same material slot it always has.
    const eye = new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", 0.276, 0.024, 10, 44),
      makeMaterial(THREE, "#d8b04a", {
        metalness: 0.85,
        roughness: 0.22,
        emissive: look.glow,
        emissiveIntensity: 1.8,
      }),
    );
    eye.position.set(0, 1.18, 0.542);
    avatar.add(eye);
    const statusStem = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.025, 0.025, 0.34, 7),
      makeSharedMaterial(THREE, "#718087", {
        metalness: 0.6,
        roughness: 0.34,
      }),
    );
    statusStem.position.set(0, 1.72, 0);
    avatar.add(statusStem);
    const statusLight = new THREE.Mesh(
      resources.primitive(THREE, "SphereGeometry", 0.085, 10, 7),
      makeMaterial(THREE, "#d29922", {
        emissive: "#d29922",
        emissiveIntensity: 1.2,
      }),
    );
    statusLight.name = `agent-status-light:${sessionId}`;
    statusLight.position.set(0, 1.94, 0);
    avatar.add(statusLight);
    const screenLabel = `${config.label} · ${look.name.toUpperCase()}`;
    const screenTexture = canvasArtTexture(THREE, 512, 176, (context, canvas) => {
      context.fillStyle = "#071917";
      context.fillRect(0, 0, canvas.width, canvas.height);
      context.strokeStyle = look.glow;
      context.lineWidth = 10;
      context.strokeRect(6, 6, canvas.width - 12, canvas.height - 12);
      context.fillStyle = look.glow;
      context.textAlign = "center";
      context.textBaseline = "middle";
      context.font = screenLabel.length > 12
        ? '800 46px "ForkMesh Mono", ui-monospace, monospace'
        : '800 62px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(screenLabel, canvas.width / 2, canvas.height / 2);
    });
    const screen = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.66, 0.24),
      resources.uniqueMaterial(THREE, { map: screenTexture, toneMapped: false }, "MeshBasicMaterial"),
    );
    screen.position.set(0, 0.79, 0.575);
    avatar.add(screen);
    avatar.userData.rollingBall = ball;
    avatar.traverse((child) => {
      if (!child.isMesh) return;
      child.userData.agentBotChat = config.id;
      child.userData.repositoryAgentSession = session;
      ctx.interactive.push(child);
    });
    ctx.agentBots.set(botKey, avatar);
    ctx.agentBotStates.set(botKey, {
      config,
      look,
      avatar,
      botKey,
      session,
      status: String(session?.status || "security_pending"),
      statusLight,
      eye,
      slot,
      spawnedAt: performance.now(),
      target: new THREE.Vector3(...config.home),
      nextWanderAt: 0,
      excitement: null,
      completion: null,
    });
    ctx.world.add(avatar);
    fns.registerWorldElement(
      "agent-npcs",
      "Agent bot avatars",
      "Avatars & bots",
      avatar,
      () => ctx.agentBots.get(botKey) === avatar,
    );
    return ctx.agentBotStates.get(botKey);
  }

  function removeAgentBotAvatar(botKey) {
    const state = ctx.agentBotStates.get(botKey);
    const avatar = state?.avatar;
    if (!avatar) return;
    avatar.traverse((child) => {
      let interactiveIndex = ctx.interactive.indexOf(child);
      while (interactiveIndex >= 0) {
        ctx.interactive.splice(interactiveIndex, 1);
        interactiveIndex = ctx.interactive.indexOf(child);
      }
      child.geometry?.dispose?.();
      // Face plates borrow the model portrait from the shared per-path
      // texture cache; disposing it here would blank every other robot
      // wearing the same emblem, so only bot-private maps are released.
      const sharedMap = Boolean(child.userData.sharedAssetMap);
      if (Array.isArray(child.material)) {
        child.material.forEach((material) => {
          if (!sharedMap) material?.map?.dispose?.();
          material?.dispose?.();
        });
      } else {
        if (!sharedMap) child.material?.map?.dispose?.();
        child.material?.dispose?.();
      }
    });
    avatar.parent?.remove?.(avatar);
    ctx.agentBots.delete(botKey);
    ctx.agentBotStates.delete(botKey);
    fns.pruneDeadElementRoots(ctx.worldElements.get("agent-npcs"));
  }

  function agentBotState(botId) {
    const wanted = String(botId || "").toLowerCase();
    return ctx.agentBotStates.get(wanted) ||
      [...ctx.agentBotStates.values()].find(
        (state) => state.config.id === wanted,
      ) || null;
  }

  function reconcileSessionAgentBots(sessions = []) {
    const visibleSessions = (Array.isArray(sessions) ? sessions : [])
      .filter(
        (session) =>
          String(session?.id || "").trim() &&
          agentBotProviderConfig(session?.provider),
      )
      .slice(0, 50);
    const live = new Set();
    visibleSessions.forEach((session, index) => {
      const botKey = `session:${String(session.id).trim()}`;
      live.add(botKey);
      let state = ctx.agentBotStates.get(botKey);
      if (!state) state = createAgentBotAvatar(session, index);
      if (!state) return;
      state.session = session;
      state.status = String(session?.status || "security_pending");
      state.avatar.visible = ctx.agentBotAccessAllowed;
      state.avatar.traverse((child) => {
        if (!child.isMesh) return;
        child.userData.agentBotChat = ctx.agentBotAccessAllowed
          ? state.config.id
          : "";
        child.userData.repositoryAgentSession = session;
      });
    });
    for (const botKey of [...ctx.agentBotStates.keys()]) {
      if (!live.has(botKey)) removeAgentBotAvatar(botKey);
    }
  }

  return {
    agentBotState,
    reconcileSessionAgentBots,
  };
}

export function build({
  THREE,
  animated,
  ctx,
  interactive,
  registerWorldElement,
  world,
}) {
  // ForkBot is a compact rolling droid, rather than another humanoid avatar.
  // Its full body is a single click target that opens the shared General chat.
  const forkbot = new THREE.Group();
  forkbot.name = "forkbot-rolling-droid";
  forkbot.userData.name = "ForkBot";
  forkbot.userData.id = FORKBOT_PEER_ID;
  forkbot.userData.accountStatus = "Bot";
  forkbot.userData.phase = hashNumber(FORKBOT_PEER_ID) * 0.0001;
  forkbot.position.set(...FORKBOT_HOME);
  const rollingBall = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.56, 24, 16),
    makeMaterial(THREE, "#173d4a", {
      metalness: 0.7,
      roughness: 0.22,
      emissive: "#0e6075",
      emissiveIntensity: 0.35,
    }),
  );
  rollingBall.position.y = 0.62;
  forkbot.add(rollingBall);
  const chassis = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.5, 0.57, 0.4, 20),
    makeSharedMaterial(THREE, "#c9eee1", { metalness: 0.55, roughness: 0.3 }),
  );
  chassis.position.y = 1.08;
  forkbot.add(chassis);
  const dome = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.46, 24, 14, 0, Math.PI * 2, 0, Math.PI / 2),
    makeSharedMaterial(THREE, "#4dc8e8", {
      metalness: 0.65,
      roughness: 0.18,
      emissive: "#176b88",
      emissiveIntensity: 0.5,
    }),
  );
  dome.position.y = 1.26;
  forkbot.add(dome);
  const eye = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.11, 14, 10),
    makeMaterial(THREE, "#ffdc76", { emissive: "#ffbd3d", emissiveIntensity: 2 }),
  );
  eye.position.set(0, 1.35, 0.43);
  forkbot.add(eye);
  const robotChest = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.76, 0.28, 0.08),
    makeSharedMaterial(THREE, "#18343b", {
      emissive: "#1d94a8",
      emissiveIntensity: 0.5,
      metalness: 0.72,
      roughness: 0.28,
    }),
  );
  robotChest.position.set(0, 1.02, 0.53);
  forkbot.add(robotChest);
  const antenna = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.035, 0.035, 0.42, 8),
    makeSharedMaterial(THREE, "#77d9ff", { emissive: "#2b9eb8", emissiveIntensity: 1.2 }),
  );
  antenna.position.set(0, 1.78, 0);
  forkbot.add(antenna);
  const antennaLight = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.1, 12, 8),
    makeMaterial(THREE, "#ff6f91", { emissive: "#ff406f", emissiveIntensity: 1.8 }),
  );
  antennaLight.position.set(0, 2.02, 0);
  forkbot.add(antennaLight);
  forkbot.userData.forkbotAntennaLight = antennaLight;
  forkbot.userData.rollingBall = rollingBall;
  // The chest screen (adhoc #369): idle it shows the FORKBOT wordmark; when a
  // mention pulls the droid over it echoes the speaker's line and then runs
  // the thinking dots (updateForkbot) until the reply lands in the room.
  const forkbotScreenTexture = canvasTexture(THREE, 512, 224, (context, canvas) =>
    drawForkbotScreen(context, canvas, null),
  );
  const forkbotScreen = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 0.82, 0.36),
    resources.uniqueMaterial(THREE, { map: forkbotScreenTexture, toneMapped: false }, "MeshBasicMaterial"),
  );
  forkbotScreen.position.set(0, 1.02, 0.58);
  forkbot.add(forkbotScreen);
  forkbot.traverse((child) => {
    if (!child.isMesh) return;
    child.userData.forkbotChat = true;
    interactive.push(child);
  });
  animated.push((time) => {
    // An excited ForkBot (someone just mentioned it) flashes its antenna and
    // eye much faster than the idle glow.
    const excited = Boolean(ctx.forkbotExcitement);
    antennaLight.material.emissiveIntensity = excited
      ? 2.2 + (Math.sin(time * 0.022) + 1) * 1.1
      : 1.2 + (Math.sin(time * 0.006) + 1) * 0.5;
    eye.material.emissiveIntensity = excited
      ? 2 + (Math.sin(time * 0.028) + 1) * 0.9
      : 1.25 + (Math.sin(time * 0.008) + 1) * 0.75;
  });
  world.add(forkbot);
  registerWorldElement("forkbot", "ForkBot", "Avatars & bots", forkbot);
  return {
    forkbot,
    forkbotScreenTexture,
  };
}
