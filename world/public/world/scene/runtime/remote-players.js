import { hashNumber } from "../../world-avatar-face.js";
import {
  CAMPFIRE_SEATED_ACTIVITY,
  flagEmoji,
  landmarkById,
} from "../../world-data.js";
import { arrivalPlaqueTexture } from "../arrival.js";
import { firstSeenMinutesBucket } from "../avatar-badges.js";
import { AVATAR_SOLANA_ADDRESS_RE } from "../avatar-shared.js";
import {
  REGISTERED_LOUNGE_STATUSES,
  applyAvatarFaceImage,
  applyLegPitch,
  applySeatedLegPose,
  createAvatar,
  seatedAvatarY,
  syncAvatarActivity,
  syncOperatorBelt,
  updateAvatarBadge,
} from "../avatars.js";
import { CAMERA_PITCH_MAX, CAMERA_PITCH_MIN } from "../gameplay-constants.js";
import {
  makeLabelSprite,
  makePlayerLabel,
  updatePlayerLabel,
} from "../labels.js";
import {
  CAMPFIRE_GROUND_Y,
  CONTINUOUS_CITY_SURFACE_Y,
  MEMBER_ISLAND_CENTER_Z,
  WORLD_RADIUS,
  arrivalFacingHeading,
} from "../layout.js";
import { makeMaterial } from "../materials.js";
import {
  MEMBER_ENGAGEMENT_STATES,
  memberHasCustomAvatar,
  rankedMembersByEngagement,
} from "../member-engagement.js";
import { newestMemberName } from "../members-circle.js";
import {
  WORLD_MESH_SPACE,
  worldCeilingForSpace,
  worldRadiusForSpace,
} from "../movement.js";
import {
  clamp,
  disposeObject3D,
  disposeOwnedGeometry,
  disposeOwnedMaterial,
} from "../object-utils.js";
import * as resources from "../resources.js";

export function functions({
  THREE,
  identity,
  labelLayer,
  onLocationChange,
  onMovement,
  ctx,
  fns,
}) {
  // A member is a person, so their figure is a person's: the circle seats
  // full-size avatars, the same build the player walks around in, never a
  // shrunken stand-in sized to make the roster fit.
  const MEMBER_CIRCLE_FIGURE_SCALE = 1;
  const neighborhoodHomes = new Map();
  let memberLoungeSignature = "";


  function setRemotePlayers(players = []) {
    players = (Array.isArray(players) ? players : []).filter(
      (remote) =>
        !ctx.administrativelyRemovedAccounts.has(
          String(remote?.name || "").trim().toLowerCase(),
        ),
    );
    const seen = new Set();
    players.forEach((remote) => {
      if (!remote?.id || remote.id === identity.id) return;
      seen.add(remote.id);
      let avatar = ctx.remotePlayers.get(remote.id);
      const badgeIdentity = fns.withMemberFacts({
        id: remote.id,
        name: remote.name || "visitor",
        flag: remote.flag || "◌",
        countryCode: remote.countryCode || "",
        browser: remote.browser || "Browser",
        os: remote.os || "Device",
        status: remote.activity || "exploring",
        accountStatus: remote.accountStatus || "Guest",
        emailVerified: remote.emailVerified === true,
        localTime: remote.localTime || "",
        activityCategory:
          String(remote.activity || "") === CAMPFIRE_SEATED_ACTIVITY
            ? "sitting-at-member-fire"
            : remote.category || remote.activityCategory || "hidden",
        inputActive: remote.inputActive === true,
        visitCount: Math.max(
          0,
          Math.min(999, Number(remote.visitCount) || 0),
        ),
        firstVisitAge: remote.firstVisitAge || "hidden",
        firstSeenMinutes: Math.max(0, Number(remote.firstSeenMinutes) || 0),
        joinedAt: Math.max(0, Number(remote.joinedAt) || 0),
        nodes: Array.isArray(remote.nodes) ? remote.nodes.slice(0, 6) : [],
        statusEmoji: remote.statusEmoji || "",
        statusNote: remote.statusNote || "",
        outfitColor: remote.outfitColor || "",
        outfitStyle: remote.outfitStyle || "",
        // A persisted contributor row carries no presence frame, so an
        // absent flag means "wear the public photo"; only an explicit opt-out
        // keeps the generated face.
        faceImage: remote.faceImage !== false,
        activityBucket: remote.activityBucket || "",
        // The public directory's total-active-time aggregate, forwarded by the
        // app layer so a live member's chest carries the same row their
        // campfire bench figure does.
        totalActiveMs: Number.isFinite(Number(remote.totalActiveMs))
          ? Math.max(0, Number(remote.totalActiveMs))
          : null,
        totalSteps:
          remote.totalSteps != null &&
          Number.isSafeInteger(Number(remote.totalSteps))
          ? Math.max(0, Number(remote.totalSteps))
          : null,
        solana: remote.solana || "",
        walletSol: remote.walletSol ?? null,
        walletTxBucket: remote.walletTxBucket || "",
      });
      const badgeKey = JSON.stringify(badgeIdentity);
      if (!avatar) {
        avatar = createAvatar(
          THREE,
          badgeIdentity,
          { remote: true, scale: 0.92, instances: ctx.avatarPartInstances },
        );
        avatar.userData.badgeKey = badgeKey;
        const initialX = Number(remote.x);
        const initialZ = Number(remote.z);
        avatar.position.set(
          Number.isFinite(initialX) ? initialX : 0,
          CONTINUOUS_CITY_SURFACE_Y,
          Number.isFinite(initialZ) ? initialZ : 0,
        );
        avatar.userData.lookYaw = Number.isFinite(Number(remote.lookYaw))
          ? Number(remote.lookYaw)
          : Number(remote.heading) || 0;
        avatar.userData.targetLookYaw = avatar.userData.lookYaw;
        avatar.userData.lookPitch = Number.isFinite(Number(remote.lookPitch))
          ? Number(remote.lookPitch)
          : 0;
        avatar.userData.targetLookPitch = avatar.userData.lookPitch;
        ctx.world.add(avatar);
        ctx.remotePlayers.set(remote.id, avatar);
        ctx.remoteLabels.set(remote.id, makePlayerLabel(avatar, labelLayer));
        fns.registerAvatarChestControls(avatar, remote.id);
        ctx.figureBatches.register(avatar);
        const remoteId = remote.id;
        fns.registerWorldElement(
          "remote-avatars",
          "Remote visitor avatars",
          "Avatars & bots",
          avatar,
          () => ctx.remotePlayers.get(remoteId) === avatar,
        );
      }
      const sharedInactive = remote.activity === "idle";
      avatar.userData.targetLookYaw = Number.isFinite(Number(remote.lookYaw))
        ? Number(remote.lookYaw)
        : Number(remote.heading) || 0;
      avatar.userData.targetLookPitch = clamp(
        Number(remote.lookPitch) || 0,
        CAMERA_PITCH_MIN,
        CAMERA_PITCH_MAX,
      );
      const loungeEligible = REGISTERED_LOUNGE_STATUSES.has(
        String(remote.accountStatus || ""),
      );
      const recentlyActive =
        remote.recent === true ||
        ["recent", "returning"].includes(String(remote.availability || ""));
      const useRegisteredLounge =
        loungeEligible && (sharedInactive || recentlyActive);
      avatar.userData.loungeActivity = useRegisteredLounge
        ? recentlyActive
          ? "recent"
          : "idle"
        : "";
      avatar.userData.campfireSeated =
        String(remote.activity || "") === CAMPFIRE_SEATED_ACTIVITY;
      if (useRegisteredLounge) {
        // Live idle peers keep their presence avatar just outside the open
        // member circle. Directory figures use the inner rings, so the same
        // account is never represented twice in the clearing.
        const place = hashNumber(remote.id) % 8;
        avatar.userData.targetPosition.set(
          ctx.campfire.position.x - 3.15 + (place % 4) * 2.1,
          ctx.campfire.position.y + CAMPFIRE_GROUND_Y,
          ctx.campfire.position.z - ctx.MEMBER_CLEARING_RADIUS -
            4.5 - Math.floor(place / 4) * 1.7,
        );
        avatar.userData.targetHeading = Math.PI;
      } else if (sharedInactive) {
        const restArea = landmarkById("neighborhood").position;
        const seat = hashNumber(remote.id) % 8;
        avatar.userData.targetPosition.set(
          restArea[0] - 2.8 + (seat % 4) * 1.8,
          fns.outdoorFloorYAt(
            restArea[0] - 2.8 + (seat % 4) * 1.8,
            restArea[2] + 4.1 + Math.floor(seat / 4) * 0.8,
          ),
          restArea[2] + 4.1 + Math.floor(seat / 4) * 0.8,
        );
      } else {
        const remoteRadius = worldRadiusForSpace(remote.space, ctx.packageSpaces);
        avatar.userData.targetPosition.set(
          clamp(Number(remote.x) || 0, -remoteRadius, remoteRadius),
          clamp(
            Number(remote.y) || CONTINUOUS_CITY_SURFACE_Y,
            CONTINUOUS_CITY_SURFACE_Y,
            worldCeilingForSpace(remote.space, ctx.packageSpaces),
          ),
          clamp(Number(remote.z) || 0, -remoteRadius, remoteRadius),
        );
        avatar.userData.targetHeading = arrivalFacingHeading(
          avatar.userData.targetPosition.x,
          avatar.userData.targetPosition.z,
          Number(remote.heading) || 0,
        );
      }
      avatar.userData.status = remote.activity || "exploring";
      if (avatar.userData.badgeKey !== badgeKey) {
        updateAvatarBadge(THREE, avatar, badgeIdentity, true);
        syncOperatorBelt(THREE, avatar, badgeIdentity.nodes);
        avatar.userData.badgeKey = badgeKey;
        updatePlayerLabel(ctx.remoteLabels.get(remote.id), badgeIdentity);
      }
      fns.dressMemberFace(avatar, badgeIdentity);
      fns.syncRemoteModerationControls(avatar, remote);
      fns.syncRemoteOrgTeamControl(avatar, remote);
    });
    ctx.remotePlayers.forEach((avatar, id) => {
      if (seen.has(id)) return;
      fns.removeRemoteModerationControls(avatar, id);
      fns.removeRemoteOrgTeamControl(avatar, id);
      fns.unregisterAvatarChestControls(avatar);
      ctx.world.remove(avatar);
      avatar.traverse((child) => {
        disposeOwnedGeometry(child.geometry);
        disposeOwnedMaterial(child.material, true);
      });
      ctx.remotePlayers.delete(id);
      ctx.remoteLabels.get(id)?.remove();
      ctx.remoteLabels.delete(id);
    });
    updateNeighborhoodHomes(
      players.filter((player) => player?.space !== WORLD_MESH_SPACE),
    );
  }

  function setRemotePlayerMovement(id, movement = {}) {
    const avatar = ctx.remotePlayers.get(String(id || ""));
    if (!avatar?.userData?.targetPosition) {
      // A connected account can remain in the app roster briefly after an
      // administrator removes its scene avatar. Treat its later move frames
      // as handled so they cannot trigger a full rejected rebuild each time.
      return ctx.administrativelyRemovedAccounts.has(
        String(movement?.name || "").trim().toLowerCase(),
      );
    }
    avatar.userData.race =
      movement.race && typeof movement.race === "object" ? movement.race : null;
    avatar.userData.raceAt = performance.now();
    avatar.userData.targetLookYaw = Number.isFinite(Number(movement.lookYaw))
      ? Number(movement.lookYaw)
      : Number(movement.heading) || avatar.userData.targetHeading;
    avatar.userData.targetLookPitch = clamp(
      Number(movement.lookPitch) || 0,
      CAMERA_PITCH_MIN,
      CAMERA_PITCH_MAX,
    );
    // The authoritative roster pass owns deterministic lounge, campfire and
    // neighborhood placement. Movement frames for those presence states are
    // transport noise, not permission to pull a parked avatar off its seat.
    if (
      avatar.userData.loungeActivity ||
      avatar.userData.campfireSeated
    ) {
      return true;
    }
    const movementRadius = worldRadiusForSpace(movement.space, ctx.packageSpaces);
    const x = clamp(
      Number(movement.x) || 0,
      -movementRadius,
      movementRadius,
    );
    const y = clamp(
      Number(movement.y) || CONTINUOUS_CITY_SURFACE_Y,
      CONTINUOUS_CITY_SURFACE_Y,
      worldCeilingForSpace(movement.space, ctx.packageSpaces),
    );
    const z = clamp(
      Number(movement.z) || 0,
      -movementRadius,
      movementRadius,
    );
    avatar.userData.targetPosition.set(x, y, z);
    avatar.userData.targetHeading = arrivalFacingHeading(
      x,
      z,
      Number(movement.heading) || 0,
    );
    return true;
  }

  function updateNeighborhoodHomes(players = []) {
    const neighborhood = ctx.landmarkObjects.get("neighborhood");
    if (!neighborhood) return;
    const homes = players
      .filter((player) => player?.id)
      .slice(0, 12)
      .map((player) => ({
        id: String(player.id),
        name: String(player.name || "visitor").slice(0, 20),
        door: ["closed", "knock", "open"].includes(player.publicDoor)
          ? player.publicDoor
          : "closed",
      }));
    const key = JSON.stringify(homes);
    if (neighborhood.userData.homesKey === key) return;
    const existing = neighborhood.userData.publicHomes;
    if (existing) {
      neighborhood.remove(existing);
      existing.traverse((child) => {
        child.geometry?.dispose?.();
        child.material?.map?.dispose?.();
        child.material?.dispose?.();
      });
    }
    const group = new THREE.Group();
    neighborhoodHomes.clear();
    homes.forEach((home, index) => {
      const angle = (index / Math.max(1, homes.length)) * Math.PI * 2;
      const house = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 0.75, 0.75, 0.7),
        makeMaterial(
          THREE,
          home.door === "open"
            ? "#72d49b"
            : home.door === "knock"
              ? "#d6b66e"
              : "#6e7973",
        ),
      );
      house.position.set(Math.cos(angle) * 6.3, 0.48, Math.sin(angle) * 6.3);
      group.add(house);
      neighborhoodHomes.set(home.id, {
        name: home.name,
        position: new THREE.Vector3(
          neighborhood.position.x + house.position.x,
          CONTINUOUS_CITY_SURFACE_Y,
          neighborhood.position.z + house.position.z,
        ),
        angle,
      });
      const label = makeLabelSprite(
        THREE,
        home.name,
        home.door === "open" ? "open lobby" : home.door,
        "#b8e986",
      );
      label.scale.set(1.45, 0.48, 1);
      label.position.copy(house.position).add(new THREE.Vector3(0, 1.15, 0));
      group.add(label);
    });
    neighborhood.add(group);
    neighborhood.userData.publicHomes = group;
    neighborhood.userData.homesKey = key;
  }

  function updateArrivalStats(stats) {
    const face = ctx.arrivalPlaque.userData.statsFace;
    if (!face || !stats || typeof stats !== "object") return;
    const safe = {
      total: Math.max(0, Math.round(Number(stats.total) || 0)),
      today: Math.max(0, Math.round(Number(stats.today) || 0)),
      yesterdaySameTime: Math.max(
        0,
        Math.round(Number(stats.yesterdaySameTime) || 0),
      ),
      pastHour: Math.max(0, Math.round(Number(stats.pastHour) || 0)),
      pastHourYesterday: Math.max(
        0,
        Math.round(Number(stats.pastHourYesterday) || 0),
      ),
    };
    const key = JSON.stringify(safe);
    if (ctx.arrivalPlaque.userData.statsShown === key) return;
    face.material.map?.dispose?.();
    face.material.map = arrivalPlaqueTexture(THREE, safe);
    face.material.needsUpdate = true;
    ctx.arrivalPlaque.userData.statsShown = key;
  }

  function updateMemberLounge(
    members = [],
    totalCount = 0,
    leaderboardMembers = members,
    guests = 0,
  ) {
    const incomingMembers = Array.isArray(members) ? members : [];
    const loungeSignature = JSON.stringify([
      incomingMembers,
      totalCount,
      leaderboardMembers,
      guests,
      ctx.localBenchName,
      [...ctx.administrativelyRemovedAccounts],
      ctx.remotePlayers.size,
      ctx.officeParticipants.size,
    ]);
    if (loungeSignature === memberLoungeSignature) return;
    memberLoungeSignature = loungeSignature;
    members = incomingMembers.filter(
      (member) =>
        !ctx.administrativelyRemovedAccounts.has(
          String(member?.name || "").trim().toLowerCase(),
        ),
    );
    leaderboardMembers = (
      Array.isArray(leaderboardMembers) ? leaderboardMembers : []
    ).filter(
      (member) =>
        !ctx.administrativelyRemovedAccounts.has(
          String(member?.name || "").trim().toLowerCase(),
        ),
    );
    const removedCount = incomingMembers.length - members.length;
    const total = Math.max(
      0,
      Math.min(999999, (Number(totalCount) || 0) - removedCount),
    );
    const rosterNames = new Set();
    const rankedRoster = rankedMembersByEngagement(
      members.filter((member) => {
        const name = String(member?.name || "").trim().toLowerCase();
        if (!name || rosterNames.has(name)) return false;
        rosterNames.add(name);
        return true;
      }),
    );
    const rosterMembers = [
      ...rankedRoster.filter(memberHasCustomAvatar),
      ...rankedRoster.filter((member) => !memberHasCustomAvatar(member)),
    ];
    const engagementCounts = Object.fromEntries(
      MEMBER_ENGAGEMENT_STATES.map((state) => [state.id, 0]),
    );
    rosterMembers.forEach((member) => {
      engagementCounts[member.engagement.id] += 1;
    });
    // The aquarium is the same public account directory expressed as a
    // school: present/recent users swim high, inactive users low. No account
    // names or labels are rendered on the fish themselves.
    fns.queueLoungeTask("aquarium", () => fns.setOfficeAquariumUsers(members));
    // The approach sign carries the headline count, lifecycle totals, and the
    // name of whoever joined last so the roster is readable at a glance.
    fns.setCampfireMemberCount(
      total,
      newestMemberName(members),
      engagementCounts,
    );
    // Refresh the badge facts a live presence frame cannot carry, so the same
    // account reads identically in the circle, on its walking avatar, and in
    // an office meeting.
    ctx.memberFacts.clear();
    rosterMembers.forEach((member) => {
      const name = String(member?.name || "").trim().toLowerCase();
      if (!name) return;
      const activeMs = Number(member?.totalActiveMs ?? member?.activeMs);
      const joinedAt = Math.max(0, Number(member?.createdAt) || 0);
      const countryCode = /^[A-Z]{2}$/.test(
        String(member?.countryCode || "").toUpperCase(),
      )
        ? String(member.countryCode).toUpperCase()
        : "";
      ctx.memberFacts.set(name, {
        joinedAt,
        countryCode,
        // The public account avatar rides with the rest of the directory
        // facts, so every figure this account owns — walking avatar, office
        // attendee, bench figure — can wear the same face without a click.
        avatar: String(member?.avatar || ""),
        flag:
          String(member?.flag || "") ||
          (countryCode ? flagEmoji(countryCode) : "◌"),
        browser: String(member?.browser || ""),
        os: String(member?.os || ""),
        firstSeenMinutes: firstSeenMinutesBucket(joinedAt),
        firstVisitAge: joinedAt > 0 ? "this-session" : "hidden",
        visitCount: Math.max(
          0,
          Math.min(999, Number(member?.visitCount) || 0),
        ),
        officeVisits: Math.max(
          0,
          Math.min(999, Number(member?.officeVisits) || 0),
        ),
        totalActiveMs:
          Number.isFinite(activeMs) && activeMs >= 0 ? activeMs : null,
        totalSteps: Number.isSafeInteger(Number(member?.totalSteps))
          ? Math.max(0, Number(member.totalSteps))
          : 0,
        emailVerified: member?.emailVerified === true,
        solana: AVATAR_SOLANA_ADDRESS_RE.test(String(member?.solana || ""))
          ? String(member.solana)
          : "",
        contributionTotals: {
          issues: Math.max(0, Number(member?.issues) || 0),
          pulls: Math.max(0, Number(member?.pulls) || 0),
          commits: Math.max(0, Number(member?.commits) || 0),
          discussions: Math.max(0, Number(member?.discussions) || 0),
        },
        achievementBadges: Array.isArray(member?.achievementBadges)
          ? member.achievementBadges.slice(0, 5)
          : [],
        engagementId: member.engagement.id,
        engagementLabel: member.engagement.label,
        engagementScore: member.engagement.score,
        // The directory is authoritative for a registered account's mail
        // stamp, so a member listed here never reads as "NOT SHARED": no
        // stamp means the account has simply never been emailed.
        lastEmailAt: Math.max(0, Number(member?.lastEmailAt) || 0),
        lastEmailStatus: String(member?.lastEmailStatus || ""),
        lastEmailPrivate: false,
      });
    });
    // A live presence frame can arrive before the public directory catches up.
    // Repaint every already-created member avatar now that the full directory
    // facts are available, rather than waiting for another movement frame.
    // This includes the local player and office figures, which do not appear
    // in remotePlayers but should wear the same earned sleeve marks.
    const refreshAvatarMemberFacts = (
      avatar,
      fallbackIdentity = {},
      remote = true,
    ) => {
      const enriched = fns.withMemberFacts(
        avatar?.userData?.badgeIdentity || fallbackIdentity,
      );
      const badgeKey = JSON.stringify(enriched);
      if (!avatar?.userData?.badge || avatar.userData.badgeKey === badgeKey) {
        return;
      }
      updateAvatarBadge(THREE, avatar, enriched, remote);
      syncOperatorBelt(THREE, avatar, enriched.nodes || []);
      avatar.userData.badgeKey = badgeKey;
    };
    fns.queueLoungeTask("member-facts", () => {
      refreshAvatarMemberFacts(ctx.player, identity, false);
      refreshAvatarMemberFacts(ctx.officeLobbyPlayer, identity, false);
      ctx.remotePlayers.forEach((avatar) => refreshAvatarMemberFacts(avatar));
      ctx.officeParticipants.forEach((avatar) => refreshAvatarMemberFacts(avatar));
      // The directory that just landed is where the public faces come from, so
      // every figure already standing in the world puts one on now.
      fns.dressMemberFace(ctx.player, identity);
      fns.dressMemberFace(ctx.officeLobbyPlayer, identity);
      ctx.remotePlayers.forEach((avatar) =>
        fns.dressMemberFace(avatar, avatar.userData.badgeIdentity || {}),
      );
      ctx.officeParticipants.forEach((avatar) =>
        fns.dressMemberFace(avatar, avatar.userData.badgeIdentity || {}),
      );
      ctx.loungeMembers.forEach((avatar) =>
        fns.dressMemberFace(avatar, avatar.userData.badgeIdentity || {}),
      );
    });
    ctx.leaderboardCardState.members = leaderboardMembers;
    fns.queueLoungeTask("leaderboard", fns.repaintActiveLeaderboardCard);

    // Every member the public directory lists owns a named bench in the rings,
    // and the open benches past the roster are there for arriving guests. The
    // rings grow to hold the whole roster rather than showing the first
    // ring-load and hiding the rest. A member out walking the world as a live
    // avatar leaves their own bench empty with their name still on it;
    // everyone else is sitting on theirs.
    {
      // One spare bench for every guest present without a users-table row, and
      // never fewer than one, so an arriving visitor always finds a seat of
      // their own by the fire instead of a stranger's plank.
      const guestBenches = Math.max(
        1,
        Math.min(64, Math.round(Number(guests) || 0)),
      );
      const layout = fns.buildMemberCircleLayout(rosterMembers, guestBenches);
      const seatOffsets = fns.rebuildMemberCircleBenches(layout) || [];
      fns.rebuildMemberEngagementBands(layout);
      const places = ctx.campfire.userData.seatCount || seatOffsets.length;
      // The pavement is laid for the rings inside the clearing; a roster that
      // outgrows them sits on dirt that widens to meet it instead of out on
      // the grass. Ranked places are handed out in lifecycle order, so the
      // coldest occupied state determines the outermost member slot.
      const occupiedRadius = Math.max(
        0,
        ...seatOffsets.map((offset) => Math.hypot(offset.x, offset.z)),
        ...layout.memberSlots.map((slot) => slot.position.length()),
      );
      // The margin matches the strip of dirt the clearing was built with
      // behind its own outermost ring, so a roster that fits inside the
      // pavement leaves it exactly as designed.
      const groundRadius = Math.max(
        ctx.MEMBER_CLEARING_RADIUS + 0.8,
        occupiedRadius + 0.9,
      );
      ctx.campfireGround.scale.setScalar(groundRadius);
      ctx.memberCircleInfo.position.z = -groundRadius - 1.1;
      ctx.startHereBoard.position.z =
        MEMBER_ISLAND_CENTER_Z + Math.max(38, groundRadius + 10);
      const seen = new Set();
      // Which plank belongs to whom, so a visitor arriving by the world map
      // lands on their own bench and a guest lands on the first open one.
      const seatByName = new Map();
      ctx.campfire.userData.seatByName = seatByName;
      ctx.campfire.userData.openSeatIndex = Math.min(
        rosterMembers.length,
        Math.max(0, places - 1),
      );
      const loungeJobs = [];
      rosterMembers.forEach((member, index) => {
        const name = String(member.name).trim().slice(0, 32);
        if (index < places) seatByName.set(name.toLowerCase(), index);
        const id = `member:${name.toLowerCase()}`;
        // Out walking the world as a live avatar — including this browser's
        // own account, unless it is the one sitting on this very bench. The
        // plank keeps their name; only the figure on it goes.
        const away =
          member?.away === true &&
          name.toLowerCase() !== ctx.localBenchName.toLowerCase();
        fns.setCampfireSeatLabel(
          index,
          name,
          away ? "away" : "seated",
          member.engagement.id,
        );
        if (away) return;
        seen.add(id);
        loungeJobs.push({ member, index, name, id });
      });
      queueMicrotask(() => fns.notifyCampfireSeatsReady());
      const dressLoungeMember = ({ member, index, name, id }) => {
        const memberCountry = /^[A-Z]{2}$/.test(
          String(member.countryCode || "").toUpperCase(),
        )
          ? String(member.countryCode).toUpperCase()
          : "";
        const joinedAt = Math.max(0, Number(member.createdAt) || 0);
        const memberAvatar = String(member.avatar || "");
        const memberIdentity = {
          id,
          name,
          flag:
            String(member.flag || "") ||
            (memberCountry ? flagEmoji(memberCountry) : "◌"),
          countryCode: memberCountry,
          browser: String(member.browser || "Hidden"),
          os: String(member.os || "Hidden"),
          status: "gathering in the open Members Circle",
          accountStatus: "Registered",
          emailVerified: member.emailVerified === true,
          localTime: "",
          activityCategory: "visiting-members-circle",
          inputActive: false,
          visitCount: Math.max(
            0,
            Math.min(999, Number(member.visitCount) || 0),
          ),
          officeVisits: Math.max(
            0,
            Math.min(999, Number(member.officeVisits) || 0),
          ),
          firstVisitAge: joinedAt > 0 ? "this-session" : "hidden",
          firstSeenMinutes: firstSeenMinutesBucket(joinedAt),
          joinedAt,
          totalActiveMs: Math.max(
            0,
            Number(member.totalActiveMs ?? member.activeMs) || 0,
          ),
          totalSteps: Math.max(0, Number(member.totalSteps) || 0),
          nodes: Array.isArray(member.nodes) ? member.nodes.slice(0, 6) : [],
          solana: AVATAR_SOLANA_ADDRESS_RE.test(String(member.solana || ""))
            ? String(member.solana)
            : "",
          contributionTotals: {
            issues: Math.max(0, Number(member.issues) || 0),
            pulls: Math.max(0, Number(member.pulls) || 0),
            commits: Math.max(0, Number(member.commits) || 0),
            discussions: Math.max(0, Number(member.discussions) || 0),
          },
          achievementBadges: Array.isArray(member.achievementBadges)
            ? member.achievementBadges.slice(0, 5)
            : [],
          engagementId: member.engagement.id,
          engagementLabel: member.engagement.label,
          engagementScore: member.engagement.score,
          statusEmoji: "",
          statusNote: String(member.status || ""),
          faceImage: Boolean(memberAvatar),
          activityBucket: member.activityBucket || "",
          lastEmailAt: Math.max(0, Number(member.lastEmailAt) || 0),
          lastEmailStatus: String(member.lastEmailStatus || ""),
          lastEmailPrivate: false,
        };
        const badgeKey = JSON.stringify(memberIdentity);
        let figure = ctx.loungeMembers.get(id);
        if (!figure) {
          figure = createAvatar(
            THREE,
            memberIdentity,
            {
              remote: true,
              scale: MEMBER_CIRCLE_FIGURE_SCALE,
              instances: ctx.avatarPartInstances,
              plateScale: 0.5,
            },
          );
          figure.userData.badgeKey = badgeKey;
          figure.userData.memberCircleFigure = true;
          figure.visible = true;
          ctx.world.add(figure);
          fns.registerCompactDistrictRoot("members", figure);
          ctx.loungeMembers.set(id, figure);
          fns.registerWorldElement(
            "member-circle-figures",
            "Members Circle figures",
            "Avatars & bots",
            figure,
            () => ctx.loungeMembers.get(id) === figure,
          );
          fns.registerAvatarChestControls(figure, id);
          ctx.figureBatches.register(figure);
          fns.registerWorldElement(
            "member-figures",
            "Member circle figures",
            "Avatars & bots",
            figure,
            () => ctx.loungeMembers.get(id) === figure,
          );
        } else if (figure.userData.badgeKey !== badgeKey) {
          updateAvatarBadge(THREE, figure, memberIdentity, true);
          syncOperatorBelt(THREE, figure, memberIdentity.nodes);
          figure.userData.badgeKey = badgeKey;
        }
        // The public avatar arrived in the same directory response that made
        // this figure. Paint it now; selecting the member remains responsible
        // only for the richer profile/contribution card, not for revealing the
        // face that was already available.
        if (memberAvatar || figure.userData.faceImageUrl) {
          applyAvatarFaceImage(THREE, figure, memberAvatar);
        }
        syncAvatarActivity(THREE, figure, {
          inputActive: false,
          accountStatus: "Registered",
          activityBucket: member.activityBucket || "",
        });
        fns.syncRemoteOrgTeamControl(figure, {
          id,
          name,
          accountStatus: "Registered",
          orgTeam: member?.orgTeam,
        });
        figure.userData.memberEngagement = {
          id: member.engagement.id,
          label: member.engagement.label,
          score: member.engagement.score,
          order: member.engagement.order,
        };
        const seat = seatOffsets[index];
        const slot = layout.memberSlots[index]?.position || seat;
        if (!slot) return;
        // Avatar fronts face local -Z, so this heading turns each member
        // toward the central fire and leaves profile backs readable outside it.
        figure.rotation.y = Math.atan2(slot.x, slot.z);
        figure.userData.ambientInteraction = null;
        if (seat) {
          // Hips on the plank, not feet: the same placement sitOnCampfireBench
          // gives the local player, so a member and a visitor sitting side by
          // side sit at one height.
          figure.position.set(
            ctx.campfire.position.x + seat.x,
            seatedAvatarY(ctx.campfire.position.y + seat.y, figure.scale.x),
            ctx.campfire.position.z + seat.z,
          );
          applySeatedLegPose(figure);
          return;
        }
        // Past the bench ceiling there is no plank to sit on, so the figure
        // stands in its place rather than being dropped from the circle.
        figure.position.set(
          ctx.campfire.position.x + slot.x,
          ctx.campfire.position.y + CAMPFIRE_GROUND_Y,
          ctx.campfire.position.z + slot.z,
        );
        applyLegPitch(figure, 0, 0);
      };
      ctx.figureBatches.reserve(loungeJobs.length + ctx.remotePlayers.size);
      fns.scheduleLoungeFigures(loungeJobs, dressLoungeMember, (index) =>
        layout.memberSlots[index]?.position || seatOffsets[index],
      );
      // Benches past the roster are the open guest seats.
      for (let index = rosterMembers.length; index < places; index += 1) {
        fns.setCampfireSeatLabel(index, "", "open");
      }
      fns.queueLoungeTask("seat-labels", fns.repaintCampfireSeatLabels);
      ctx.loungeMembers.forEach((figure, id) => {
        if (seen.has(id)) return;
        fns.removeRemoteOrgTeamControl(figure, id);
        fns.unregisterAvatarChestControls(figure);
        ctx.figureBatches.release(figure);
        ctx.world.remove(figure);
        fns.unregisterCompactDistrictRoot("members", figure);
        disposeObject3D(figure);
        ctx.loungeMembers.delete(id);
      });
      ctx.campfire.userData.memberFigureCount = seen.size;
      ctx.campfire.userData.detailedMemberFigures = seen.size;
      ctx.campfire.userData.seatedMemberFigures = seen.size;
      return;
    }
  }

  function visitNeighborhoodHome(ownerId) {
    const home = neighborhoodHomes.get(String(ownerId || ""));
    if (!home) return false;
    const destination = home.position.clone();
    destination.x += Math.cos(home.angle) * 1.15;
    destination.z += Math.sin(home.angle) * 1.15;
    ctx.currentSpace = "town-square";
    ctx.currentFloorY = fns.outdoorFloorYAt(destination.x, destination.z);
    destination.y = ctx.currentFloorY;
    ctx.player.position.copy(destination);
    ctx.cameraFocus = null;
    fns.cancelDash();
    ctx.focusedRepositoryKey = "";
    ctx.currentLocation = `${home.name}'s front yard`;
    onLocationChange(ctx.currentLocation, "neighborhood");
    onMovement({
      x: destination.x,
      y: destination.y,
      z: destination.z,
      heading: ctx.player.rotation.y,
      activity: "visiting a consented front yard",
      space: "town-square",
      moving: false,
    });
    return true;
  }

  // Concentric complete rings around the centred SOL board. Each populated
  // ring distributes its current members at equal angular intervals; a join
  // or deletion deliberately reflows that ring so gaps never accumulate.
  function rewardCircleSlots(centreX, centreZ, count) {
    const minimumSpacing = 3.65;
    const ringGap = 4.25;
    const requested = Math.max(0, Math.min(64, Math.round(Number(count) || 0)));
    const slots = [];
    let remaining = requested;
    let radius = 11.75;
    let ringIndex = 0;
    while (remaining > 0 && radius < Math.min(68, WORLD_RADIUS - 6)) {
      const capacity = Math.max(
        1,
        Math.floor((Math.PI * 2 * radius) / minimumSpacing),
      );
      const ringCount = Math.min(remaining, capacity);
      const angleStep = (Math.PI * 2) / ringCount;
      // Offset alternate rings so their cabinets do not line up radially.
      const phase =
        Math.PI / 4 +
        Math.PI / ringCount +
        (ringIndex % 2) * angleStep * 0.5;
      for (let index = 0; index < ringCount; index += 1) {
        const angle = phase + index * angleStep;
        slots.push({
          x: centreX + Math.cos(angle) * radius,
          z: centreZ + Math.sin(angle) * radius,
        });
      }
      remaining -= ringCount;
      radius += ringGap;
      ringIndex += 1;
    }
    return slots;
  }

  function deleteNetworkNode(target = {}) {
    const identifiers = new Set(
      [
        target?.name,
        target?.machineName,
        target?.nodeId,
        ...(Array.isArray(target?.identifiers) ? target.identifiers : []),
      ]
        .map((value) => String(value || "").trim().toLowerCase())
        .filter(Boolean),
    );
    const matches = [];
    ctx.nodeInfrastructure.forEach((candidate, id) => {
      const record = candidate?.userData?.nodeRecord || {};
      const candidateIds = [
        id.replace(/^node:/, ""),
        record.name,
        record.machineName,
        record.nodeId,
      ].map((value) => String(value || "").trim().toLowerCase());
      if (candidateIds.some((value) => identifiers.has(value))) {
        candidateIds.filter(Boolean).forEach((value) => identifiers.add(value));
        matches.push([id, candidate]);
      }
    });
    identifiers.forEach((value) => ctx.deletedNodeIdentifiers.add(value));
    matches.forEach(([matchId, cabinet]) => {
      ctx.nodeInfrastructure.delete(matchId);
      cabinet.traverse((child) => {
        if (!child.userData?.nodeCabinet) return;
        const index = ctx.interactive.indexOf(child);
        if (index >= 0) ctx.interactive.splice(index, 1);
        if (child.material?.emissive) {
          child.material.emissive.set("#ff3b4f");
          child.material.emissiveIntensity = 1.8;
        }
      });
      const origin = cabinet.position.clone();
      const shockwave = new THREE.Mesh(
        resources.primitive(THREE, "RingGeometry", 0.8, 1.2, 48),
        resources.uniqueMaterial(THREE, {
          color: "#ff5d6c",
          transparent: true,
          opacity: 0.92,
          depthWrite: false,
          side: THREE.DoubleSide,
          forceSinglePass: true,
        }, "MeshBasicMaterial"),
      );
      shockwave.name = `forkmesh-node-delete-effect:${matchId}`;
      shockwave.rotation.x = -Math.PI / 2;
      shockwave.position.set(origin.x, 0.24, origin.z);
      ctx.nodeInterior.add(shockwave);
      const startedAt = performance.now();
      const duration = 720;
      const animateDeletion = (time) => {
        const progress = clamp((time - startedAt) / duration, 0, 1);
        const scale = Math.max(0.001, 1 - progress ** 1.4);
        cabinet.scale.set(scale, scale * (1 + progress * 0.8), scale);
        cabinet.position.y = origin.y + Math.sin(progress * Math.PI) * 2.8;
        cabinet.rotation.y += 0.12;
        shockwave.scale.setScalar(1 + progress * 18);
        shockwave.material.opacity = 0.92 * (1 - progress);
        if (progress < 1) return;
        const callbackIndex = ctx.animated.indexOf(animateDeletion);
        if (callbackIndex >= 0) ctx.animated.splice(callbackIndex, 1);
        ctx.nodeInterior.remove(cabinet, shockwave);
        disposeObject3D(cabinet);
        shockwave.geometry.dispose();
        shockwave.material.dispose();
      };
      ctx.animated.push(animateDeletion);
    });
    if (matches.length) fns.relayoutNetworkNodes();
    return matches.length > 0;
  }

  return {
    setRemotePlayers,
    setRemotePlayerMovement,
    updateArrivalStats,
    updateMemberLounge,
    visitNeighborhoodHome,
    rewardCircleSlots,
    deleteNetworkNode,
  };
}
