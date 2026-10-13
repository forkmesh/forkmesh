import { flagEmoji } from "../../world-data.js";
import {
  BADGE_DETAIL_UPGRADES_PER_SAMPLE,
  badgeCardDrawnPixels,
  badgeFollowPillHit,
  badgeReadingSelection,
  badgeWalletSquareHit,
} from "../avatar-badges.js";
import {
  WORLD_MODERATION_HANDLE_PATTERN,
  createAvatarModerationControls,
  createAvatarOrgTeamControl,
  createAvatarTeamBadges,
  sanitizedAdminGuestNetwork,
  sanitizedModerationHandles,
  sanitizedOrgTeamAssignment,
} from "../avatar-controls.js";
import { avatarDetailParent } from "../avatar-instancing.js";
import { AVATAR_SOLANA_ADDRESS_RE } from "../avatar-shared.js";
import {
  applyAvatarFaceImage,
  renderAvatarBadge,
  setAvatarBadgeDetail,
  syncAvatarBackName,
  syncAvatarVerifiedPin,
  syncCountryShirt,
} from "../avatars.js";
import {
  disposeObject3D,
  objectIsEffectivelyVisible,
} from "../object-utils.js";
import { AVATAR_HIGHLIGHT_SAMPLE_MS } from "../quality.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  identity,
  onAvatarWalletAction,
  onFediverseFollow,
  onFediverseProfile,
  reducedMotion,
  ctx,
}) {
  const moderationControlKeys = new Map();
  const orgTeamControlKeys = new Map();
  // Every avatar wearing a chest card, so the reading-resolution sweep has a
  // bounded list to rank instead of walking the scene graph. Registration is
  // the one path every badged avatar takes — the local player, live peers,
  // agent bots and directory figures alike.
  const badgedAvatars = new Set();


  // One unified chest card is clickable on the local player and live peers.
  // It contains identity, Fediverse and wallet controls without mode tabs.
  function registerAvatarChestControls(avatar, peerId) {
    if (!avatar?.userData?.badge || avatar.userData.chestRegistered) return;
    const meshes = [];
    avatar.traverse((child) => {
      if (child.isMesh) meshes.push(child);
    });
    meshes.forEach((mesh) => {
      ctx.avatarSelections.set(mesh, { avatar, peerId: String(peerId || "") });
      if (!ctx.interactive.includes(mesh)) ctx.interactive.push(mesh);
    });
    ctx.chestControls.set(avatar.userData.badge, {
      avatar,
      peerId: String(peerId || ""),
    });
    avatar.userData.avatarSelectionMeshes = meshes;
    avatar.userData.chestRegistered = true;
    badgedAvatars.add(avatar);
    if (!avatar.userData.fediverseProfile) {
      const directoryFigure = String(peerId || "").startsWith("member:");
      avatar.userData.fediverseProfile = {
        state: directoryFigure ? "idle" : "loading",
      };
      const target = {
        peerId: String(peerId || ""),
        name: String(avatar.userData.badgeIdentity?.name || ""),
        accountStatus: String(
          avatar.userData.badgeIdentity?.accountStatus || "Guest",
        ),
        self: String(peerId || "") === identity.id,
      };
      // Do not fan one profile + contribution request out for every seat as
      // the directory arrives. Directory figures hydrate on first selection;
      // local and live peer avatars remain eager.
      if (!directoryFigure) {
        queueMicrotask(() => onFediverseProfile(target));
      }
    }
  }

  function unregisterAvatarChestControls(avatar) {
    if (!avatar?.userData?.chestRegistered) return;
    (avatar.userData.avatarSelectionMeshes || []).forEach((mesh) => {
      if (!mesh) return;
      ctx.avatarSelections.delete(mesh);
      ctx.chestControls.delete(mesh);
      const index = ctx.interactive.indexOf(mesh);
      if (index >= 0) ctx.interactive.splice(index, 1);
    });
    delete avatar.userData.avatarSelectionMeshes;
    if (ctx.activeAvatarSelection === avatar) setActiveAvatarSelection(null);
    avatar.userData.chestRegistered = false;
    badgedAvatars.delete(avatar);
  }

  /**
   * Hands the reading resolution to the cards a visitor can actually read.
   * Card text is painted in one 512-unit layout at every size on screen, so
   * the only thing deciding whether it arrives as text or as mush is how many
   * device pixels its backing store holds while it is being read. The largest
   * few on screen are therefore repainted supersampled and released again on
   * the way out, which keeps the sharp text bounded by a headcount instead of
   * by every card in the World paying for it.
   */
  function updateAvatarBadgeDetail() {
    const viewportPixels = Math.max(
      1,
      Number(ctx.renderer.domElement?.height) || 0,
    );
    const cards = [];
    badgedAvatars.forEach((avatar) => {
      if (!avatar?.userData?.badge) return;
      cards.push({
        key: avatar,
        visible: objectIsEffectivelyVisible(avatar),
        reading: avatar.userData.badgeDetail === "reading",
        farPixels: avatar.userData.badgeFarPixels,
        drawnPixels: badgeCardDrawnPixels({
          distance: ctx.camera.position.distanceTo(
            avatar.userData.badge.getWorldPosition(ctx.badgeDetailWorldPosition),
          ),
          fov: ctx.camera.fov,
          viewportPixels,
          scale: avatar.scale.y,
        }),
      });
    });
    const reading = new Set(badgeReadingSelection(cards));
    let upgrades = 0;
    cards.forEach((card) => {
      if (!reading.has(card.key)) {
        setAvatarBadgeDetail(THREE, card.key, "far");
        return;
      }
      if (card.reading) return;
      // A 1024px repaint costs a few milliseconds, so walking into a group
      // upgrades over a handful of samples instead of stalling one frame with
      // every card at once.
      if (upgrades >= BADGE_DETAIL_UPGRADES_PER_SAMPLE) return;
      if (setAvatarBadgeDetail(THREE, card.key, "reading")) upgrades += 1;
    });
  }

  function setActiveAvatarSelection(avatar) {
    ctx.activeAvatarSelection = avatar || null;
    const now = performance.now();
    if (now >= ctx.nextAvatarHighlightAt) {
      ctx.nextAvatarHighlightAt = now + AVATAR_HIGHLIGHT_SAMPLE_MS;
      refreshAvatarSelectionHighlight();
    }
  }

  function refreshAvatarSelectionHighlight() {
    if (
      !ctx.activeAvatarSelection ||
      !objectIsEffectivelyVisible(ctx.activeAvatarSelection)
    ) {
      ctx.avatarSelectionHighlight.visible = false;
      return;
    }
    ctx.avatarSelectionBounds.setFromObject(ctx.activeAvatarSelection);
    ctx.avatarSelectionHighlight.visible = !ctx.avatarSelectionBounds.isEmpty();
  }

  function avatarSelectionPayload(control) {
    const avatar = control?.avatar;
    const member = avatar?.userData?.badgeIdentity || {};
    const profile = avatar?.userData?.fediverseProfile || {};
    const assignment = avatar?.userData?.orgTeamAssignment || {};
    return {
      peerId: String(control?.peerId || "").slice(0, 96),
      self: String(control?.peerId || "") === String(identity.id || ""),
      name: String(member.name || "World visitor").slice(0, 32),
      accountStatus: String(member.accountStatus || "Guest").slice(0, 32),
      flag: String(member.flag || "").slice(0, 8),
      countryCode: String(member.countryCode || "").slice(0, 2),
      browser: String(member.browser || "Browser").slice(0, 32),
      os: String(member.os || "Device").slice(0, 32),
      status: String(member.status || "exploring").slice(0, 64),
      statusEmoji: String(member.statusEmoji || "").slice(0, 16),
      statusNote: String(member.statusNote || "").slice(0, 80),
      emailVerified: member.emailVerified === true,
      firstVisitAge: String(member.firstVisitAge || "hidden").slice(0, 24),
      joinedAt: Math.max(0, Number(member.joinedAt) || 0),
      visitCount: Math.max(0, Number(member.visitCount) || 0),
      officeVisits: Math.max(0, Number(member.officeVisits) || 0),
      totalActiveMs: Math.max(0, Number(member.totalActiveMs) || 0),
      totalSteps: Math.max(0, Number(member.totalSteps) || 0),
      engagementId: String(member.engagementId || "").slice(0, 32),
      engagementLabel: String(member.engagementLabel || "").slice(0, 32),
      engagementScore: Math.max(
        0,
        Math.min(100, Math.round(Number(member.engagementScore) || 0)),
      ),
      nodes: (Array.isArray(member.nodes) ? member.nodes : [])
        .map((node) =>
          String(
            node && typeof node === "object"
              ? node.name || node.node || ""
              : node || "",
          ).slice(0, 48),
        )
        .slice(0, 6),
      teams: (Array.isArray(assignment.teams) ? assignment.teams : [])
        .map((team) => String(team || "").slice(0, 40))
        .slice(0, 12),
      fediverse:
        profile.state === "ready"
          ? {
              state: "ready",
              handle: String(profile.fediverse || profile.handle || "")
                .slice(0, 80),
              bio: String(profile.bio || "").slice(0, 160),
              followers: Math.max(0, Number(profile.followers) || 0),
              following: Math.max(0, Number(profile.following) || 0),
              typeTotals:
                profile.typeTotals && typeof profile.typeTotals === "object"
                  ? {
                      pulls: Math.max(0, Number(profile.typeTotals.pulls) || 0),
                      issues: Math.max(0, Number(profile.typeTotals.issues) || 0),
                      discussions: Math.max(
                        0,
                        Number(profile.typeTotals.discussions) || 0,
                      ),
                    }
                  : {},
              posts: (Array.isArray(profile.posts) ? profile.posts : [])
                .slice(0, 5)
                .map((post) => ({
                  label: String(
                    typeof post === "string"
                      ? post
                      : post?.label || post?.title || "",
                  ).slice(0, 100),
                  href: String(
                    typeof post === "object"
                      ? post?.href || post?.url || ""
                      : "",
                  ).slice(0, 500),
                })),
            }
          : { state: String(profile.state || "loading").slice(0, 24) },
    };
  }

  function removeDeletedAvatar(avatar, peerId = "") {
    if (!avatar) return;
    const id = String(peerId || "");
    removeRemoteModerationControls(avatar, id);
    removeRemoteOrgTeamControl(avatar, id);
    unregisterAvatarChestControls(avatar);
    if (ctx.remotePlayers.get(id) === avatar) {
      ctx.remotePlayers.delete(id);
      ctx.remoteLabels.get(id)?.remove();
      ctx.remoteLabels.delete(id);
    }
    if (ctx.loungeMembers.get(id) === avatar) ctx.loungeMembers.delete(id);
    if (ctx.officeParticipants.get(id) === avatar) {
      ctx.officeParticipants.delete(id);
      ctx.officeParticipantLabels.get(id)?.remove();
      ctx.officeParticipantLabels.delete(id);
      ctx.officeBubbles.get(id)?.remove();
      ctx.officeBubbles.delete(id);
    }
    avatar.parent?.remove(avatar);
    disposeObject3D(avatar);
  }

  async function animateUnverifiedAvatarDeletion(member = {}) {
    const name = String(member?.name || "").trim().toLowerCase();
    if (!name) return false;
    ctx.administrativelyRemovedAccounts.add(name);
    const matches = [];
    const collect = (avatar, peerId) => {
      const avatarName = String(
        avatar?.userData?.badgeIdentity?.name || "",
      ).trim().toLowerCase();
      if (avatar && avatarName === name) matches.push({ avatar, peerId });
    };
    ctx.remotePlayers.forEach((avatar, peerId) => collect(avatar, peerId));
    ctx.loungeMembers.forEach((avatar, peerId) => collect(avatar, peerId));
    ctx.officeParticipants.forEach((avatar, peerId) => collect(avatar, peerId));
    if (!matches.length) return false;

    const duration = reducedMotion ? 120 : 900;
    await Promise.all(
      matches.map(({ avatar, peerId }) => new Promise((resolve) => {
        const parent = avatar.parent;
        const burst = new THREE.Group();
        burst.name = "forkmesh-admin-delete-burst";
        burst.position.copy(avatar.position);
        const particles = [];
        const colors = ["#ff4d6d", "#ffcf4a", "#77d9ff", "#b8f2a1"];
        for (let index = 0; index < 18; index += 1) {
          const particle = new THREE.Mesh(
            resources.primitive(THREE, "SphereGeometry", 0.035 + (index % 3) * 0.012, 8, 6),
            resources.uniqueMaterial(THREE, {
              color: colors[index % colors.length],
              transparent: true,
              depthWrite: false,
            }, "MeshBasicMaterial"),
          );
          const angle = (index / 18) * Math.PI * 2;
          particle.userData.velocity = new THREE.Vector3(
            Math.cos(angle) * (0.9 + (index % 4) * 0.18),
            0.8 + (index % 5) * 0.22,
            Math.sin(angle) * (0.9 + (index % 4) * 0.18),
          );
          particles.push(particle);
          burst.add(particle);
        }
        parent?.add(burst);
        unregisterAvatarChestControls(avatar);
        const startedAt = performance.now();
        const initialY = avatar.position.y;
        const initialScale = avatar.scale.clone();
        const frame = (now) => {
          const progress = Math.min(1, (now - startedAt) / duration);
          const eased = 1 - (1 - progress) ** 3;
          avatar.rotation.y += reducedMotion ? 0 : 0.22;
          avatar.position.y = initialY + eased * 1.35;
          avatar.scale.copy(initialScale).multiplyScalar(
            Math.max(0.02, 1 - eased),
          );
          particles.forEach((particle) => {
            const velocity = particle.userData.velocity;
            particle.position.copy(velocity).multiplyScalar(eased);
            particle.position.y -= eased * eased * 0.45;
            particle.material.opacity = 1 - progress;
          });
          if (progress < 1) {
            requestAnimationFrame(frame);
            return;
          }
          parent?.remove(burst);
          disposeObject3D(burst);
          removeDeletedAvatar(avatar, peerId);
          resolve();
        };
        requestAnimationFrame(frame);
      })),
    );
    return true;
  }

  /** Attach a loaded (or failed) fediverse card to one avatar's chest. */
  function setAvatarFediverseProfile(peerId, profile) {
    const id = String(peerId || "");
    const officeParticipant = ctx.officeParticipants.get(id);
    const avatar =
      id === identity.id
        ? ctx.player
        : ctx.remotePlayers.get(id) ||
          ctx.loungeMembers.get(id) ||
          officeParticipant;
    if (!avatar?.userData?.badge) return;
    avatar.userData.fediverseProfile =
      profile && typeof profile === "object" ? profile : { state: "unavailable" };
    syncAvatarBackName(
      THREE,
      avatar,
      avatar.userData.badgeIdentity || {},
    );
    const profileCountry = /^[A-Z]{2}$/.test(
      String(profile?.countryCode || "").toUpperCase(),
    )
      ? String(profile.countryCode).toUpperCase()
      : "";
    if (
      profile?.emailVerified === true ||
      profileCountry ||
      String(profile?.flag || "") ||
      String(profile?.avatarUrl || "")
    ) {
      const enriched = {
        ...(avatar.userData.badgeIdentity || {}),
        emailVerified:
          profile?.emailVerified === true ||
          avatar.userData.badgeIdentity?.emailVerified === true,
        countryCode:
          profileCountry ||
          avatar.userData.badgeIdentity?.countryCode ||
          "",
        flag:
          String(profile?.flag || "") ||
          (profileCountry ? flagEmoji(profileCountry) : "") ||
          avatar.userData.badgeIdentity?.flag ||
          "◌",
      };
      avatar.userData.badgeIdentity = enriched;
      syncCountryShirt(THREE, avatar, enriched);
      syncAvatarVerifiedPin(THREE, avatar, enriched);
      if (String(profile?.avatarUrl || "")) {
        applyAvatarFaceImage(THREE, avatar, profile.avatarUrl);
      }
    }
    renderAvatarBadge(THREE, avatar, avatar.userData.badgeRemote === true);
    if (avatar === ctx.player && ctx.officeLobbyPlayer?.userData?.badge) {
      ctx.officeLobbyPlayer.userData.fediverseProfile =
        avatar.userData.fediverseProfile;
      ctx.officeLobbyPlayer.userData.badgeIdentity = {
        ...(ctx.officeLobbyPlayer.userData.badgeIdentity || {}),
        ...(avatar.userData.badgeIdentity || {}),
      };
      syncCountryShirt(
        THREE,
        ctx.officeLobbyPlayer,
        ctx.officeLobbyPlayer.userData.badgeIdentity,
      );
      syncAvatarVerifiedPin(
        THREE,
        ctx.officeLobbyPlayer,
        ctx.officeLobbyPlayer.userData.badgeIdentity,
      );
      if (String(profile?.avatarUrl || "")) {
        applyAvatarFaceImage(THREE, ctx.officeLobbyPlayer, profile.avatarUrl);
      }
      renderAvatarBadge(THREE, ctx.officeLobbyPlayer, false);
    }
    if (
      officeParticipant?.userData?.badge &&
      officeParticipant !== avatar
    ) {
      officeParticipant.userData.fediverseProfile =
        avatar.userData.fediverseProfile;
      officeParticipant.userData.badgeIdentity = {
        ...(officeParticipant.userData.badgeIdentity || {}),
        ...(avatar.userData.badgeIdentity || {}),
      };
      syncCountryShirt(
        THREE,
        officeParticipant,
        officeParticipant.userData.badgeIdentity,
      );
      syncAvatarVerifiedPin(
        THREE,
        officeParticipant,
        officeParticipant.userData.badgeIdentity,
      );
      if (String(profile?.avatarUrl || "")) {
        applyAvatarFaceImage(THREE, officeParticipant, profile.avatarUrl);
      }
      renderAvatarBadge(THREE, officeParticipant, true);
    }
  }

  function setAvatarRecentPublicMessage(sender, text) {
    const name = String(sender || "").trim().toLowerCase();
    const message = String(text || "")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, 120);
    if (!name || !message) return false;
    const avatars = new Set([
      ctx.player,
      ctx.officeLobbyPlayer,
      ...ctx.remotePlayers.values(),
      ...ctx.loungeMembers.values(),
      ...ctx.officeParticipants.values(),
    ]);
    let updated = false;
    avatars.forEach((avatar) => {
      const badgeIdentity = avatar?.userData?.badgeIdentity;
      if (
        !badgeIdentity ||
        String(badgeIdentity.name || "").trim().toLowerCase() !== name
      ) {
        return;
      }
      avatar.userData.badgeIdentity = {
        ...badgeIdentity,
        recentPublicMessage: message,
      };
      renderAvatarBadge(
        THREE,
        avatar,
        avatar.userData.badgeRemote === true,
      );
      updated = true;
    });
    return updated;
  }

  /** Dress one avatar's face with a consented account avatar photo. */
  function setAvatarFaceImage(peerId, url) {
    const id = String(peerId || "");
    const avatar =
      id === identity.id
        ? ctx.player
        : ctx.remotePlayers.get(id) || ctx.loungeMembers.get(id);
    if (avatar) applyAvatarFaceImage(THREE, avatar, url);
    if (id === identity.id && ctx.officeLobbyPlayer) {
      applyAvatarFaceImage(THREE, ctx.officeLobbyPlayer, url);
    }
    const officeParticipant = ctx.officeParticipants.get(id);
    if (officeParticipant) {
      applyAvatarFaceImage(THREE, officeParticipant, url);
    }
  }

  function handleChestControl(control, hit) {
    const avatar = control.avatar;
    if (badgeWalletSquareHit(hit.uv)) {
      onAvatarWalletAction({
        peerId: control.peerId,
        self: control.peerId === identity.id,
        address: String(avatar.userData.badgeIdentity?.solana || ""),
      });
      return true;
    }
    if (!badgeFollowPillHit(hit.uv)) {
      return false;
    }
    const profile = avatar.userData.fediverseProfile || {};
    if (profile.state !== "ready" || profile.canFollow !== true) return false;
    avatar.userData.fediverseProfile = { ...profile, pending: true };
    renderAvatarBadge(THREE, avatar, avatar.userData.badgeRemote === true);
    onFediverseFollow({
      peerId: control.peerId,
      name: String(profile.account || avatar.userData.badgeIdentity?.name || ""),
      following: profile.isFollowing === true,
    });
    return true;
  }

  function removeRemoteModerationControls(avatar, peerId) {
    const controls = avatar?.userData?.moderationControls;
    if (controls) {
      controls.traverse((child) => {
        ctx.moderationActions.delete(child);
        ctx.adminGuestCopyActions.delete(child);
        const interactiveIndex = ctx.interactive.indexOf(child);
        if (interactiveIndex >= 0) ctx.interactive.splice(interactiveIndex, 1);
      });
      controls.parent?.remove(controls);
      disposeObject3D(controls);
      delete avatar.userData.moderationControls;
    }
    moderationControlKeys.delete(String(peerId || ""));
  }

  function syncRemoteModerationControls(avatar, remote) {
    const peerId = String(remote?.id || "");
    const name = String(remote?.name || "visitor").slice(0, 32);
    const handles = sanitizedModerationHandles(
      remote,
      identity?.isAdmin === true,
    );
    const guestNetwork = sanitizedAdminGuestNetwork(
      remote,
      identity?.isAdmin === true,
    );
    const key = JSON.stringify([
      name,
      handles.ip || "",
      handles.agent || "",
      guestNetwork.ipAddress || "",
      guestNetwork.userAgent || "",
    ]);
    if (
      moderationControlKeys.get(peerId) === key &&
      avatar?.userData?.moderationControls
    ) {
      return;
    }
    removeRemoteModerationControls(avatar, peerId);
    if (
      !handles.ip &&
      !handles.agent &&
      !guestNetwork.ipAddress &&
      !guestNetwork.userAgent
    ) {
      return;
    }

    const controls = createAvatarModerationControls(
      THREE,
      handles,
      guestNetwork,
    );
    controls.children.forEach((control) => {
      const copyField = control.userData.worldGuestCopy;
      if (copyField && guestNetwork[copyField]) {
        ctx.adminGuestCopyActions.set(control, {
          field: copyField,
          value: guestNetwork[copyField],
          peerId,
          name,
        });
        ctx.interactive.push(control);
        return;
      }
      const targetType = control.userData.worldModerationControl;
      const handle = handles[targetType];
      if (!WORLD_MODERATION_HANDLE_PATTERN.test(handle || "")) return;
      ctx.moderationActions.set(control, {
        targetType,
        handle,
        peerId,
        name,
      });
      ctx.interactive.push(control);
    });
    avatarDetailParent(avatar).add(controls);
    avatar.userData.moderationControls = controls;
    moderationControlKeys.set(peerId, key);
  }

  function removeRemoteOrgTeamControl(avatar, peerId) {
    const controls = avatar?.userData?.orgTeamControls;
    if (controls) {
      controls.traverse((child) => {
        ctx.orgTeamActions.delete(child);
        const interactiveIndex = ctx.interactive.indexOf(child);
        if (interactiveIndex >= 0) ctx.interactive.splice(interactiveIndex, 1);
      });
      controls.parent?.remove(controls);
      disposeObject3D(controls);
      delete avatar.userData.orgTeamControls;
    }
    const teamBadges = avatar?.userData?.teamBadges;
    if (teamBadges) {
      avatar.userData.leftArm?.remove(teamBadges);
      disposeObject3D(teamBadges);
      delete avatar.userData.teamBadges;
    }
    orgTeamControlKeys.delete(String(peerId || ""));
    delete avatar.userData.orgTeamAssignment;
  }

  // The organization-admin counterpart of the moderation plaques: one plaque
  // per member avatar, opening the team multi-select the app layer owns. The
  // app only supplies `orgTeam` for members of an organization the viewer
  // owns or administers, so a plain member never sees another back plaque.
  // Registered inactive and same-browser peers are intentionally accepted:
  // login state does not change organization membership, and every write is
  // re-authorized by the worker.
  function syncRemoteOrgTeamControl(avatar, remote) {
    const peerId = String(remote?.id || "");
    const name = String(remote?.name || "visitor").slice(0, 32);
    const assignment = sanitizedOrgTeamAssignment(remote);
    const key = JSON.stringify([
      name,
      assignment?.org || "",
      assignment?.member || "",
      assignment?.assigned ?? -1,
      assignment?.total ?? -1,
      assignment?.canManage === true,
      ...(assignment?.teams || []),
    ]);
    const hasExpectedControl =
      assignment?.canManage !== true ||
      Boolean(avatar?.userData?.orgTeamControls);
    const hasExpectedBadges =
      !assignment?.teams?.length || Boolean(avatar?.userData?.teamBadges);
    if (
      orgTeamControlKeys.get(peerId) === key &&
      hasExpectedControl &&
      hasExpectedBadges
    ) {
      return;
    }
    removeRemoteOrgTeamControl(avatar, peerId);
    if (!assignment) return;
    avatar.userData.orgTeamAssignment = {
      org: assignment.org,
      member: assignment.member,
      teams: [...assignment.teams],
    };

    if (assignment.canManage) {
      const controls = createAvatarOrgTeamControl(THREE, assignment);
      controls.children.forEach((control) => {
        ctx.orgTeamActions.set(control, {
          org: assignment.org,
          member: assignment.member,
          peerId,
          name,
        });
        ctx.interactive.push(control);
      });
      avatarDetailParent(avatar).add(controls);
      avatar.userData.orgTeamControls = controls;
    }
    if (assignment.teams.length && avatar.userData.leftArm) {
      const teamBadges = createAvatarTeamBadges(THREE, assignment);
      avatar.userData.leftArm.add(teamBadges);
      avatar.userData.teamBadges = teamBadges;
    }
    orgTeamControlKeys.set(peerId, key);
  }

  function setLocalOrgTeam(orgTeam) {
    const localRecord = {
      id: identity.id,
      name: identity.name,
      accountStatus: identity.accountStatus || "Registered",
      orgTeam,
    };
    syncRemoteOrgTeamControl(ctx.player, localRecord);
    if (ctx.officeLobbyPlayer) {
      syncRemoteOrgTeamControl(ctx.officeLobbyPlayer, localRecord);
    }
  }

  // Fill in the directory-only rows for an avatar's badge. A guest can type
  // any display name, so only a server-stamped account status may claim the
  // public record filed under that name.
  function withMemberFacts(identity) {
    if (!identity || String(identity.accountStatus || "Guest") === "Guest") {
      return identity;
    }
    const facts = ctx.memberFacts.get(
      String(identity.name || "").trim().toLowerCase(),
    );
    if (!facts) return identity;
    const countryCode = String(identity.countryCode || facts.countryCode || "")
      .toUpperCase();
    const currentFlag = String(identity.flag || "");
    const browser = String(identity.browser || "");
    const os = String(identity.os || "");
    return {
      ...identity,
      countryCode,
      flag:
        currentFlag && currentFlag !== "◌"
          ? currentFlag
          : facts.flag || (countryCode ? flagEmoji(countryCode) : "◌"),
      browser:
        browser && !["browser", "hidden"].includes(browser.toLowerCase())
          ? browser
          : facts.browser || browser,
      os:
        os && !["device", "hidden"].includes(os.toLowerCase())
          ? os
          : facts.os || os,
      firstSeenMinutes:
        Number(identity.firstSeenMinutes) > 0
          ? identity.firstSeenMinutes
          : facts.firstSeenMinutes,
      firstVisitAge:
        identity.firstVisitAge && identity.firstVisitAge !== "hidden"
          ? identity.firstVisitAge
          : facts.firstVisitAge,
      visitCount: Math.max(
        Number(identity.visitCount) || 0,
        Number(facts.visitCount) || 0,
      ),
      officeVisits: Math.max(
        Number(identity.officeVisits) || 0,
        Number(facts.officeVisits) || 0,
      ),
      joinedAt:
        Number(identity.joinedAt) > 0 ? identity.joinedAt : facts.joinedAt,
      solana: AVATAR_SOLANA_ADDRESS_RE.test(String(identity.solana || ""))
        ? String(identity.solana)
        : facts.solana || "",
      contributionTotals: Object.fromEntries(
        ["issues", "pulls", "commits", "discussions"].map((key) => [
          key,
          Math.max(
            Number(identity.contributionTotals?.[key]) || 0,
            Number(facts.contributionTotals?.[key]) || 0,
          ),
        ]),
      ),
      achievementBadges: Array.isArray(facts.achievementBadges)
        ? facts.achievementBadges.slice(0, 5)
        : Array.isArray(identity.achievementBadges)
          ? identity.achievementBadges.slice(0, 5)
          : [],
      engagementId: facts.engagementId || identity.engagementId || "",
      engagementLabel:
        facts.engagementLabel || identity.engagementLabel || "",
      engagementScore: Math.max(
        Number(identity.engagementScore) || 0,
        Number(facts.engagementScore) || 0,
      ),
      emailVerified:
        identity.emailVerified === true || facts.emailVerified === true,
      lastEmailAt: Math.max(
        Number(identity.lastEmailAt) || 0,
        Number(facts.lastEmailAt) || 0,
      ),
      lastEmailStatus:
        String(identity.lastEmailStatus || facts.lastEmailStatus || ""),
      totalActiveMs:
        identity.totalActiveMs == null
          ? facts.totalActiveMs
          : identity.totalActiveMs,
      totalSteps:
        identity.totalSteps == null ? facts.totalSteps : identity.totalSteps,
      // A presence frame carries no mail stamp; the owner's own authenticated
      // read (which knows the exact minute) still wins on their avatar.
      lastEmailAt: Math.max(
        Number(identity.lastEmailAt) || 0,
        Number(facts.lastEmailAt) || 0,
      ),
      lastEmailStatus:
        Number(identity.lastEmailAt) >= Number(facts.lastEmailAt || 0)
          ? identity.lastEmailStatus || facts.lastEmailStatus
          : facts.lastEmailStatus,
      lastEmailPrivate: false,
    };
  }

  // The account avatar is public directory data, not a reward for clicking a
  // chest open: every signed-in figure wears it from the moment it is built.
  // Only an explicit opt-out keeps the generated face.
  function dressMemberFace(avatar, identity = {}) {
    if (!avatar?.userData?.faceMesh) return;
    if (identity.faceImage === false) return;
    if (String(identity.accountStatus || "Guest") === "Guest") return;
    const photo = String(
      ctx.memberFacts.get(String(identity.name || "").trim().toLowerCase())
        ?.avatar || "",
    );
    if (!photo || avatar.userData.faceImageUrl === photo) return;
    if (avatar.userData.faceImageRetry?.url === photo) return;
    applyAvatarFaceImage(THREE, avatar, photo);
  }

  return {
    registerAvatarChestControls,
    unregisterAvatarChestControls,
    updateAvatarBadgeDetail,
    setActiveAvatarSelection,
    refreshAvatarSelectionHighlight,
    avatarSelectionPayload,
    animateUnverifiedAvatarDeletion,
    setAvatarFediverseProfile,
    setAvatarRecentPublicMessage,
    setAvatarFaceImage,
    handleChestControl,
    removeRemoteModerationControls,
    syncRemoteModerationControls,
    removeRemoteOrgTeamControl,
    syncRemoteOrgTeamControl,
    setLocalOrgTeam,
    withMemberFacts,
    dressMemberFace,
  };
}
