import { hashNumber } from "../../world-avatar-face.js";
import { landmarkById } from "../../world-data.js";
import { FORKBOT_PEER_ID } from "../agent-bots.js";
import { chatBubbleTexture } from "../avatar-controls.js";
import {
  AVATAR_HANDSHAKE_DURATION_MS,
  startAvatarHandshake,
  startAvatarHudAction,
  startAvatarWave,
} from "../avatars.js";
import { canvasTexture, roundedRect } from "../canvas.js";
import { PLAYER_SUPER_JUMP_VELOCITY } from "../gameplay-constants.js";
import { makeLabelSprite } from "../labels.js";
import { makeMaterial, makeSharedMaterial } from "../materials.js";
import { createServedVisitorFigure } from "../mirror-nodes.js";
import { removeGeneratedLayer } from "../object-utils.js";
import {
  makeRepositoryContributorIcon,
  repositorySizeMapSegments,
  repositoryWedgeGeometry,
} from "../repositories.js";
import {
  applyRewardTreasury,
  rewardPoolMirrorCountTexture,
} from "../treasury.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  identity,
  reducedMotion,
  ctx,
  fns,
}) {
  let mirrorCountShown = "";


  function updateRepositoryGraph(entries = [], entities = []) {
    const district = ctx.landmarkObjects.get("repositories");
    const meshes = district?.userData?.fileMeshes || [];
    const portal = district?.userData?.portal;
    const safeEntries = Array.isArray(entries) ? entries.slice(0, meshes.length) : [];
    const safeEntities = Array.isArray(entities)
      ? entities
          .filter(
            (entity) =>
              entity &&
              ["contributor", "issue", "issue-collection", "pull-request",
                "pull-request-collection"].includes(String(entity.kind || "")),
          )
          .slice(0, 32)
      : [];
    // Contributors occupy their own smaller INNER portrait ring on the
    // selected repository face. This is deliberately separate from the large
    // outer Mastodon-follower ring so the two relationships are immediately
    // distinguishable.
    const activePortal = ctx.repositoryPortals.get(
      String(ctx.world.userData.repositoryActiveKey || ""),
    )?.group;
    const previousContributorOrbit =
      activePortal?.getObjectByName?.("repository-contributor-inner-ring");
    removeGeneratedLayer(
      activePortal,
      previousContributorOrbit,
      ctx.interactive,
    );
    if (activePortal) {
      const contributors = safeEntities
        .filter((entity) => String(entity?.kind || "") === "contributor")
        .slice(0, 8);
      if (contributors.length) {
        const contributorOrbit = new THREE.Group();
        contributorOrbit.name = "repository-contributor-inner-ring";
        contributorOrbit.position.set(0, 0.55, 0.17);
        contributors.forEach((contributor, contributorIndex) => {
          const icon = makeRepositoryContributorIcon(THREE, contributor);
          const angle =
            Math.PI * 1.04 -
            (contributorIndex /
              Math.max(1, contributors.length - 1)) *
              Math.PI *
              1.08;
          const radius = 2.7;
          icon.position.set(
            Math.cos(angle) * radius,
            Math.sin(angle) * radius,
            0,
          );
          contributorOrbit.add(icon);
          ctx.interactive.push(icon);
        });
        // Like the follower orbit, this must survive the portal-face swap that
        // displays the repository size map.
        activePortal.add(contributorOrbit);
      }
    }
    const maxSize = Math.max(
      1,
      ...safeEntries.map((entry) => Math.max(0, Number(entry?.size) || 0)),
    );
    const byPath = new Map(
      safeEntries.map((entry, index) => [String(entry.path || ""), index]),
    );
    const edgeIndexes = [];
    const connected = new Set();
    safeEntries.forEach((entry, sourceIndex) => {
      const dependencies = Array.isArray(entry.dependencies)
        ? entry.dependencies
        : [];
      dependencies.forEach((dependency) => {
        const targetIndex = byPath.get(String(dependency));
        if (targetIndex === undefined || targetIndex === sourceIndex) return;
        const key = [sourceIndex, targetIndex].sort((a, b) => a - b).join(":");
        if (connected.has(key)) return;
        connected.add(key);
        edgeIndexes.push([sourceIndex, targetIndex]);
      });
    });
    // A small deterministic force layout makes the actual import graph legible:
    // spring forces pull adjacent files together, while repulsion prevents one
    // dense dependency component from collapsing into a single icon.
    const layout = safeEntries.map((entry, index) => {
      const angle = (index / Math.max(1, safeEntries.length)) * Math.PI * 2;
      const directoryBias =
        String(entry.path || "")
          .split("/")
          .slice(0, -1)
          .join("")
          .split("")
          .reduce((total, character) => total + character.charCodeAt(0), 0) %
        13;
      return {
        x: Math.cos(angle) * (1.45 + (directoryBias % 3) * 0.12),
        z: Math.sin(angle) * (0.52 + (directoryBias % 2) * 0.08),
      };
    });
    for (let iteration = 0; iteration < 36; iteration += 1) {
      const forces = layout.map(() => ({ x: 0, z: 0 }));
      for (let left = 0; left < layout.length; left += 1) {
        for (let right = left + 1; right < layout.length; right += 1) {
          let dx = layout[right].x - layout[left].x;
          let dz = layout[right].z - layout[left].z;
          const distanceSquared = Math.max(0.04, dx * dx + dz * dz);
          const distance = Math.sqrt(distanceSquared);
          dx /= distance;
          dz /= distance;
          const push = 0.018 / distanceSquared;
          forces[left].x -= dx * push;
          forces[left].z -= dz * push;
          forces[right].x += dx * push;
          forces[right].z += dz * push;
        }
      }
      edgeIndexes.forEach(([sourceIndex, targetIndex]) => {
        let dx = layout[targetIndex].x - layout[sourceIndex].x;
        let dz = layout[targetIndex].z - layout[sourceIndex].z;
        const distance = Math.max(0.001, Math.hypot(dx, dz));
        dx /= distance;
        dz /= distance;
        const pull = (distance - 0.72) * 0.075;
        forces[sourceIndex].x += dx * pull;
        forces[sourceIndex].z += dz * pull;
        forces[targetIndex].x -= dx * pull;
        forces[targetIndex].z -= dz * pull;
      });
      layout.forEach((point, index) => {
        point.x = THREE.MathUtils.clamp(
          point.x + forces[index].x * 0.65 - point.x * 0.008,
          -2.1,
          2.1,
        );
        point.z = THREE.MathUtils.clamp(
          point.z + forces[index].z * 0.65 - point.z * 0.006,
          -0.85,
          0.85,
        );
      });
    }
    meshes.forEach((mesh, index) => {
      const entry = safeEntries[index];
      mesh.visible = Boolean(entry);
      if (!entry) return;
      const ratio = Math.sqrt(Math.max(0, Number(entry.size) || 0) / maxSize);
      const scale = entry.type === "directory" ? 1.55 : 0.72 + ratio * 1.5;
      mesh.scale.setScalar(scale);
      mesh.userData.path = String(entry.path || entry.name || "").slice(0, 160);
      mesh.userData.kind = entry.type === "directory" ? "directory" : "file";
      const architecturalLayers = [
        "directory",
        "module",
        "package",
        "database-model",
        "service",
        "api",
        "test",
      ];
      const architecturalLayer = Math.max(
        0,
        architecturalLayers.indexOf(String(entry.role || "module")),
      );
      const depth = Math.min(
        4,
        Math.max(
          String(entry.path || "").split("/").length - 1,
          Number(entry.dependencyDepth) || 0,
        ),
      );
      mesh.userData.layer = depth;
      mesh.userData.radius = 1.0 + depth * 0.42 + (index % 3) * 0.16;
      mesh.userData.angle =
        (architecturalLayer / architecturalLayers.length) * Math.PI * 2 +
        (index % 4) * 0.11;
      const point = layout[index] || { x: 0, z: 0 };
      mesh.userData.layoutPosition = {
        x: point.x,
        y: 2.65 + depth * 0.18 + architecturalLayer * 0.035,
        z: point.z,
      };
      mesh.position.set(
        mesh.userData.layoutPosition.x,
        mesh.userData.layoutPosition.y,
        mesh.userData.layoutPosition.z,
      );
      if (mesh.userData.fileColor) {
        mesh.material = makeSharedMaterial(THREE, mesh.userData.fileColor, {
          emissive: mesh.userData.fileColor,
          emissiveIntensity:
            entry.frequency === "high" ? 0.9 : entry.frequency === "medium" ? 0.5 : 0.18,
        });
      }
    });
    const previousEntityLayer = portal?.userData?.repositoryEntityLayer;
    if (previousEntityLayer && portal) {
      previousEntityLayer.traverse((child) => {
        const interactiveIndex = ctx.interactive.indexOf(child);
        if (interactiveIndex >= 0) ctx.interactive.splice(interactiveIndex, 1);
        child.geometry?.dispose?.();
        child.material?.map?.dispose?.();
        child.material?.dispose?.();
      });
      portal.remove(previousEntityLayer);
      portal.userData.repositoryEntityLayer = null;
      portal.userData.repositoryEntityMeshes = [];
    }
    if (portal && safeEntities.length) {
      const layer = new THREE.Group();
      layer.name = "repository-entity-layer";
      const entityMeshes = [];
      const entityColors = {
        contributor: "#9ef7c6",
        issue: "#f7c96b",
        "issue-collection": "#f7c96b",
        "pull-request": "#d5b6ff",
        "pull-request-collection": "#d5b6ff",
      };
      safeEntities.forEach((entity, index) => {
        const kind = String(entity.kind || "");
        let geometry;
        if (kind === "contributor") {
          geometry = resources.primitive(THREE, "SphereGeometry", 0.19, 14, 10);
        } else if (kind.startsWith("issue")) {
          geometry = resources.primitive(THREE, "OctahedronGeometry", 0.22, 0);
        } else {
          geometry = resources.primitive(THREE, "TorusGeometry", 0.18, 0.065, 8, 20);
        }
        const color = entityColors[kind] || "#ffffff";
        const mesh = new THREE.Mesh(
          geometry,
          makeMaterial(THREE, color, {
            emissive: color,
            emissiveIntensity: 0.7,
            metalness: 0.18,
            roughness: 0.4,
          }),
        );
        const angle =
          (index / Math.max(1, safeEntities.length)) * Math.PI * 2 +
          (kind === "contributor" ? 0 : kind.startsWith("issue") ? 0.35 : 0.7);
        const radius =
          kind === "contributor" ? 2.55 : kind.startsWith("issue") ? 2.9 : 3.25;
        mesh.position.set(
          Math.cos(angle) * radius,
          2.5 + (index % 4) * 0.22,
          Math.sin(angle) * 0.78,
        );
        mesh.name = `repository-entity-${kind}-${index}`;
        mesh.userData.landmark = "repositories";
        mesh.userData.graphNode = {
          id: String(entity.id || "").slice(0, 96),
          kind,
          label: String(entity.label || "").slice(0, 100),
          detail: String(entity.detail || "").slice(0, 240),
          href: String(entity.href || "").startsWith("/")
            ? String(entity.href).slice(0, 400)
            : "",
        };
        layer.add(mesh);
        ctx.interactive.push(mesh);
        entityMeshes.push(mesh);
        const label = makeLabelSprite(
          THREE,
          String(entity.label || "").slice(0, 22),
          kind.replaceAll("-", " "),
          color,
        );
        label.name = `repository-entity-label-${kind}-${index}`;
        label.scale.set(1.45, 0.48, 1);
        label.position.copy(mesh.position);
        label.position.y += 0.48;
        layer.add(label);
      });
      portal.add(layer);
      portal.userData.repositoryEntityLayer = layer;
      portal.userData.repositoryEntityMeshes = entityMeshes;
    }
    portal?.scale.setScalar(safeEntries.length ? 1.05 : 1);
  }

  function playEmote(peerId, emote, local = false) {
    const avatar = local
      ? ctx.player
      : ctx.remotePlayers.get(peerId) || (peerId === identity.id ? ctx.player : null);
    if (!avatar) return;
    // A wave is the one emote with a body pose behind the floating glyph, and
    // it plays the same way whether the gesture came from this browser or off
    // the relay.
    if (emote === "wave") startAvatarWave(avatar);
    if (emote === "superjump" && local && ctx.officeSceneMode === "town") {
      fns.standUpFromBench();
      fns.dismountSwing({ relocate: false });
      fns.cancelDash();
      fns.removeRoofParachute();
      ctx.jumpQueued = false;
      ctx.jumpVelocity = PLAYER_SUPER_JUMP_VELOCITY;
      ctx.superJumping = true;
    } else if (
      [
        "jump",
        "superjump",
        "spin",
        "backflip",
        "dance",
        "float",
        "wobble",
        "sparkle",
      ].includes(emote)
    ) {
      startAvatarHudAction(avatar, emote);
    }
    // The pose above stays (it is gameplay); only the floating glyph sprite
    // is part of the toggleable bubble/emote element.
    if (!fns.worldElementEnabled("chat-bubbles")) return;
    const glyphs = {
      wave: "WAVE",
      idea: "IDEA ✦",
      celebrate: "NICE ★",
      jump: "BOING ↑",
      superjump: "SUPER JUMP ⇈",
      spin: "WHEE ↻",
      backflip: "FLIP!",
      dance: "DANCE ♫",
      float: "UP WE GO ☁",
      wobble: "WOBBLE 〰",
      sparkle: "SPARKLE ✦",
    };
    const sprite = makeLabelSprite(
      THREE,
      glyphs[emote] || "HELLO",
      "public emote",
      emote === "celebrate"
        ? "#f7c96b"
        : emote === "idea"
          ? "#d5b6ff"
          : "#9ef7c6",
    );
    sprite.scale.set(1.8, 0.6, 1);
    ctx.world.add(sprite);
    ctx.emoteSprites.push({
      sprite,
      avatar,
      startedAt: performance.now(),
      duration: 1900,
    });
  }

  function avatarForPeerId(peerId) {
    const id = String(peerId || "");
    if (!id) return null;
    return ctx.remotePlayers.get(id) || (id === identity.id ? ctx.player : null);
  }

  // An accepted handshake: both avatars start the same pose on the same frame
  // and one glyph floats over the pair, so the greeting reads as a single
  // shared event rather than two unrelated gestures. Either half may be
  // missing here — a peer can be out of the room or not yet rendered — and the
  // visible half still shakes.
  function playHandshake(peerId, partnerId) {
    const first = avatarForPeerId(peerId);
    const second = avatarForPeerId(partnerId);
    if (!first && !second) return false;
    const startedAt = performance.now();
    if (first) startAvatarHandshake(first, startedAt);
    if (second) startAvatarHandshake(second, startedAt);
    const sprite = makeLabelSprite(
      THREE,
      "HANDSHAKE",
      "public greeting",
      "#9ef7c6",
    );
    sprite.scale.set(1.9, 0.6, 1);
    ctx.world.add(sprite);
    ctx.emoteSprites.push({
      sprite,
      avatar: first || second,
      startedAt,
      duration: AVATAR_HANDSHAKE_DURATION_MS,
    });
    return true;
  }

  function showAvatarChatBubble(avatar, text, options = {}) {
    const message = String(text || "").replace(/\s+/g, " ").trim().slice(0, 140);
    if (!avatar || !message) return false;
    if (!fns.worldElementEnabled("chat-bubbles")) return false;
    // One bubble per speaker: a rapid follow-up message replaces the first
    // instead of stacking on top of it.
    for (let index = ctx.emoteSprites.length - 1; index >= 0; index -= 1) {
      const existing = ctx.emoteSprites[index];
      if (existing.chat && existing.avatar === avatar) {
        ctx.world.remove(existing.sprite);
        existing.sprite.material?.map?.dispose?.();
        existing.sprite.material?.dispose?.();
        ctx.emoteSprites.splice(index, 1);
      }
    }
    const sprite = new THREE.Sprite(
      new THREE.SpriteMaterial({
        map: chatBubbleTexture(
          THREE,
          avatar.userData?.name || "visitor",
          message,
        ),
        transparent: true,
        depthTest: false,
        depthWrite: false,
      }),
    );
    sprite.renderOrder = 11;
    sprite.scale.set(6.6, 2.2, 1);
    sprite.userData.message = message;
    sprite.userData.officeReceptionGreeting =
      options.officeReception === true;
    ctx.world.add(sprite);
    ctx.emoteSprites.push({
      sprite,
      avatar,
      chat: true,
      startedAt: performance.now(),
      // Longer messages linger longer before fading out.
      duration: Math.min(14000, 10000 + message.length * 30),
      baseHeight: 5.2,
      rise: 0.5,
      fadeStart: 0.75,
    });
    return true;
  }

  function showChatBubble(peerId, text, local = false) {
    const avatar =
      local || peerId === identity.id
        ? ctx.player
        : peerId === FORKBOT_PEER_ID
          ? ctx.forkbot
          : fns.agentBotState(peerId)?.avatar ||
            ctx.remotePlayers.get(String(peerId || "")) ||
            ctx.loungeMembers.get(String(peerId || ""));
    if (!avatar) return false;
    // ForkBot's own line is the reply the thinking dots were waiting for.
    if (avatar === ctx.forkbot && ctx.forkbotExcitement) {
      fns.settleForkbot(performance.now());
    }
    const agentState = fns.agentBotState(peerId);
    if (agentState?.excitement) {
      agentState.excitement = null;
      agentState.target.copy(agentState.avatar.position);
      agentState.nextWanderAt = performance.now() + 9000;
    }
    return showAvatarChatBubble(avatar, text);
  }

  // Open-circle directory figures anchor member chat even when the account is
  // not represented by a live peer.
  function showMemberChatBubble(name, text) {
    const wanted = String(name || "").trim().toLowerCase();
    if (!wanted) return false;
    if (ctx.loungeMembers.has(`member:${wanted}`)) {
      return showChatBubble(`member:${wanted}`, text);
    }
    // The public room truncates asserted names to 16 characters, so a
    // truncated sender may only be a prefix of the member figure's name.
    if (wanted.length < 16) return false;
    for (const id of ctx.loungeMembers.keys()) {
      if (id.slice("member:".length).startsWith(wanted)) {
        return showChatBubble(id, text);
      }
    }
    return false;
  }

  // Repaints the fountain's treasury board with the public pool address QR
  // code and the balance reported by /api/accounts/central-fund.
  function updateRewardPool(state = {}) {
    const sign = ctx.landmarkObjects.get("fountain")?.userData?.treasurySign;
    if (!sign) return;
    applyRewardTreasury(THREE, sign, state);
  }

  // Repaints the tally hovering over the pool orb. The catalog refresh runs on
  // a timer and mirror health reorders the payload without changing these two
  // numbers, so a memo keeps the canvas from being rebuilt every pass.
  function setRewardPoolMirrorCount(total, online) {
    const sprite = ctx.landmarkObjects.get("fountain")?.userData?.mirrorCountSprite;
    if (!sprite) return;
    const count = Math.max(0, Math.min(9999, Math.round(Number(total) || 0)));
    const live = Math.max(0, Math.min(count, Math.round(Number(online) || 0)));
    // No cabinets means no catalog yet: stay hidden rather than claim zero.
    sprite.visible = count > 0;
    const key = `${count}|${live}`;
    if (mirrorCountShown === key) return;
    mirrorCountShown = key;
    sprite.material.map?.dispose?.();
    sprite.material.map = rewardPoolMirrorCountTexture(THREE, count, live);
    sprite.material.needsUpdate = true;
  }

  // Freshly pushed code announces itself: a tall light column rises from the
  // cabinet and a ground shockwave expands beyond the reward-pool node rings,
  // so the arrival reads from anywhere in the town — not just beside the rack.
  // Purely cosmetic and driven only by a verified commit change in the signed
  // mirror payload (see updateNetworkNodes), never by an unauthenticated frame.
  function spawnPushSurge(position) {
    if (!fns.worldElementEnabled("effects")) return;
    // Bound a burst of simultaneous publishes to a fixed effect budget.
    if (ctx.pushSurges.length >= 8) return;
    const group = new THREE.Group();
    group.name = "mirror-push-surge";
    const beam = new THREE.Mesh(
      resources.primitive(THREE, "CylinderGeometry", 0.55, 0.9, 56, 18, 1, true),
      resources.uniqueMaterial(THREE, {
        color: "#7dffb8",
        toneMapped: false,
        transparent: true,
        opacity: 0.85,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        side: THREE.DoubleSide,
        forceSinglePass: true,
      }, "MeshBasicMaterial"),
    );
    beam.position.y = 28;
    group.add(beam);
    const ring = new THREE.Mesh(
      resources.primitive(THREE, "RingGeometry", 0.9, 1, 48),
      resources.uniqueMaterial(THREE, {
        color: "#9ef7c6",
        toneMapped: false,
        transparent: true,
        opacity: 0.8,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        side: THREE.DoubleSide,
        forceSinglePass: true,
      }, "MeshBasicMaterial"),
    );
    ring.rotation.x = -Math.PI / 2;
    ring.position.y = 0.07;
    group.add(ring);
    group.position.set(position.x, 0, position.z);
    ctx.world.add(group);
    ctx.pushSurges.push({
      group,
      beam,
      ring,
      startedAt: performance.now(),
      duration: 4200,
    });
  }

  function repositoryLandingTexture(path, accent = "#3fb950") {
    return canvasTexture(THREE, 512, 144, (context, canvas) => {
      context.fillStyle = "#0d1117";
      roundedRect(context, 3, 3, canvas.width - 6, canvas.height - 6, 18);
      context.fill();
      context.strokeStyle = "#58a6ff";
      context.lineWidth = 7;
      context.stroke();
      context.fillStyle = accent;
      context.fillRect(24, 26, 12, canvas.height - 52);
      context.fillStyle = "#f0f6fc";
      context.font = '800 34px "ForkMesh Mono", ui-monospace, monospace';
      context.textAlign = "left";
      context.textBaseline = "middle";
      const label = String(path || "changed file");
      const compact =
        label.length > 26 ? `…${label.slice(-25)}` : label;
      context.fillText(compact, 58, canvas.height / 2, canvas.width - 78);
    });
  }

  function repositoryChangedFileSegment(segments, path) {
    let match = null;
    for (const segment of Array.isArray(segments) ? segments : []) {
      if (segment?.type === "summary" || !segment?.path) continue;
      if (path !== segment.path && !path.startsWith(`${segment.path}/`)) {
        continue;
      }
      if (!match || segment.depth > match.depth) match = segment;
    }
    return match;
  }

  function spawnRepositoryCodeLanding(pending = {}) {
    if (!fns.worldElementEnabled("effects")) return false;
    if (ctx.repositoryCodeLandings.length >= 4) return false;
    const owner = String(pending.owner || "").toLowerCase();
    const repo = String(pending.repo || "").toLowerCase();
    let portal =
      ctx.repositoryPortals.get(`${owner}/${repo}`) || null;
    if (!portal && repo) {
      portal =
        [...ctx.repositoryPortals.values()].find(
          (candidate) =>
            String(candidate?.name || "").toLowerCase() === repo,
        ) || null;
    }
    if (!portal?.group) return false;
    portal.group.updateWorldMatrix(true, true);
    const target = new THREE.Vector3();
    portal.group.getWorldPosition(target);
    target.y += 0.25;
    const files = (Array.isArray(pending.changedFiles)
      ? pending.changedFiles
      : []
    ).slice(0, 8);
    if (!files.length && portal.record?.sizeTree) {
      repositorySizeMapSegments(portal.record.sizeTree, "")
        .segments.filter((segment) => segment.type === "file")
        .slice(0, 4)
        .forEach((segment) => files.push(segment.path || segment.name));
    }
    if (!files.length) files.push("changed files");

    const sizeLayer = portal.group.userData.repositorySizeLayer;
    const miniSizeMap = portal.group.userData.repositoryMiniSizeMap;
    const ringFrame = sizeLayer?.userData?.repositorySizeMap
      ? sizeLayer
      : miniSizeMap?.userData?.repositorySizeMap
        ? miniSizeMap
        : null;
    const sizeMap = ringFrame?.userData.repositorySizeMap || null;
    const frameObject = ringFrame || portal.group;
    const frameCenter = new THREE.Vector3();
    frameObject.getWorldPosition(frameCenter);
    if (!ringFrame) frameCenter.y += 0.25;
    const frameQuaternion = new THREE.Quaternion();
    frameObject.getWorldQuaternion(frameQuaternion);
    const frameScale = new THREE.Vector3();
    frameObject.getWorldScale(frameScale);
    const frameSide = new THREE.Vector3(1, 0, 0).applyQuaternion(
      frameQuaternion,
    );
    const frameUp = new THREE.Vector3(0, 1, 0).applyQuaternion(
      frameQuaternion,
    );
    const frameNormal = new THREE.Vector3(0, 0, 1).applyQuaternion(
      frameQuaternion,
    );
    const ringOuter = sizeMap
      ? sizeMap.outerRadius * frameScale.x
      : 1.72;

    const commitSeed = hashNumber(String(pending.commit || repo || "code"));
    const direction = commitSeed % 2 ? -1 : 1;
    const start = target.clone().add(
      new THREE.Vector3(
        direction * (88 + (commitSeed % 24)),
        34 + (commitSeed % 13),
        -64 + (commitSeed % 31),
      ),
    );
    const group = new THREE.Group();
    group.name = `repository-code-landing:${owner}/${repo}`;
    const tileScale = 0.64;
    const stagger = reducedMotion ? 0 : 150;
    const landings = files.map((path, index) => {
      const segment = sizeMap
        ? repositoryChangedFileSegment(sizeMap.segments, path)
        : null;
      let impact;
      if (segment) {
        const middle = (segment.from + segment.to) * 0.5;
        const radius =
          sizeMap.innerRadius + (segment.depth - 0.5) * sizeMap.ringWidth;
        impact = ringFrame.localToWorld(
          new THREE.Vector3(
            Math.cos(middle) * radius,
            Math.sin(middle) * radius,
            sizeMap.lift + segment.depth * sizeMap.depthStep,
          ),
        );
      } else {
        const angle =
          (index / Math.max(1, files.length)) * Math.PI * 2 +
          (commitSeed % 17) * 0.08;
        impact = frameCenter
          .clone()
          .addScaledVector(frameSide, Math.cos(angle) * ringOuter * 0.6)
          .addScaledVector(frameUp, Math.sin(angle) * ringOuter * 0.6)
          .addScaledVector(frameNormal, 0.3);
      }
      const offset = impact.clone().sub(frameCenter);
      return {
        path,
        index,
        segment,
        impact,
        side: offset.dot(frameSide) >= 0 ? 1 : -1,
        height: offset.dot(frameUp),
      };
    });
    const calloutGap = 0.96 * tileScale + 0.14;
    const calloutReach = ringOuter + 0.45 + 1.7 * tileScale;
    [-1, 1].forEach((side) => {
      const column = landings
        .filter((landing) => landing.side === side)
        .sort((first, second) => second.height - first.height);
      column.forEach((landing, row) => {
        landing.callout = frameCenter
          .clone()
          .addScaledVector(frameSide, side * calloutReach)
          .addScaledVector(frameUp, ((column.length - 1) / 2 - row) * calloutGap)
          .addScaledVector(frameNormal, 0.42);
      });
    });

    const highlights = new Map();
    landings.forEach((landing) => {
      const segment = landing.segment;
      if (!segment || highlights.has(segment)) return;
      const inner =
        sizeMap.innerRadius + (segment.depth - 1) * sizeMap.ringWidth;
      const outer = inner + sizeMap.ringWidth;
      const glowGeometry = repositoryWedgeGeometry(
        THREE,
        inner,
        outer,
        segment.from,
        segment.to,
        0.01,
      );
      if (!glowGeometry) return;
      const holder = new THREE.Group();
      holder.name = `repository-changed-area:${segment.path}`;
      holder.position.copy(frameCenter);
      holder.quaternion.copy(frameQuaternion);
      holder.scale.copy(frameScale);
      const glow = new THREE.Mesh(
        glowGeometry,
        resources.uniqueMaterial(THREE, {
          color: segment.color,
          transparent: true,
          opacity: 0,
          blending: THREE.AdditiveBlending,
          depthWrite: false,
          toneMapped: false,
          side: THREE.DoubleSide,
          forceSinglePass: true,
        }, "MeshBasicMaterial"),
      );
      glow.position.z = sizeMap.lift + segment.depth * sizeMap.depthStep;
      const span = Math.max(0.02, segment.to - segment.from);
      const rim = new THREE.Mesh(
        resources.primitive(
          THREE,
          "TorusGeometry",
          outer,
          sizeMap.ringWidth * 0.1,
          6,
          Math.max(6, Math.ceil(span * 24)),
          span,
        ),
        resources.uniqueMaterial(THREE, {
          color: "#f0fdfa",
          transparent: true,
          opacity: 0,
          blending: THREE.AdditiveBlending,
          depthWrite: false,
          toneMapped: false,
        }, "MeshBasicMaterial"),
      );
      rim.rotation.z = segment.from;
      rim.position.z = glow.position.z + 0.012;
      holder.add(glow, rim);
      group.add(holder);
      highlights.set(segment, {
        glow,
        rim,
        delay: landing.index * stagger,
      });
    });

    const fileTiles = [];
    const shadows = [];
    landings.forEach(({ path, index, segment, impact, callout }) => {
      const accent = segment?.color || "#3fb950";
      const tile = new THREE.Mesh(
        resources.primitive(THREE, "PlaneGeometry", 3.4, 0.96),
        resources.uniqueMaterial(THREE, {
          map: repositoryLandingTexture(path, accent),
          transparent: true,
          opacity: 1,
          toneMapped: false,
          side: THREE.DoubleSide,
          forceSinglePass: true,
          depthWrite: false,
        }, "MeshBasicMaterial"),
      );
      tile.name = `repository-changed-file:${path}`;
      const fileStart = start.clone().add(
        new THREE.Vector3(index * direction * 3.1, index * 1.4, index * -2.4),
      );
      tile.position.copy(fileStart);
      tile.scale.setScalar(tileScale);
      group.add(tile);
      let leader = null;
      if (segment) {
        leader = new THREE.Mesh(
          resources.primitive(THREE, "CylinderGeometry", 0.018, 0.018, 1, 5),
          resources.uniqueMaterial(THREE, {
            color: new THREE.Color(accent).lerp(new THREE.Color("#ffffff"), 0.5),
            transparent: true,
            opacity: 0,
            depthWrite: false,
            toneMapped: false,
          }, "MeshBasicMaterial"),
        );
        leader.visible = false;
        leader.name = `repository-changed-file-leader:${path}`;
        group.add(leader);
      }
      fileTiles.push({
        tile,
        leader,
        leaderSpan: new THREE.Vector3(),
        leaderAxis: new THREE.Vector3(0, 1, 0),
        start: fileStart,
        impact,
        target: callout,
        delay: index * stagger,
        phase: index * 1.73 + (commitSeed % 11),
      });

      const shadow = new THREE.Mesh(
        resources.primitive(THREE, "PlaneGeometry", 4.2, 1.4),
        resources.uniqueMaterial(THREE, {
          color: "#010409",
          transparent: true,
          opacity: 0.34,
          depthWrite: false,
          side: THREE.DoubleSide,
          forceSinglePass: true,
        }, "MeshBasicMaterial"),
      );
      shadow.name = `repository-file-shadow:${path}`;
      shadow.rotation.x = -Math.PI / 2;
      shadow.position.set(fileStart.x, 0.19, fileStart.z);
      group.add(shadow);
      shadows.push(shadow);
    });

    const collisionRing = new THREE.Mesh(
      resources.primitive(THREE, "TorusGeometry", 2.05, 0.1, 8, 48),
      resources.uniqueMaterial(THREE, {
        color: "#79c0ff",
        transparent: true,
        opacity: 0,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    collisionRing.name = `repository-ring-collision:${owner}/${repo}`;
    collisionRing.position.copy(target);
    group.add(collisionRing);

    const sparkleCount = 36;
    const sparklePositions = new Float32Array(sparkleCount * 3);
    const sparkleSeeds = new Float32Array(sparkleCount * 3);
    for (let index = 0; index < sparkleCount; index += 1) {
      const angle = (index / sparkleCount) * Math.PI * 2;
      sparkleSeeds[index * 3] = Math.cos(angle) * (1.2 + (index % 5) * 0.22);
      sparkleSeeds[index * 3 + 1] =
        Math.sin(angle * 2.7) * 0.65 + (index % 4) * 0.25;
      sparkleSeeds[index * 3 + 2] =
        Math.sin(angle) * (1.2 + (index % 5) * 0.22);
    }
    const sparkleGeometry = new THREE.BufferGeometry();
    sparkleGeometry.setAttribute(
      "position",
      new THREE.BufferAttribute(sparklePositions, 3),
    );
    const sparkles = new THREE.Points(
      sparkleGeometry,
      new THREE.PointsMaterial({
        color: "#a5f3fc",
        size: 0.19,
        transparent: true,
        opacity: 0,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        toneMapped: false,
      }),
    );
    sparkles.name = `repository-file-sizzle:${owner}/${repo}`;
    sparkles.position.copy(target);
    group.add(sparkles);

    const night =
      ctx.localDaylightMinute < 6 * 60 || ctx.localDaylightMinute >= 20 * 60;
    let lightning = null;
    if (night) {
      const points = [];
      for (let index = 0; index <= 12; index += 1) {
        const progress = index / 12;
        points.push(
          new THREE.Vector3(
            target.x + Math.sin(index * 4.7) * (1 - progress) * 2.6,
            target.y + (1 - progress) * 62,
            target.z + Math.cos(index * 3.9) * (1 - progress) * 2.2,
          ),
        );
      }
      lightning = new THREE.Group();
      const lightningMaterial = resources.uniqueMaterial(THREE, {
        color: "#dbeafe",
        transparent: true,
        opacity: 0,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        toneMapped: false,
      }, "MeshBasicMaterial");
      for (let index = 1; index < points.length; index += 1) {
        const start = points[index - 1];
        const end = points[index];
        const direction = end.clone().sub(start);
        const segment = new THREE.Mesh(
          resources.primitive(THREE, "CylinderGeometry", 0.055, 0.055, direction.length(), 5),
          lightningMaterial,
        );
        segment.position.copy(start).add(end).multiplyScalar(0.5);
        segment.quaternion.setFromUnitVectors(
          new THREE.Vector3(0, 1, 0),
          direction.normalize(),
        );
        lightning.add(segment);
      }
      lightning.userData.material = lightningMaterial;
      lightning.name = `repository-night-lightning:${owner}/${repo}`;
      group.add(lightning);
    }
    shadows.forEach((shadow) => {
      shadow.visible = !night;
    });
    ctx.world.add(group);
    ctx.repositoryCodeLandings.push({
      group,
      fileTiles,
      shadows,
      highlights: [...highlights.values()],
      tileScale,
      collisionRing,
      sparkles,
      sparkleSeeds,
      lightning,
      target,
      startedAt: performance.now(),
      flightDuration: reducedMotion ? 120 : 1800,
      settleDuration: reducedMotion ? 120 : 640,
      sizzleDuration: 10_000,
    });
    return true;
  }

  // Serving reads as traffic leaving the rack: each clone or repository page a
  // node answers launches a small figure for the agent it served, shooting up
  // out of the cabinet and shrinking away into the sky. Purely cosmetic and
  // driven only by the node's own served counters in the signed mirror payload
  // (see updateNetworkNodes), never by an unauthenticated frame.
  function spawnServeFlights(position, count = 1) {
    if (!fns.worldElementEnabled("effects")) return;
    const requested = Math.floor(Number(count));
    const wanted = Math.min(
      3,
      Math.max(1, Number.isFinite(requested) ? requested : 1),
    );
    for (let index = 0; index < wanted; index += 1) {
      // Bound a busy fleet's burst to a fixed effect budget.
      if (ctx.serveFlights.length >= 12) return;
      const figure = createServedVisitorFigure(
        THREE,
        index % 2 ? "#8fd8ff" : "#9ef7c6",
      );
      figure.position.set(
        position.x + (Math.random() - 0.5) * 0.7,
        position.y + 3.3,
        position.z + (Math.random() - 0.5) * 0.7,
      );
      figure.rotation.y = Math.random() * Math.PI * 2;
      figure.visible = index === 0;
      ctx.world.add(figure);
      ctx.serveFlights.push({
        group: figure,
        materials: figure.userData.figureMaterials,
        // Staggered so a multi-request update reads as a stream rather than
        // one clump of overlapping figures.
        startedAt: performance.now() + index * 220,
        duration: 2400,
        baseY: figure.position.y,
        climb: 34 + Math.random() * 12,
        spin: (Math.random() < 0.5 ? -1 : 1) * (1.2 + Math.random()),
      });
    }
  }

  function playRewardEvent(targetHint = "") {
    if (!fns.worldElementEnabled("effects")) return;
    let targetPylon = null;
    ctx.nodeInfrastructure.forEach((pylon) => {
      if (
        !targetPylon &&
        (!targetHint ||
          String(pylon.userData?.nodeId || "")
            .toLowerCase()
            .includes(String(targetHint).toLowerCase()))
      ) {
        targetPylon = pylon;
      }
    });
    const target = targetPylon
      ? targetPylon.position.clone()
      : new THREE.Vector3(...landmarkById("organizations").position);
    target.y = 1.2;
    const start = new THREE.Vector3(...landmarkById("fountain").position);
    start.y = 2.4;
    const group = new THREE.Group();
    const particles = [];
    for (let index = 0; index < 18; index += 1) {
      const coin = new THREE.Mesh(
        resources.primitive(THREE, "OctahedronGeometry", 0.1 + (index % 3) * 0.025, 0),
        makeMaterial(THREE, index % 2 ? "#f7c96b" : "#9ef7c6", {
          emissive: index % 2 ? "#d49a28" : "#39c783",
          emissiveIntensity: 1.15,
          metalness: 0.5,
        }),
      );
      coin.position.copy(start);
      group.add(coin);
      particles.push(coin);
    }
    ctx.world.add(group);
    ctx.rewardFlights.push({
      group,
      particles,
      start,
      target,
      startedAt: performance.now(),
      duration: 2800,
    });
  }

  return {
    updateRepositoryGraph,
    playEmote,
    playHandshake,
    showAvatarChatBubble,
    showChatBubble,
    showMemberChatBubble,
    updateRewardPool,
    setRewardPoolMirrorCount,
    spawnPushSurge,
    spawnRepositoryCodeLanding,
    spawnServeFlights,
    playRewardEvent,
  };
}
