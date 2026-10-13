import { AVATAR_SOLANA_ADDRESS_RE } from "./avatar-shared.js";
import {
  canvasTexture,
  canvasTextureScale,
  roundedRect,
  sharpenCanvasTextTexture,
  softwareCanvas,
} from "./canvas.js";
import { drawQrModules } from "./treasury.js";

const memberCardTexture = (...args) =>
  softwareCanvas(() => canvasTexture(...args));
import * as resources from "./resources.js";

const ACCOUNT_STATUS_ICONS = Object.freeze({
  Guest: "○",
  Registered: "✓",
  "Supporting member": "♥",
  "Mirror operator": "◈",
  "Organization admin": "◆",
  "Verified bot": "⌘",
});

function accountStatusIcon(identity) {
  const status = String(identity?.accountStatus || "Guest");
  if (status === "Registered" && identity?.emailVerified !== true) return "×";
  return ACCOUNT_STATUS_ICONS[status] || "○";
}

// The avatar chest card is the one plate in the World written to be read
// rather than glanced at: fourteen rows of 14-17px type in a 512-unit layout
// on a plane a visitor walks up to and studies. Sent through the reductions
// above it lost that reading: an oversized desktop surface halved it, a
// directory figure's plateScale halved it again, and a phone quartered it, so
// the card a visitor stood in front of held a 128px — on mobile a 92px —
// backing store for a 512-unit layout, and its rows arrived as mush. Chest
// text therefore gets its own ladder on top of the memory budget, bounded by
// how many cards can be read at once rather than by shrinking every card in
// the World.
//
// The plate's world height: its geometry is square at 0.88 and the mesh
// stretches the vertical by 1.12 (see createAvatar).
const BADGE_CARD_WORLD_HEIGHT = 0.88 * 1.12;
const BADGE_TEXTURE_READING_SCALE = 2;
const BADGE_TEXTURE_READING_SCALE_COMPACT = 1;
// Below this the card is scenery — about a fifth of a 1080p viewport — and
// its resolution is not what limits legibility.
const BADGE_READING_MIN_PIXELS = 220;
// Hysteresis, so a visitor pacing around a group cannot repaint the same
// canvas on every sample.
const BADGE_READING_EXIT_RATIO = 0.7;
// Six supersampled cards is 24 MB of backing store on a renderer that already
// halved its plates, and more than a visitor can read at once anyway.
const BADGE_READING_LIMIT = 6;
// Walking speed against the hysteresis band above: often enough that a card
// is sharp by the time the visitor has turned to face it.
export const BADGE_DETAIL_SAMPLE_MS = 250;
export const BADGE_DETAIL_UPGRADES_PER_SAMPLE = 2;

/**
 * Backing-store scale for one chest card, or 0 to keep whatever the memory
 * budget already chose. Exported pure so the device matrix can check the
 * reading resolution without a GPU: `canvasScale` is the renderer-wide
 * reduction, and a 512-unit layout needs a 1024px store to still be sharp at
 * the closest a visitor can stand to a card.
 */
export function badgeTextTextureScale(detail, canvasScale = 1) {
  if (detail !== "reading") return 0;
  return canvasScale <= 0.25
    ? BADGE_TEXTURE_READING_SCALE_COMPACT
    : BADGE_TEXTURE_READING_SCALE;
}

function badgeTextureScale(detail) {
  return badgeTextTextureScale(detail, canvasTextureScale);
}

/**
 * How tall one chest card will be drawn, in device pixels. Distance alone
 * cannot answer this: the World's third-person camera stands some thirty
 * metres behind its own avatar, and zoom, field of view, window size and
 * device pixel ratio each move the answer by more than walking does.
 */
export function badgeCardDrawnPixels({
  distance = 0,
  fov = 60,
  viewportPixels = 0,
  scale = 1,
} = {}) {
  const span =
    2 *
    Math.max(0.001, Number(distance) || 0) *
    Math.tan(((Number(fov) || 60) * Math.PI) / 360);
  return (
    (BADGE_CARD_WORLD_HEIGHT *
      Math.max(0, Number(scale) || 0) *
      Math.max(0, Number(viewportPixels) || 0)) /
    span
  );
}

/**
 * Which chest cards have earned the reading resolution. A card earns it by
 * being drawn taller than the backing store the memory budget gave it — the
 * point where its text is being magnified rather than sampled — and the
 * largest few on screen win, because that is as many as a visitor can read at
 * once. Pure, and separate from the repaint, so the crowd decision is
 * testable without a scene.
 */
export function badgeReadingSelection(cards = [], limit = BADGE_READING_LIMIT) {
  return cards
    .filter((card) => card && card.visible !== false)
    .filter((card) => {
      const threshold = Math.max(
        BADGE_READING_MIN_PIXELS,
        Number(card.farPixels) || 0,
      );
      const drawn = Number(card.drawnPixels) || 0;
      return card.reading === true
        ? drawn >= threshold * BADGE_READING_EXIT_RATIO
        : drawn >= threshold;
    })
    .sort(
      (left, right) =>
        (Number(right.drawnPixels) || 0) - (Number(left.drawnPixels) || 0),
    )
    .slice(0, Math.max(0, limit))
    .map((card) => card.key);
}

// Directory figures carry the account's public joined timestamp and coarse
// total active time. A live avatar is handed the same directory reading, so a
// member wears the identical record whether they are walking around or seated.
function joinedAgoLabel(joinedAt, now = Date.now()) {
  const timestamp = Number(joinedAt);
  if (!Number.isFinite(timestamp) || timestamp <= 0) return "";
  const elapsed = Math.max(0, now - timestamp);
  const units = [
    [365 * 24 * 60 * 60 * 1000, "Y"],
    [30 * 24 * 60 * 60 * 1000, "MO"],
    [7 * 24 * 60 * 60 * 1000, "W"],
    [24 * 60 * 60 * 1000, "D"],
    [60 * 60 * 1000, "H"],
    [60 * 1000, "M"],
  ];
  for (const [size, unit] of units) {
    const count = Math.floor(elapsed / size);
    if (count) return `JOINED ${count}${unit} AGO`;
  }
  return "JOINED JUST NOW";
}

function badgeActiveDurationLabel(value) {
  const minutes = Math.floor(Math.max(0, Number(value) || 0) / 60000);
  const days = Math.floor(minutes / 1440);
  const hours = Math.floor((minutes % 1440) / 60);
  if (days) return `${days}D ${hours}H`;
  if (hours) return `${hours}H ${minutes % 60}M`;
  return `${minutes % 60}M`;
}

// "FIRST SEEN 14 MINUTES AGO". The exact reading only exists while the visitor
// shares generalized activity; the coarse bucket label remains the fallback.
// Minutes-since-join quantized to the unit firstSeenAgoLabel displays. The
// raw minute count feeds cached badge keys, and un-quantized it ticked for
// every member on the same minute boundary — repainting every avatar badge
// canvas in a single frame once a minute.
export function firstSeenMinutesBucket(joinedAt) {
  if (!(Number(joinedAt) > 0)) return 0;
  const total = Math.max(0, Math.floor((Date.now() - joinedAt) / 60_000));
  for (const size of [365 * 24 * 60, 30 * 24 * 60, 7 * 24 * 60, 24 * 60, 60]) {
    if (total >= size) return Math.floor(total / size) * size;
  }
  return total;
}

function firstSeenAgoLabel(minutes) {
  const total = Number(minutes);
  if (!Number.isFinite(total) || total < 0) return "";
  if (total === 0) return "FIRST SEEN JUST NOW";
  const units = [
    [365 * 24 * 60, "Y"],
    [30 * 24 * 60, "MO"],
    [7 * 24 * 60, "W"],
    [24 * 60, "D"],
    [60, "H"],
    [1, "M"],
  ];
  for (const [size, unit] of units) {
    const count = Math.floor(total / size);
    if (count) {
      return `FIRST SEEN ${count}${unit} AGO`;
    }
  }
  return "";
}

// The visitor's own browser family and operating system, as the two coarse
// categories world.js derived locally — never a raw user-agent string.
function badgeClientLabel(identity) {
  // "Hidden" is a privacy choice and "Browser"/"Device" are the placeholders a
  // peer carries when its family is unrecognized: neither is worth a row.
  const placeholders = new Set(["hidden", "browser", "device"]);
  const parts = [identity.browser, identity.os]
    .map((value) => String(value || "").trim())
    .filter((value) => value && !placeholders.has(value.toLowerCase()));
  return parts.length ? parts.join(" · ").toUpperCase().slice(0, 30) : "";
}

function badgeStatusLabel(identity) {
  const note = String(identity.statusNote || "").trim();
  const emoji = String(identity.statusEmoji || "").trim();
  if (!emoji) return "";
  return `${emoji} ${note}`.trim().slice(0, 24);
}

function badgeEmailLabel(identity) {
  const sentAt = Math.max(0, Number(identity.lastEmailAt) || 0);
  if (!sentAt) return "";
  const elapsed = Math.max(0, Date.now() - sentAt);
  const units = [
    [365 * 24 * 60 * 60 * 1000, "Y"],
    [30 * 24 * 60 * 60 * 1000, "MO"],
    [24 * 60 * 60 * 1000, "D"],
    [60 * 60 * 1000, "H"],
    [60 * 1000, "M"],
  ];
  let ago = "NOW";
  for (const [size, label] of units) {
    const count = Math.floor(elapsed / size);
    if (count) {
      ago = `${count}${label} AGO`;
      break;
    }
  }
  const status = String(identity.lastEmailStatus || "accepted")
    .replace(/[^a-z ]/gi, "")
    .trim()
    .toUpperCase()
    .slice(0, 16);
  return `EMAIL ${ago} · ${status || "ACCEPTED"}`;
}

export function badgeTexture(
  THREE,
  identity,
  accent = "#9ef7c6",
  profile = {},
  plateScale = 1,
  detail = "far",
) {
  const activityLabels = {
    "browsing-code-visualization": "CODE MAP",
    "exploring-town-square": "TOWN SQUARE",
    "reading-documentation": "DOCUMENTATION",
    "viewing-repository": "REPOSITORY",
    "visiting-organization": "ORGANIZATION",
    "sitting-at-member-fire": "MEMBER FIRE",
    hidden: "ACTIVITY HIDDEN",
  };
  const firstSeenLabels = {
    "this-session": "FIRST SEEN THIS SESSION",
    today: "FIRST SEEN TODAY",
    "this-week": "FIRST SEEN THIS WEEK",
    "this-month": "FIRST SEEN THIS MONTH",
    "this-year": "FIRST SEEN THIS YEAR",
    "over-a-year": "FIRST SEEN 1Y+ AGO",
    hidden: "FIRST SEEN HIDDEN",
  };
  const texture = memberCardTexture(THREE, 512, 512, (context) => {
    context.fillStyle = "#0c2019";
    context.fillRect(0, 0, 512, 512);
    // Activity belongs around the avatar portrait, not around this information
    // panel. Keep a quiet structural edge here so the two signals cannot be
    // confused.
    context.strokeStyle = "rgba(158,247,198,0.5)";
    context.lineWidth = 5;
    context.strokeRect(8, 8, 496, 496);

    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '62px system-ui, "Apple Color Emoji", "Segoe UI Emoji"';
    context.fillStyle = "#ffffff";
    context.fillText(identity.flag || "◌", 54, 62);

    context.textAlign = "left";
    context.font = '800 34px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffffff";
    context.fillText(String(identity.name || "guest").slice(0, 13), 94, 54);
    context.font = '700 18px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#a9b8ff";
    context.fillText(
      String(profile.handle || `@${identity.name || "guest"}`).slice(0, 27),
      94,
      82,
    );
    context.fillStyle = "#9ef7c6";
    context.fillText(
      profile.state === "ready"
        ? `${Math.max(0, Number(profile.followers) || 0)} FOLLOWERS · ${
            Math.max(0, Number(profile.following) || 0)
          } FOLLOWING`
        : profile.state === "unavailable"
          ? "FEDIVERSE · UNAVAILABLE"
          : "FEDIVERSE · LOADING",
      94,
      106,
    );

    const walletAddress = AVATAR_SOLANA_ADDRESS_RE.test(
      String(identity.solana || ""),
    )
      ? String(identity.solana)
      : "";
    context.setLineDash(walletAddress ? [] : [7, 6]);
    context.strokeStyle = walletAddress ? "#f7c96b" : "#708078";
    context.lineWidth = 4;
    context.strokeRect(376, 12, 124, 124);
    context.setLineDash([]);
    if (walletAddress) {
      drawQrModules(context, walletAddress, 384, 20, 108);
    } else {
      context.textAlign = "center";
      context.fillStyle = "#708078";
      context.font = '900 28px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("+", 438, 57);
      context.font = '700 11px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        identity.walletEditable ? "ADD WALLET" : "NO WALLET",
        438,
        96,
      );
    }
    context.textAlign = "center";
    context.fillStyle = "#f7c96b";
    context.font = '700 14px "ForkMesh Mono", ui-monospace, monospace';
    const walletSol = Number(identity.walletSol);
    context.fillText(
      walletAddress && Number.isFinite(walletSol)
        ? `${walletSol.toLocaleString("en-US", {
            maximumFractionDigits: 4,
          })} SOL`
        : "",
      438,
      148,
    );
    const nodes = Array.isArray(identity.nodes) ? identity.nodes.slice(0, 6) : [];
    nodes.forEach((node, index) => {
      const status = String(
        typeof node === "object"
          ? node?.health || node?.status || ""
          : "",
      ).toLowerCase();
      const color = ["online", "healthy", "available"].includes(status)
        ? "#22e06a"
        : ["warning", "degraded", "stale"].includes(status)
          ? "#ffd23f"
          : ["offline", "failed", "error"].includes(status)
            ? "#ff4d57"
            : "#8da09a";
      context.beginPath();
      context.fillStyle = color;
      context.arc(112 + index * 25, 132, 8, 0, Math.PI * 2);
      context.fill();
      context.strokeStyle = "#dffff1";
      context.lineWidth = 2;
      context.stroke();
    });
    const socialDot = (x, color, glyph) => {
      context.beginPath();
      context.fillStyle = color;
      context.arc(x, 132, 9, 0, Math.PI * 2);
      context.fill();
      context.fillStyle = "#082018";
      context.font = '900 10px "ForkMesh Mono", ui-monospace, monospace';
      context.textAlign = "center";
      context.fillText(glyph, x, 132);
    };
    // A compact handful of follower/following people makes the relationship
    // counts visual without turning the chest into another text-only table.
    [270, 294].forEach((x) => socialDot(x, "#77d9ff", "←"));
    [326, 350, 374].forEach((x) => socialDot(x, "#9ef7c6", "→"));
    context.textAlign = "left";
    context.strokeStyle = "rgba(158,247,198,0.28)";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(30, 154);
    context.lineTo(482, 154);
    context.stroke();

    const joined = joinedAgoLabel(identity.joinedAt);
    const firstSeen =
      firstSeenAgoLabel(identity.firstSeenMinutes) ||
      firstSeenLabels[identity.firstVisitAge] ||
      // A directory figure carries a joined date but no live first-seen
      // reading; leave the row out instead of stating "hidden" twice.
      (joined ? "" : firstSeenLabels.hidden);
    const activity =
      activityLabels[identity.activityCategory] || activityLabels.hidden;
    const visits = Math.max(0, Math.min(999, Number(identity.visitCount) || 0));
    // Every reading the badge holds gets its own row. The world active-time
    // aggregate no longer replaces the shared activity line, so nobody's chest
    // shows less than what is publicly known about them.
    const activeTime =
      identity.totalActiveMs != null &&
      Number.isFinite(Number(identity.totalActiveMs))
        ? `ACTIVE ${badgeActiveDurationLabel(identity.totalActiveMs)} IN WORLD`
        : "";
    const stepTotal = Number(identity.totalSteps);
    const stepCount =
      identity.totalSteps != null && Number.isSafeInteger(stepTotal)
        ? `${Math.max(0, stepTotal).toLocaleString("en-US")} STEPS`
        : "";
    const activeRow = [activeTime, stepCount].filter(Boolean).join(" · ");
    const sharesActivity =
      Boolean(identity.activityCategory) &&
      identity.activityCategory !== "hidden";
    const rows = [
      [firstSeen, "#9ef7c6"],
      [joined, "#77d9ff"],
      [badgeClientLabel(identity), "#f7c96b"],
      [activeRow, "#9ef7c6"],
      [badgeEmailLabel(identity), "#77d9ff"],
      // A directory figure publishes no live activity, so its "hidden · 0
      // visits" line is the one row worth dropping once the active-time row
      // carries a real reading.
      [
        sharesActivity || !activeRow
          ? `${activity} · ${visits} PUBLIC URL VISITS`
          : "",
        "#b9cfc4",
      ],
    ].filter(([text]) => text);
    // Five rows still have to clear the status pill at y=374.
    const step = rows.length > 5 ? 22 : rows.length > 4 ? 25 : 29;
    context.textAlign = "left";
    context.font = '700 17px "ForkMesh Mono", ui-monospace, monospace';
    rows.forEach(([text, color], index) => {
      context.fillStyle = color;
      context.fillText(text, 34, 181 + index * step, 448);
    });

    const posts = Array.isArray(profile.posts) ? profile.posts.slice(0, 1) : [];
    context.fillStyle = "#a9b8ff";
    context.font = '800 15px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("RECENT FEDIVERSE", 34, 309);
    context.font = '600 14px "ForkMesh Mono", ui-monospace, monospace';
    posts.forEach((post, index) => {
      context.fillStyle = index ? "#93a4c8" : "#dfe8ff";
      context.fillText(String(post).slice(0, 48), 34, 330 + index * 23);
    });
    const recentPublicMessage = String(
      identity.recentPublicMessage || "",
    ).replace(/\s+/g, " ").trim().slice(0, 56);
    context.fillStyle = "#77d9ff";
    context.font = '800 15px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("RECENT PUBLIC CHAT", 34, 356);
    context.fillStyle = recentPublicMessage ? "#d9f7ff" : "#708078";
    context.font = '600 14px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      recentPublicMessage || "NO RECENT PUBLIC MESSAGE",
      34,
      378,
    );

    const lastEmailAt = Math.max(0, Number(identity.lastEmailAt) || 0);
    const lastEmailStatus = String(identity.lastEmailStatus || "").toLowerCase();
    const emailLabel = lastEmailAt
      ? `LAST EMAIL ${joinedAgoLabel(lastEmailAt).replace(/^JOINED /, "")} · ${
          lastEmailStatus === "delivered" ? "DELIVERED" : "FAILED"
        }`
      : identity.lastEmailPrivate === false
        ? "LAST EMAIL NEVER · NO DELIVERY"
        : "LAST EMAIL · NOT SHARED";
    context.fillStyle =
      lastEmailStatus === "failed" ? "#ff6b72" : "#f7c96b";
    context.font = '800 16px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(emailLabel.slice(0, 43), 34, 410);
    const frontMood = badgeStatusLabel(identity);
    if (frontMood) {
      context.fillStyle = "#eafff2";
      context.font = '800 17px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(`MOOD · ${frontMood}`.slice(0, 27), 34, 435);
    }

    context.textAlign = "left";
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#b9cfc4";
    const account = String(identity.accountStatus || "Guest")
      .replace(/\s+/g, " ")
      .slice(0, 18);
    context.fillText(
      [
        `${accountStatusIcon(identity)} ${account}`,
        identity.localTime,
      ].filter(Boolean).join(" · "),
      52,
      468,
    );

    const followLabel = fediverseFollowLabel(profile);
    if (followLabel) {
      roundedRect(context, 300, 442, 184, 42, 21);
      context.fillStyle = "rgba(169,184,255,0.18)";
      context.fill();
      context.strokeStyle = "#a9b8ff";
      context.lineWidth = 2;
      context.stroke();
      context.textAlign = "center";
      context.fillStyle = "#eaefff";
      context.font = '700 16px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(followLabel, 392, 463);
    }
  }, plateScale, badgeTextureScale(detail));
  return sharpenCanvasTextTexture(THREE, texture);
}

// The follow control lives inside the fediverse tab's canvas rather than on a
// separate sliver of chest: the click handler hits it by UV rectangle.
const BADGE_FOLLOW_PILL = Object.freeze({
  minU: 300 / 512,
  maxU: 484 / 512,
  minV: 1 - 484 / 512,
  maxV: 1 - 438 / 512,
});
const BADGE_WALLET_SQUARE = Object.freeze({
  minU: 370 / 512,
  maxU: 506 / 512,
  minV: 1 - 154 / 512,
  maxV: 1 - 8 / 512,
});

export function badgeFollowPillHit(uv) {
  return Boolean(
    uv &&
      uv.x >= BADGE_FOLLOW_PILL.minU &&
      uv.x <= BADGE_FOLLOW_PILL.maxU &&
      uv.y >= BADGE_FOLLOW_PILL.minV &&
      uv.y <= BADGE_FOLLOW_PILL.maxV,
  );
}

export function badgeWalletSquareHit(uv) {
  return Boolean(
    uv &&
      uv.x >= BADGE_WALLET_SQUARE.minU &&
      uv.x <= BADGE_WALLET_SQUARE.maxU &&
      uv.y >= BADGE_WALLET_SQUARE.minV &&
      uv.y <= BADGE_WALLET_SQUARE.maxV,
  );
}

function fediverseFollowLabel(profile) {
  if (profile.state !== "ready") return "";
  if (profile.self === true) return "THIS IS YOU";
  if (profile.canFollow !== true) return "SIGN IN TO FOLLOW";
  if (profile.pending === true) return "…";
  return profile.isFollowing === true ? "✓ FOLLOWING" : "+ FOLLOW";
}

/**
 * The second chest tab: the account's public ForkMesh profile, the activity
 * ForkMesh federates for it, the fediverse handle its owner published, and a
 * follow control. Every value comes from public /api/accounts/{name} data.
 */
function fediverseBadgeTexture(THREE, identity, accent, profile = {}) {
  const state = String(profile.state || "loading");
  const posts = Array.isArray(profile.posts) ? profile.posts.slice(0, 5) : [];
  return memberCardTexture(THREE, 512, 512, (context) => {
    context.fillStyle = "#101724";
    context.fillRect(0, 0, 512, 512);
    context.strokeStyle = accent;
    context.lineWidth = 12;
    context.strokeRect(8, 8, 496, 496);

    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#a9b8ff";
    context.fillText("FEDIVERSE", 256, 52);

    context.font = '700 27px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffffff";
    context.fillText(
      String(profile.handle || `@${identity.name || "guest"}`).slice(0, 24),
      256,
      96,
    );

    let cursor = 130;
    // ForkMesh federates repositories, not accounts, so the fediverse address
    // shown here is the one this account published on its own profile.
    const fediverse = String(profile.fediverse || "").trim();
    if (fediverse) {
      context.font = '400 19px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#a9b8ff";
      context.fillText(fediverse.slice(0, 30), 256, cursor);
      cursor += 30;
    }
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#9ef7c6";
    context.fillText(
      state === "ready"
        ? `${Math.max(0, Number(profile.followers) || 0)} FOLLOWERS · ` +
            `${Math.max(0, Number(profile.following) || 0)} FOLLOWING`
        : state === "unavailable"
          ? "NO PUBLIC PROFILE"
          : "LOADING…",
      256,
      cursor,
    );
    cursor += 26;

    context.beginPath();
    context.moveTo(48, cursor);
    context.lineTo(464, cursor);
    context.strokeStyle = "rgba(169,184,255,0.35)";
    context.lineWidth = 2;
    context.stroke();
    cursor += 30;

    context.font = '400 19px "ForkMesh Mono", ui-monospace, monospace';
    const bio = String(profile.bio || "").trim();
    if (bio) {
      context.fillStyle = "#c9d6ff";
      context.fillText(bio.slice(0, 34), 256, cursor);
      cursor += 40;
    }
    if (posts.length) {
      posts.forEach((post, index) => {
        context.fillStyle = index % 2 ? "#93a4c8" : "#dfe8ff";
        context.fillText(String(post).slice(0, 36), 256, cursor + index * 30);
      });
    } else if (state === "ready") {
      context.fillStyle = "#7f8ea8";
      context.fillText("NO FEDERATED ACTIVITY YET", 256, cursor);
    }

    const label = fediverseFollowLabel(profile);
    if (label) {
      const active = profile.isFollowing === true;
      roundedRect(context, 96, 424, 320, 52, 26);
      context.fillStyle = active
        ? "rgba(158,247,198,0.18)"
        : "rgba(169,184,255,0.2)";
      context.fill();
      context.strokeStyle = active ? "#9ef7c6" : "#a9b8ff";
      context.lineWidth = 3;
      context.stroke();
      context.font = '700 23px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = active ? "#9ef7c6" : "#eaefff";
      context.fillText(label, 256, 451);
    }
  });
}

// Two small tabs under the badge switch the chest display between the
// visitor's world info and their fediverse card.
function chestTabTexture(THREE, label, active) {
  return canvasTexture(THREE, 256, 128, (context) => {
    context.clearRect(0, 0, 256, 128);
    roundedRect(context, 4, 4, 248, 120, 22);
    context.fillStyle = active ? "rgba(158,247,198,0.9)" : "rgba(8,20,17,0.92)";
    context.fill();
    context.strokeStyle = active ? "#eafff2" : "#9ef7c6";
    context.lineWidth = 6;
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '700 46px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = active ? "#08241a" : "#9ef7c6";
    context.fillText(String(label).slice(0, 8), 128, 66);
  });
}

function createAvatarChestTabs(THREE) {
  const group = new THREE.Group();
  group.name = "forkmesh-chest-tabs";
  [
    { tab: "info", label: "INFO", x: -0.19 },
    { tab: "fediverse", label: "FEDI", x: 0.19 },
  ].forEach((spec) => {
    const button = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.35, 0.19),
      resources.uniqueMaterial(THREE, {
        map: chestTabTexture(THREE, spec.label, spec.tab === "info"),
        transparent: true,
      }, "MeshBasicMaterial"),
    );
    button.name = `world-chest-tab-${spec.tab}`;
    // Avatar fronts face -Z, matching the badge just above these tabs. The
    // tabs sit flush under the badge so the wallet chip fits beneath them.
    button.position.set(spec.x, 1.79, -0.318);
    button.rotation.y = Math.PI;
    button.userData.chestTab = spec.tab;
    button.renderOrder = 3;
    group.add(button);
  });
  return group;
}

function syncChestTabs(THREE, avatar) {
  const tabs = avatar?.userData?.chestTabs;
  if (!tabs) return;
  const active = avatar.userData.chestTab === "fediverse" ? "fediverse" : "info";
  // Badge repaints are frequent; the two small tab textures only change when
  // the selected tab does.
  if (avatar.userData.chestTabRendered === active) return;
  avatar.userData.chestTabRendered = active;
  tabs.children.forEach((button) => {
    const label = button.userData.chestTab === "fediverse" ? "FEDI" : "INFO";
    const old = button.material.map;
    button.material.map = chestTabTexture(
      THREE,
      label,
      button.userData.chestTab === active,
    );
    button.material.needsUpdate = true;
    old?.dispose?.();
  });
}
