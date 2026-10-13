import { roundedRect } from "./canvas.js";
import { makeLabelSprite } from "./labels.js";
import { CONTINUOUS_CITY_SURFACE_Y } from "./layout.js";
import { makeMaterial, makeSharedMaterial } from "./materials.js";
import { setShadows } from "./object-utils.js";
import * as resources from "./resources.js";

// ForkBot's world presence: a wandering guide anchored to the Town Square.
// Chat bubbles addressed to this peer id float over its avatar, mirroring how
// visitor bubbles work (world.js handleWorldChatMessage).
export const FORKBOT_PEER_ID = "forkbot";
export const FORKBOT_HOME = Object.freeze([6, CONTINUOUS_CITY_SURFACE_Y, 12]);
export const DESKTOP_AGENT_GRID_SPACING = 3.2;
export const DESKTOP_AGENT_DROP_HEIGHT = 14;
export const DESKTOP_AGENT_DROP_MS = 1100;
export const FORKBOT_WANDER_RADIUS = 14;
export const FORKBOT_SPEED = 3.4;
export const AGENT_BOT_STATUS_IDLE_VISUAL = {
  color: "#58a6ff",
  speed: 0.0022,
  floor: 0.6,
  range: 0.55,
};
const AGENT_BOT_STATUS_FAILED_VISUAL = {
  color: "#f85149",
  speed: 0.009,
  floor: 0.35,
  range: 2.5,
};
const AGENT_BOT_STATUS_WAITING_VISUAL = {
  color: "#d29922",
  speed: 0.0045,
  floor: 0.45,
  range: 1.8,
};
export const AGENT_BOT_STATUS_VISUALS = new Map([
  ["running", { color: "#3fb950", speed: 0.014, floor: 0.55, range: 2.9 }],
  ["failed", AGENT_BOT_STATUS_FAILED_VISUAL],
  ["rejected", AGENT_BOT_STATUS_FAILED_VISUAL],
  ["queued", AGENT_BOT_STATUS_WAITING_VISUAL],
  ["security_pending", AGENT_BOT_STATUS_WAITING_VISUAL],
]);
export const FORKBOT_GREETING_RANGE = 3.2;
// If a visitor is out of reach (travelled to another space, moderation walls,
// …) the greeting still fires from wherever ForkBot got to.
export const FORKBOT_GREETING_TIMEOUT_MS = 12000;
// Noah speaks only on a genuine outside -> desk-side approach. The wider reset
// radius adds hysteresis, while the cooldown prevents a visitor pacing on the
// boundary from creating a new local bubble every frame.
export const OFFICE_RECEPTION_TALK_RANGE = 2.2;
export const OFFICE_RECEPTION_RESET_RANGE = 3.8;
export const OFFICE_RECEPTION_TALK_COOLDOWN_MS = 12000;
export const OFFICE_RECEPTION_GUEST_TIPS = Object.freeze([
  "The lobby is open to everyone. Log in to use elevator buttons for restricted team floors.",
  "Guests can browse public repository portals; log in before requesting access to restricted team floors.",
  "Public mirrors keep source readable. Log in to unlock only the team floors granted to your account.",
]);
export const OFFICE_RECEPTION_MEMBER_TIPS = Object.freeze([
  "Elevator buttons reflect your server-verified team access; locked floors stay locked.",
  "Repository portals show live mirrors, branches, issues, pull requests, and discussions.",
  "The rooftop laptop opens the real ForkMesh source browser; source edits stay in the desktop app or an optional Marketplace IDE plugin.",
]);
// A chat mention (exciteForkbot) sends the droid rushing to the speaker at a
// faster clip than its idle wander. The chest screen echoes the mention alone
// for a beat before the thinking dots join it, and the wait for a reply is
// bounded so an unavailable bot doesn't leave the dots running forever.
export const FORKBOT_EXCITED_SPEED = 5.6;
export const FORKBOT_ECHO_MS = 2500;
export const FORKBOT_THINKING_TIMEOUT_MS = 45000;
export const WORLD_AGENT_BOTS = Object.freeze([
  {
    id: "claude",
    label: "CLAUDE",
    home: Object.freeze([12, CONTINUOUS_CITY_SURFACE_Y, 11]),
    shell: "#d8895b",
    glow: "#ffb37f",
  },
  {
    id: "codex",
    label: "CODEX",
    home: Object.freeze([17, CONTINUOUS_CITY_SURFACE_Y, 10]),
    shell: "#75cbb4",
    glow: "#8fffe0",
  },
]);
// Model emblem art for the per-session agent robots. Each entry mirrors one
// portrait in /assets/bot-avatars/ (the -face.webp files are 512px repacks of
// the same art, small enough for a fleet): the emblem becomes the robot's
// face plate and the shell/glow colours repaint its body to match the
// portrait's armor and accent light.
const WORLD_AGENT_BOT_AVATAR_LOOKS = Object.freeze({
  opus: {
    texture: "/assets/bot-avatars/opus-face.webp",
    shell: "#b8892f",
    glow: "#b07bff",
  },
  sonnet: {
    texture: "/assets/bot-avatars/sonnet-face.webp",
    shell: "#d09a3c",
    glow: "#ff70bd",
  },
  haiku: {
    texture: "/assets/bot-avatars/haiku-face.webp",
    shell: "#c3ced1",
    glow: "#59e5e8",
  },
  fable: {
    texture: "/assets/bot-avatars/fable-face.webp",
    shell: "#a08434",
    glow: "#4fd9c2",
  },
  sol: {
    texture: "/assets/bot-avatars/sol-face.webp",
    shell: "#e9b23a",
    glow: "#ffd25e",
  },
  luna: {
    texture: "/assets/bot-avatars/luna-face.webp",
    shell: "#4a4173",
    glow: "#9678ff",
  },
  terra: {
    texture: "/assets/bot-avatars/terra-face.webp",
    shell: "#8e7a3d",
    glow: "#79e77f",
  },
});
const DESKTOP_AGENT_MODELS = Object.freeze({
  sol: Object.freeze({
    label: "Sol",
    icon: "./assets/agent-model-sol.webp",
    shell: "#e9b23a",
  }),
  luna: Object.freeze({
    label: "Luna",
    icon: "./assets/agent-model-luna.webp",
    shell: "#7868c8",
  }),
  terra: Object.freeze({
    label: "Terra",
    icon: "./assets/agent-model-terra.webp",
    shell: "#68b875",
  }),
  opus: Object.freeze({
    label: "Opus",
    icon: "./assets/agent-model-opus.webp",
    shell: "#b8892f",
  }),
  fable: Object.freeze({
    label: "Fable",
    icon: "./assets/agent-model-fable.webp",
    shell: "#a08434",
  }),
  sonnet: Object.freeze({
    label: "Sonnet",
    icon: "./assets/agent-model-sonnet.webp",
    shell: "#d09a3c",
  }),
});
const DESKTOP_AGENT_STATUSES = Object.freeze({
  queued: Object.freeze({ label: "Queued", color: "#77d9ff" }),
  running: Object.freeze({ label: "Working", color: "#77d9ff" }),
  waiting: Object.freeze({ label: "Waiting", color: "#ffcc72" }),
  success: Object.freeze({ label: "Done", color: "#73f0ad" }),
  failed: Object.freeze({ label: "Failed", color: "#ff7189" }),
  stopped: Object.freeze({ label: "Stopped", color: "#91a39a" }),
});

export function desktopAgentModel(model, provider) {
  const wanted = `${String(model || "")} ${String(provider || "")}`.toLowerCase();
  const key = Object.keys(DESKTOP_AGENT_MODELS).find((name) =>
    wanted.includes(name),
  );
  return DESKTOP_AGENT_MODELS[key || "terra"];
}

export function desktopAgentStatus(status) {
  const key = String(status || "").trim().toLowerCase();
  return DESKTOP_AGENT_STATUSES[key] || DESKTOP_AGENT_STATUSES.queued;
}

// A session names its model freely ("opus", "claude-sonnet-5", …), so match
// by substring first and only then fall back to a provider default: Claude
// runs read as Sonnet, Codex as Sol, OpenAI API as Luna, anything else as
// Terra. The lookup never fails — every robot gets a face.
export function agentBotAvatarLook(provider, model) {
  const wantedModel = String(model || "").toLowerCase();
  for (const [name, look] of Object.entries(WORLD_AGENT_BOT_AVATAR_LOOKS)) {
    if (wantedModel.includes(name)) return { name, ...look };
  }
  const wantedProvider = String(provider || "").toLowerCase();
  const fallback = wantedProvider.startsWith("claude")
    ? "sonnet"
    : wantedProvider === "codex"
      ? "sol"
      : wantedProvider === "openai"
        ? "luna"
        : "terra";
  return { name: fallback, ...WORLD_AGENT_BOT_AVATAR_LOOKS[fallback] };
}

// Repaints ForkBot's chest screen in place. `state` is null for the idle
// wordmark, or { message, thinking, dotPhase } while ForkBot is answering a
// mention: the echoed line renders in quotes and, once thinking starts, a row
// of pulsing dots runs beneath it until the reply is broadcast.
export function drawForkbotScreen(context, canvas, state) {
  const width = canvas.width;
  const height = canvas.height;
  context.clearRect(0, 0, width, height);
  roundedRect(context, 3, 3, width - 6, height - 6, 20);
  context.fillStyle = "#06181d";
  context.fill();
  context.strokeStyle = "#4dc8e8";
  context.lineWidth = 5;
  context.stroke();
  context.textAlign = "center";
  context.textBaseline = "middle";
  if (!state) {
    context.fillStyle = "#2fa5c4";
    context.font = '700 44px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("FORKBOT", width / 2, height / 2);
    return;
  }
  context.fillStyle = "#e9fbff";
  context.font = '600 34px "ForkMesh Favorit", system-ui, sans-serif';
  const words = `“${state.message}”`.split(/\s+/).filter(Boolean);
  const lines = [""];
  for (const word of words) {
    const current = lines[lines.length - 1];
    const candidate = current ? `${current} ${word}` : word;
    if (!current || context.measureText(candidate).width <= width - 56) {
      lines[lines.length - 1] = candidate;
    } else if (lines.length < 3) {
      lines.push(word);
    } else {
      lines[2] += "…";
      break;
    }
  }
  const textCenter = state.thinking ? height / 2 - 26 : height / 2;
  lines.forEach((line, index) => {
    context.fillText(
      line,
      width / 2,
      textCenter + (index - (lines.length - 1) / 2) * 40,
      width - 56,
    );
  });
  if (state.thinking) {
    for (let dot = 0; dot < 3; dot += 1) {
      const active = dot === state.dotPhase % 3;
      context.fillStyle = active ? "#9ef7c6" : "#2c5a66";
      context.beginPath();
      context.arc(
        width / 2 + (dot - 1) * 44,
        height - 40,
        active ? 13 : 9,
        0,
        Math.PI * 2,
      );
      context.fill();
    }
  }
}

export function createAgentRobot(THREE, bot, id) {
  const group = new THREE.Group();
  group.name = `verified-agent-object:${id}`;
  group.userData.infrastructureKind = "automated-agent";
  group.userData.agentId = id;
  group.userData.verified = bot?.verified === true;
  const verified = bot?.verified === true;
  const core = new THREE.Mesh(
    resources.primitive(THREE, "OctahedronGeometry", 0.62, 0),
    makeMaterial(THREE, verified ? "#d5b6ff" : "#89929b", {
      emissive: verified ? "#6f4da0" : "#32383d",
      emissiveIntensity: verified ? 0.85 : 0.2,
      metalness: 0.42,
      roughness: 0.28,
    }),
  );
  core.position.y = 1.35;
  group.add(core);
  const lens = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.17, 12, 10),
    makeMaterial(THREE, verified ? "#9ef7c6" : "#91a39a", {
      emissive: verified ? "#39c783" : "#3f4a45",
      emissiveIntensity: verified ? 1.25 : 0.3,
    }),
  );
  lens.position.set(0, 1.36, -0.55);
  group.add(lens);
  for (const side of [-1, 1]) {
    const rotor = new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", 0.34, 0.055, 7, 20),
      makeSharedMaterial(THREE, "#77d9ff", {
        emissive: "#237a99",
        emissiveIntensity: 0.72,
      }),
    );
    rotor.position.set(side * 0.8, 1.35, 0);
    rotor.rotation.x = Math.PI / 2;
    group.add(rotor);
  }
  const label = makeLabelSprite(
    THREE,
    String(bot.name).slice(0, 22),
    `${verified ? "verified" : "unverified"} · ${String(bot.type).slice(0, 20)}`,
    verified ? "#d5b6ff" : "#91a39a",
  );
  label.scale.set(2.7, 0.9, 1);
  label.position.y = 2.7;
  group.add(label);
  group.userData.core = core;
  setShadows(group);
  return group;
}

export function desktopAgentBallMaterial(THREE, shell) {
  return makeSharedMaterial(THREE, "#14272c", {
    metalness: 0.72,
    roughness: 0.24,
    emissive: shell,
    emissiveIntensity: 0.2,
  });
}

export function desktopAgentBodyMaterial(THREE, shell) {
  return makeSharedMaterial(THREE, shell, {
    metalness: 0.5,
    roughness: 0.28,
    emissive: shell,
    emissiveIntensity: 0.26,
  });
}

export function createDesktopAgentRobot(THREE, session, textureLoader) {
  const appearance = desktopAgentModel(session?.model, session?.provider);
  const status = desktopAgentStatus(session?.status);
  const group = new THREE.Group();
  group.name = `desktop-agent:${String(session?.id || "unknown")}`;
  group.userData.infrastructureKind = "desktop-agent";

  const ball = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.52, 22, 15),
    desktopAgentBallMaterial(THREE, appearance.shell),
  );
  ball.position.y = 0.56;
  group.add(ball);

  const body = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.55, 0.62, 0.82, 20),
    desktopAgentBodyMaterial(THREE, appearance.shell),
  );
  body.position.y = 1.15;
  group.add(body);

  const portrait = new THREE.Sprite(
    new THREE.SpriteMaterial({
      map: textureLoader.load(appearance.icon),
      transparent: true,
      toneMapped: false,
      depthWrite: false,
    }),
  );
  portrait.name = "desktop-agent-model-symbol";
  portrait.scale.set(0.72, 0.72, 1);
  portrait.position.set(0, 1.16, 0.58);
  group.add(portrait);

  const mast = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.035, 0.035, 0.42, 8),
    makeSharedMaterial(THREE, "#bed9d2", { metalness: 0.65, roughness: 0.28 }),
  );
  mast.position.y = 1.78;
  group.add(mast);
  const light = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.12, 14, 10),
    makeMaterial(THREE, status.color, {
      emissive: status.color,
      emissiveIntensity: 2,
    }),
  );
  light.position.y = 2.02;
  group.add(light);

  group.userData.rollingBall = ball;
  group.userData.body = body;
  group.userData.portrait = portrait;
  group.userData.statusLight = light;
  group.userData.modelKey = `${appearance.label}|${appearance.icon}`;
  group.userData.label = null;
  setShadows(group);
  portrait.castShadow = false;
  portrait.receiveShadow = false;
  return group;
}
