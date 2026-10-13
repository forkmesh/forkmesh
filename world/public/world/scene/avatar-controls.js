import { wrapCanvasText } from "./boards.js";
import { canvasTexture, roundedRect } from "./canvas.js";
import { cloneSharedPlane } from "./materials.js";
import { clamp, disposeObject3D } from "./object-utils.js";
import * as resources from "./resources.js";

export const WORLD_MODERATION_HANDLE_PATTERN = /^[a-f0-9]{64}$/;
// Organization and account names share the node-name grammar the worker
// enforces. The team plaque only ever carries names, never a session secret.
const WORLD_ORG_NAME_PATTERN = /^[a-z0-9][a-z0-9-]{0,39}$/;

export function chatBubbleTexture(THREE, name, text) {
  return canvasTexture(THREE, 768, 256, (context) => {
    context.clearRect(0, 0, 768, 256);
    roundedRect(context, 4, 4, 760, 216, 26);
    context.fillStyle = "rgba(6,17,14,0.94)";
    context.fill();
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 4;
    context.stroke();
    // Speech-bubble tail pointing down toward the speaker's head.
    context.beginPath();
    context.moveTo(354, 214);
    context.lineTo(384, 250);
    context.lineTo(414, 214);
    context.closePath();
    context.fillStyle = "rgba(6,17,14,0.94)";
    context.fill();
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#9ef7c6";
    context.font = '700 26px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(name || "visitor").toUpperCase().slice(0, 24), 42, 50);
    context.fillStyle = "#f1fff6";
    context.font = '600 38px "ForkMesh Favorit", system-ui, sans-serif';
    const words = String(text || "").split(/\s+/).filter(Boolean);
    const lines = [""];
    for (const word of words) {
      const current = lines[lines.length - 1];
      const candidate = current ? `${current} ${word}` : word;
      if (!current || context.measureText(candidate).width <= 680) {
        lines[lines.length - 1] = candidate;
      } else if (lines.length < 3) {
        lines.push(word);
      } else {
        lines[2] += "…";
        break;
      }
    }
    lines.forEach((line, index) => {
      context.fillText(line, 42, 102 + index * 50, 680);
    });
  });
}

function moderationControlTexture(THREE, title, subtitle, color) {
  return canvasTexture(THREE, 512, 160, (context) => {
    context.clearRect(0, 0, 512, 160);
    roundedRect(context, 4, 4, 504, 152, 18);
    context.fillStyle = "rgba(35,8,12,0.96)";
    context.fill();
    context.strokeStyle = color;
    context.lineWidth = 8;
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = "#fff5f6";
    context.font = '800 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(title, 256, 58);
    context.fillStyle = color;
    context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(subtitle, 256, 113);
  });
}

export function sanitizedModerationHandles(remote, isAdmin) {
  const peerId = String(remote?.id || "");
  if (
    isAdmin !== true ||
    !peerId ||
    /^(?:inactive|local|node|bot):/.test(peerId) ||
    remote?.persistedInactive === true ||
    remote?.accountStatus === "Verified bot" ||
    !remote?.moderationHandles ||
    typeof remote.moderationHandles !== "object" ||
    Array.isArray(remote.moderationHandles)
  ) {
    return {};
  }
  const handles = {};
  for (const targetType of ["ip", "agent"]) {
    const handle = remote.moderationHandles[targetType];
    if (
      typeof handle === "string" &&
      WORLD_MODERATION_HANDLE_PATTERN.test(handle)
    ) {
      handles[targetType] = handle;
    }
  }
  return handles;
}

export function sanitizedAdminGuestNetwork(remote, isAdmin) {
  if (
    isAdmin !== true ||
    String(remote?.accountStatus || "Guest") !== "Guest" ||
    !remote?.adminGuestNetwork ||
    typeof remote.adminGuestNetwork !== "object" ||
    Array.isArray(remote.adminGuestNetwork)
  ) {
    return {};
  }
  const ipAddress = String(
    remote.adminGuestNetwork.ipAddress || "",
  ).slice(0, 64);
  const userAgent = String(
    remote.adminGuestNetwork.userAgent || "",
  ).slice(0, 1024);
  return {
    ipAddress: /^[0-9a-f:.]{2,64}$/i.test(ipAddress) ? ipAddress : "",
    userAgent:
      userAgent &&
      !/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/.test(userAgent)
        ? userAgent
        : "",
  };
}

function guestNetworkTexture(THREE, label, value, color, height = 220) {
  return canvasTexture(THREE, 1024, height, (context) => {
    context.clearRect(0, 0, 1024, height);
    roundedRect(context, 4, 4, 1016, height - 8, 22);
    context.fillStyle = "rgba(4,16,14,0.97)";
    context.fill();
    context.strokeStyle = color;
    context.lineWidth = 9;
    context.stroke();
    context.textAlign = "left";
    context.textBaseline = "top";
    context.fillStyle = color;
    context.font = '800 28px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(`${label} · CLICK TO COPY`, 28, 20);
    context.fillStyle = "#effff7";
    context.font = `${
      label === "FULL USER AGENT" ? 19 : 34
    }px "ForkMesh Mono", ui-monospace, monospace`;
    wrapCanvasText(
      context,
      value || "Not reported",
      28,
      label === "FULL USER AGENT" ? 62 : 66,
      968,
      label === "FULL USER AGENT" ? 24 : 38,
      label === "FULL USER AGENT" ? 16 : 2,
    );
  });
}

export function createAvatarModerationControls(THREE, handles, guestNetwork = {}) {
  const group = new THREE.Group();
  group.name = "forkmesh-world-moderation-controls";
  const specs = [
    {
      targetType: "ip",
      title: "TEMP BLOCK IP",
      subtitle: "ROTATING IP TOKEN · 1 HOUR",
      color: "#ff9ca4",
      y: 2.43,
    },
    {
      targetType: "agent",
      title: "TEMP BLOCK AGENT",
      subtitle: "BROWSER AGENT · 1 HOUR",
      color: "#ffc279",
      y: 2.02,
    },
  ];
  specs.forEach((spec) => {
    if (!handles[spec.targetType]) return;
    const control = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 1.02, 0.34),
      resources.uniqueMaterial(THREE, {
        map: moderationControlTexture(
          THREE,
          spec.title,
          spec.subtitle,
          spec.color,
        ),
        transparent: false,
        depthWrite: true,
      }, "MeshBasicMaterial"),
    );
    control.name = `world-moderation-${spec.targetType}-control`;
    // Avatar fronts face -Z; positive Z places this single-sided control on
    // the back, so it cannot be selected through the avatar from the front.
    control.position.set(0, spec.y, 0.321);
    control.userData.worldModerationControl = spec.targetType;
    control.renderOrder = 3;
    group.add(control);
  });
  for (const spec of [
    {
      field: "ipAddress",
      label: "GUEST IP",
      color: "#80e8ff",
      y: 1.63,
      width: 1.18,
      height: 0.3,
      textureHeight: 220,
    },
    {
      field: "userAgent",
      label: "FULL USER AGENT",
      color: "#b8a8ff",
      y: 1.12,
      width: 1.18,
      height: 0.68,
      textureHeight: 600,
    },
  ]) {
    const value = String(guestNetwork[spec.field] || "");
    if (!value) continue;
    const control = new THREE.Mesh(
      new THREE.PlaneGeometry(spec.width, spec.height),
      resources.uniqueMaterial(THREE, {
        map: guestNetworkTexture(
          THREE,
          spec.label,
          value,
          spec.color,
          spec.textureHeight,
        ),
        depthWrite: true,
      }, "MeshBasicMaterial"),
    );
    control.position.set(0, spec.y, 0.322);
    control.userData.worldGuestCopy = spec.field;
    control.renderOrder = 3;
    group.add(control);
  }
  return group;
}

// The organization team plaque the app layer hands down for members of an
// organization the viewer owns or administers. Everything here is a name the
// roster already publishes to org members plus two counts; the plaque never
// carries a role decision — the worker re-checks the caller's role on write.
export function sanitizedOrgTeamAssignment(remote) {
  const peerId = String(remote?.id || "");
  const assignment = remote?.orgTeam;
  if (
    !peerId ||
    /^(?:node|bot):/.test(peerId) ||
    remote?.accountStatus === "Verified bot" ||
    String(remote?.accountStatus || "Guest") === "Guest" ||
    !assignment ||
    typeof assignment !== "object" ||
    Array.isArray(assignment)
  ) {
    return null;
  }
  const org = String(assignment.org || "").toLowerCase();
  const member = String(assignment.member || "").toLowerCase();
  if (
    !WORLD_ORG_NAME_PATTERN.test(org) ||
    !WORLD_ORG_NAME_PATTERN.test(member)
  ) {
    return null;
  }
  const total = Math.max(0, Math.min(99, Number(assignment.total) || 0));
  if (!total) return null;
  return {
    org,
    member,
    canManage: assignment.canManage === true,
    assigned: Math.max(0, Math.min(total, Number(assignment.assigned) || 0)),
    total,
    teams: (Array.isArray(assignment.teams) ? assignment.teams : [])
      .map((team) => String(team || "").toLowerCase())
      .filter((team) => WORLD_ORG_NAME_PATTERN.test(team))
      .slice(0, 6),
  };
}

function avatarTeamBadgeTexture(THREE, team, index) {
  const colors = ["#9ef7c6", "#80e8ff", "#ffc279", "#d8b7ff", "#ff9ca4"];
  const accent = colors[index % colors.length];
  return canvasTexture(THREE, 512, 176, (context) => {
    roundedRect(context, 4, 4, 504, 168, 42);
    context.fillStyle = "rgba(5,20,17,0.97)";
    context.fill();
    context.strokeStyle = accent;
    context.lineWidth = 10;
    context.stroke();
    context.fillStyle = accent;
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '900 54px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(team || "").toUpperCase(), 256, 88, 452);
  });
}

// Personal activity awards mount on the right sleeve, leaving organization
// teams on the left. These are fixed public facts from the member directory,
// never claims copied out of a guest presence frame.
const AVATAR_ACHIEVEMENT_BADGE_DEFINITIONS = Object.freeze({
  qa: { label: "QA", color: "#75d9ff" },
  coder: { label: "CODE", color: "#9ef7c6" },
  discord: { label: "DISCORD", color: "#8fa8ff" },
  poster: { label: "POST", color: "#ffc779" },
  chat: { label: "CHAT", color: "#ec9cff" },
});
const AVATAR_ACHIEVEMENT_BADGE_ORDER = Object.freeze(
  Object.keys(AVATAR_ACHIEVEMENT_BADGE_DEFINITIONS),
);
// A member circle can contain hundreds of avatars. The five fixed cloth patch
// textures/materials are therefore shared rather than repainting an identical
// canvas for each person; disposeObject3D honors this shared-resource marker.
const avatarAchievementBadgeMaterials = new WeakMap();

function avatarAchievementBadgeSlugs(identity) {
  if (String(identity?.accountStatus || "Guest") === "Guest") return [];
  const present = new Set(
    (Array.isArray(identity?.achievementBadges)
      ? identity.achievementBadges
      : [])
      .map((value) => String(value || "").trim().toLowerCase())
      .filter((slug) => AVATAR_ACHIEVEMENT_BADGE_DEFINITIONS[slug]),
  );
  return AVATAR_ACHIEVEMENT_BADGE_ORDER.filter((slug) => present.has(slug));
}

function avatarAchievementBadgeTexture(THREE, definition) {
  return canvasTexture(THREE, 320, 112, (context) => {
    context.clearRect(0, 0, 320, 112);
    roundedRect(context, 5, 5, 310, 102, 28);
    context.fillStyle = "rgba(5, 20, 17, 0.97)";
    context.fill();
    context.strokeStyle = definition.color;
    context.lineWidth = 8;
    context.stroke();
    context.fillStyle = definition.color;
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '900 46px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(definition.label, 160, 58, 284);
  });
}

function avatarAchievementBadgeMaterial(THREE, slug) {
  let materials = avatarAchievementBadgeMaterials.get(THREE);
  if (!materials) {
    materials = new Map();
    avatarAchievementBadgeMaterials.set(THREE, materials);
  }
  let material = materials.get(slug);
  if (material) return material;
  const definition = AVATAR_ACHIEVEMENT_BADGE_DEFINITIONS[slug];
  material = resources.uniqueMaterial(THREE, {
    map: avatarAchievementBadgeTexture(THREE, definition),
    transparent: true,
    depthWrite: true,
    toneMapped: false,
  }, "MeshBasicMaterial");
  material.userData.forkmeshSharedResource = true;
  materials.set(slug, material);
  return material;
}

function createAvatarAchievementBadges(THREE, slugs) {
  const group = new THREE.Group();
  group.name = "forkmesh-avatar-right-arm-achievement-badges";
  slugs.forEach((slug, index) => {
    const badge = cloneSharedPlane(
      THREE,
      0.275,
      0.092,
      avatarAchievementBadgeMaterial(THREE, slug),
    );
    // The anatomical right arm sits at group x +0.73, so its outward sleeve
    // face is local +X. Stack compact cloth patches down that upper arm.
    badge.position.set(0.148, 0.47 - index * 0.105, 0);
    badge.rotation.y = Math.PI / 2;
    badge.renderOrder = 3;
    badge.userData.achievementBadge = slug;
    group.add(badge);
  });
  return group;
}

function removeAvatarAchievementBadges(avatar) {
  const badges = avatar?.userData?.achievementBadges;
  if (!badges) return;
  badges.parent?.remove(badges);
  disposeObject3D(badges);
  delete avatar.userData.achievementBadges;
  delete avatar.userData.achievementBadgeKey;
}

export function syncAvatarAchievementBadges(THREE, avatar, identity) {
  if (!avatar?.userData) return;
  const slugs = avatarAchievementBadgeSlugs(identity);
  const key = slugs.join("|");
  if (
    avatar.userData.achievementBadgeKey === key &&
    avatar.userData.achievementBadges
  ) {
    return;
  }
  removeAvatarAchievementBadges(avatar);
  avatar.userData.achievementBadgeKey = key;
  if (!slugs.length || !avatar.userData.rightArm) return;
  const badges = createAvatarAchievementBadges(THREE, slugs);
  avatar.userData.rightArm.add(badges);
  avatar.userData.achievementBadges = badges;
}

export function createAvatarTeamBadges(THREE, assignment) {
  const group = new THREE.Group();
  group.name = "forkmesh-avatar-left-arm-team-badges";
  const organizationLevel = clamp(
    Math.ceil(
      (Math.max(1, Number(assignment.assigned) || 1) /
        Math.max(1, Number(assignment.total) || 1)) *
        5,
    ),
    1,
    5,
  );
  const stars = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 0.28, 0.075),
    resources.uniqueMaterial(THREE, {
      map: canvasTexture(THREE, 512, 136, (context) => {
        context.clearRect(0, 0, 512, 136);
        context.fillStyle = "#ffd33d";
        context.textAlign = "center";
        context.textBaseline = "middle";
        context.font = '900 96px "ForkMesh Favorit", system-ui, sans-serif';
        context.fillText("★".repeat(organizationLevel), 256, 70, 476);
      }),
      transparent: true,
      depthWrite: true,
    }, "MeshBasicMaterial"),
  );
  stars.name = "forkmesh-avatar-organization-level-stars";
  stars.position.set(-0.151, 0.505, 0);
  stars.rotation.y = -Math.PI / 2;
  stars.renderOrder = 4;
  group.add(stars);
  assignment.teams.slice(0, 4).forEach((team, index) => {
    const badge = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.26, 0.09),
      resources.uniqueMaterial(THREE, {
        map: avatarTeamBadgeTexture(THREE, team, index),
        depthWrite: true,
      }, "MeshBasicMaterial"),
    );
    // The anatomical left arm sits at group x -0.73, so its outward face is
    // local -X. Mount the marks there — sized to the 0.32-deep sleeve so they
    // never overhang it — stacked down the upper arm like cloth patches.
    badge.position.set(-0.148, 0.36 - index * 0.105, 0);
    badge.rotation.y = -Math.PI / 2;
    badge.renderOrder = 3;
    group.add(badge);
  });
  return group;
}

function orgTeamControlTexture(THREE, title, subtitle) {
  return canvasTexture(THREE, 512, 160, (context) => {
    context.clearRect(0, 0, 512, 160);
    roundedRect(context, 4, 4, 504, 152, 18);
    context.fillStyle = "rgba(6,26,22,0.96)";
    context.fill();
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 8;
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = "#eafff4";
    context.font = '800 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(title, 256, 58, 456);
    context.fillStyle = "#9ef7c6";
    context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(subtitle, 256, 113, 456);
  });
}

export function createAvatarOrgTeamControl(THREE, assignment) {
  const group = new THREE.Group();
  group.name = "forkmesh-world-org-team-control";
  const control = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 1.02, 0.34),
    resources.uniqueMaterial(THREE, {
      map: orgTeamControlTexture(
        THREE,
        "ASSIGN TEAMS",
        `${assignment.org.toUpperCase()} · ${assignment.assigned}/${
          assignment.total
        } TEAMS`,
      ),
      transparent: false,
      depthWrite: true,
    }, "MeshBasicMaterial"),
  );
  control.name = "world-org-team-control";
  // Same back face as the moderation plaques: avatar fronts face -Z, so a
  // single-sided plane at positive Z cannot be clicked through the chest.
  control.position.set(0, 1.61, 0.321);
  control.userData.worldOrgTeamControl = true;
  control.renderOrder = 3;
  group.add(control);
  return group;
}

export const BATCH_TEXTURE_SLOTS = [
  "map",
  "alphaMap",
  "aoMap",
  "bumpMap",
  "displacementMap",
  "emissiveMap",
  "envMap",
  "lightMap",
  "metalnessMap",
  "normalMap",
  "roughnessMap",
];
