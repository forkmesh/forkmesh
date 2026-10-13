import { canvasTexture, roundedRect } from "./canvas.js";
import { makeMaterial, makeSharedMaterial } from "./materials.js";
import { clamp, setShadows } from "./object-utils.js";
import * as resources from "./resources.js";

// The board is a normal standing banner now, not a tower: five entries per
// page keep it dense while every line stays readable at banner scale.
export const WORLD_BULLETIN_VISIBLE_EVENTS = 5;
const WORLD_BULLETIN_WIDTH = 1536;
const WORLD_BULLETIN_HEIGHT = 1024;

export function worldBulletinTexture(THREE, events = [], offset = 0) {
  const allEntries = (Array.isArray(events) ? events : [])
    .filter((event) => event && String(event.title || "").trim())
    // Newest alerts always take priority at the top of the board.
    .sort((left, right) => Date.parse(right.startsAt || 0) - Date.parse(left.startsAt || 0));
  const start = clamp(
    Number(offset) || 0,
    0,
    Math.max(0, allEntries.length - WORLD_BULLETIN_VISIBLE_EVENTS),
  );
  const entries = allEntries.slice(start, start + WORLD_BULLETIN_VISIBLE_EVENTS);
  const wrapText = (context, text, x, y, maxWidth, lineHeight, maxLines = Infinity) => {
    const words = String(text || "").split(/\s+/).filter(Boolean);
    let line = "";
    let lines = 0;
    for (const word of words) {
      const candidate = line ? `${line} ${word}` : word;
      if (line && context.measureText(candidate).width > maxWidth) {
        context.fillText(line, x, y + lines * lineHeight);
        lines += 1;
        if (lines >= maxLines) return lines;
        line = word;
      } else {
        line = candidate;
      }
    }
    if (line && lines < maxLines) {
      context.fillText(line, x, y + lines * lineHeight);
      lines += 1;
    }
    return lines;
  };
  const formatTime = (value) => {
    const timestamp = Date.parse(value || "");
    return Number.isFinite(timestamp)
      ? new Date(timestamp).toLocaleString([], { dateStyle: "medium", timeStyle: "short" })
      : "Time to be announced";
  };
  // The right-hand column is left clear for the ▲ / ▼ scroll controls, so no
  // line is allowed to run under them.
  const textWidth = 1276;
  return canvasTexture(THREE, WORLD_BULLETIN_WIDTH, WORLD_BULLETIN_HEIGHT, (context) => {
    context.fillStyle = "#0b1820";
    context.fillRect(0, 0, WORLD_BULLETIN_WIDTH, WORLD_BULLETIN_HEIGHT);
    context.strokeStyle = "#7ed9ff";
    context.lineWidth = 10;
    context.strokeRect(8, 8, WORLD_BULLETIN_WIDTH - 16, WORLD_BULLETIN_HEIGHT - 16);
    context.fillStyle = "#e5f8ff";
    context.font = '800 54px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("WORLD BULLETIN", 42, 72);
    context.fillStyle = "#8eddf7";
    context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("PUBLIC ALERTS · LIVE COMMUNITY EVENTS · NEWEST FIRST", 44, 108);
    context.strokeStyle = "rgba(126,217,255,0.42)";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(44, 128);
    context.lineTo(1492, 128);
    context.stroke();
    if (!entries.length) {
      context.fillStyle = "#c3dbe3";
      context.font = '700 34px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO ACTIVE PUBLIC ALERTS", 44, 210);
      context.font = '600 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("THE COMMUNITY SCHEDULE WILL APPEAR HERE.", 44, 256);
      return;
    }
    entries.forEach((event, index) => {
      const y = 168 + index * 168;
      context.fillStyle = "#f7d58a";
      context.font = '800 28px "ForkMesh Mono", ui-monospace, monospace';
      wrapText(context, `${start + index + 1}. ${event.title}`, 44, y, textWidth, 32, 1);
      context.fillStyle = "#bad0d8";
      context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `${event.type || "Community"} · ${event.destination || "Town Square"}`,
        60,
        y + 30,
      );
      context.fillStyle = "#8eddf7";
      context.font = '600 19px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `START ${formatTime(event.startsAt)} · END ${formatTime(event.endsAt)}`,
        60,
        y + 55,
      );
      context.fillStyle = "#d5e6e9";
      // Event descriptions are sanitized to 500 characters upstream; three
      // wrapped lines carry the opening of each one without pushing the five
      // slots off the board.
      context.font = '600 17px "ForkMesh Mono", ui-monospace, monospace';
      wrapText(context, event.description || "Community event", 60, y + 80, textWidth - 16, 21, 3);
      if (index < entries.length - 1) {
        context.strokeStyle = "rgba(126,217,255,0.28)";
        context.lineWidth = 2;
        context.beginPath();
        context.moveTo(44, y + 140);
        context.lineTo(1492, y + 140);
        context.stroke();
      }
    });
    context.fillStyle = "#8eddf7";
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      `SHOWING ${start + 1}-${Math.min(start + WORLD_BULLETIN_VISIBLE_EVENTS, allEntries.length)} OF ${allEntries.length} · ▲ / ▼ OR SCROLL OVER THIS BOARD`,
      44,
      998,
    );
  });
}

export function worldGeneralChatTexture(THREE, messages = []) {
  const entries = (Array.isArray(messages) ? messages : [])
    .filter((message) => message && String(message.sender || "").trim())
    .sort((left, right) => Number(left.ts || 0) - Number(right.ts || 0))
    .slice(-8);
  return canvasTexture(THREE, 1536, 1024, (context) => {
    context.fillStyle = "#071712";
    context.fillRect(0, 0, 1536, 1024);
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 10;
    context.strokeRect(8, 8, 1520, 1008);
    context.fillStyle = "#effff6";
    context.font = '800 52px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("#GENERAL · RECENT CHAT", 42, 70);
    context.fillStyle = "#80bda0";
    context.font = '700 21px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      "LIVE HISTORY · CLICK TO OPEN FULL CHAT · IMAGES & REACTIONS",
      44,
      108,
    );
    context.strokeStyle = "rgba(158,247,198,0.35)";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(44, 128);
    context.lineTo(1492, 128);
    context.stroke();
    if (!entries.length) {
      context.fillStyle = "#9ab3a5";
      context.font = '700 31px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("CONNECTING TO ENCRYPTED #GENERAL HISTORY…", 44, 210);
      return;
    }
    entries.forEach((message, index) => {
      const y = 170 + index * 101;
      const timestamp = Number(message.ts) || 0;
      const time = timestamp
        ? new Date(timestamp).toLocaleTimeString([], {
            hour: "2-digit",
            minute: "2-digit",
          })
        : "now";
      context.fillStyle = "#9ef7c6";
      context.font = '800 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `${String(message.sender).slice(0, 26)}  ${time}`,
        44,
        y,
        800,
      );
      const reactionCount = Math.max(0, Number(message.reactionCount) || 0);
      const attachmentName = String(message.attachmentName || "").slice(0, 52);
      const meta = [
        attachmentName ? `▣ ${attachmentName}` : "",
        reactionCount ? `♡ ${reactionCount}` : "",
      ].filter(Boolean).join("  ");
      const preview = message.previewImage;
      const hasPreview =
        preview &&
        preview.complete === true &&
        Number(preview.naturalWidth) > 0;
      if (hasPreview) {
        try {
          context.drawImage(preview, 1398, y - 22, 72, 72);
        } catch (_) {}
      }
      if (meta) {
        context.fillStyle = "#f7d58a";
        context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
        context.textAlign = "right";
        context.fillText(meta, hasPreview ? 1382 : 1484, y, 580);
        context.textAlign = "left";
      }
      context.fillStyle = "#d9eee2";
      context.font = '600 22px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        String(message.text || (attachmentName ? "Shared an image or file" : ""))
          .replace(/\s+/g, " ")
          .slice(0, 104),
        64,
        y + 35,
        hasPreview ? 1290 : 1390,
      );
      context.strokeStyle = "rgba(158,247,198,0.18)";
      context.beginPath();
      context.moveTo(44, y + 61);
      context.lineTo(1492, y + 61);
      context.stroke();
    });
  });
}

export function worldDiscordBoardTexture(THREE) {
  return canvasTexture(THREE, 1536, 1024, (context) => {
    context.fillStyle = "#0d1117";
    context.fillRect(0, 0, 1536, 1024);
    context.strokeStyle = "#5865f2";
    context.lineWidth = 14;
    context.strokeRect(10, 10, 1516, 1004);
    context.fillStyle = "#f0f6fc";
    context.font = '900 70px "ForkMesh Mono", ui-monospace, monospace';
    context.textAlign = "center";
    context.fillText("DISCORD BRIDGE", 768, 150);
    context.fillStyle = "#a5b4fc";
    context.font = '800 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("PUBLIC ORGANIZATION CHANNELS", 768, 245);
    context.strokeStyle = "rgba(88,101,242,0.52)";
    context.lineWidth = 3;
    context.beginPath();
    context.moveTo(160, 300);
    context.lineTo(1376, 300);
    context.stroke();
    context.textAlign = "left";
    context.fillStyle = "#c9d1d9";
    context.font = '700 34px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("▸ OWNER-VERIFIED SERVER", 220, 430);
    context.fillText("▸ SELECTED PUBLIC CHANNELS ONLY", 220, 505);
    context.fillText("▸ MANUAL REFRESH · NO BACKGROUND RELAY", 220, 580);
    context.fillText("▸ MENTIONS DISABLED", 220, 655);
    context.textAlign = "center";
    context.fillStyle = "#7ee787";
    context.font = '900 48px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("CLICK TO OPEN CONNECTOR", 768, 810);
    context.fillStyle = "#8c959f";
    context.font = '650 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("CREDENTIALS STAY IN THE WORKER SECRET STORE", 768, 900);
  });
}

// Three posts at a time on a board twice as tall as the old one, so each card
// holds the full text, a deep image strip, and the post's engagement counts.
// The board is repainted from the same snapshot the mini-app renders.
export const MASTODON_KIOSK_VISIBLE_TOOTS = 3;
const MASTODON_KIOSK_VISIBLE_REPLIES = 5;
// Vertical pitch of one toot card, and the top of the first card. The extra
// height over the old 470 all goes to the attachment strip.
const MASTODON_KIOSK_TOOT_PITCH = 720;
const MASTODON_KIOSK_TOOT_TOP = 1390;
const MASTODON_KIOSK_WIDTH = 1536;
const MASTODON_KIOSK_HEIGHT = 4096;
export const MASTODON_KIOSK_REFRESH_MS = 10 * 60 * 1000;

// Shared by the board texture and the "open in a new tab" buttons so both
// agree on which two toots are on screen for a given scroll offset.
export function mastodonKioskVisibleToots(snapshot, offset) {
  const toots = Array.isArray(snapshot?.toots) ? snapshot.toots : [];
  const start = clamp(
    Number(offset) || 0,
    0,
    Math.max(0, toots.length - MASTODON_KIOSK_VISIBLE_TOOTS),
  );
  return toots.slice(start, start + MASTODON_KIOSK_VISIBLE_TOOTS);
}

export function wrapCanvasText(context, text, x, y, maxWidth, lineHeight, maxLines = Infinity) {
  const words = String(text || "").split(/\s+/).filter(Boolean);
  let line = "";
  let lines = 0;
  for (const word of words) {
    const candidate = line ? `${line} ${word}` : word;
    if (line && context.measureText(candidate).width > maxWidth) {
      context.fillText(line, x, y + lines * lineHeight);
      lines += 1;
      if (lines >= maxLines) return lines;
      line = word;
    } else {
      line = candidate;
    }
  }
  if (line && lines < maxLines) {
    context.fillText(line, x, y + lines * lineHeight);
    lines += 1;
  }
  return lines;
}

// One-line fit for the reply rows: replies get a single line each, so long
// bodies are cut at the card width with an ellipsis rather than wrapped away.
export function clipCanvasText(context, text, maxWidth) {
  const flat = String(text || "").replace(/\s+/g, " ").trim();
  if (!flat || context.measureText(flat).width <= maxWidth) return flat;
  let cut = flat;
  while (cut.length > 1 && context.measureText(`${cut}…`).width > maxWidth) {
    cut = cut.slice(0, -1);
  }
  return `${cut.trimEnd()}…`;
}

export function mastodonKioskTexture(THREE, snapshot = null, offset = 0, resolveImage = null) {
  const WIDTH = MASTODON_KIOSK_WIDTH;
  const HEIGHT = MASTODON_KIOSK_HEIGHT;
  return canvasTexture(THREE, WIDTH, HEIGHT, (context) => {
    context.fillStyle = "#191a2e";
    context.fillRect(0, 0, WIDTH, HEIGHT);
    if (!snapshot) {
      // No live profile yet (still fetching, or mastodon.social unreachable):
      // fall back to the static kiosk sign describing the board.
      context.fillStyle = "#6364ff";
      context.fillRect(12, 12, 1512, 260);
      context.fillStyle = "#f2f3ff";
      context.font = '800 150px "ForkMesh Favorit", sans-serif';
      context.fillText("MASTODON", 96, 200);
      context.fillStyle = "#c8c9ff";
      context.font = '700 72px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("@forkmesh", 96, 470);
      context.font = '600 56px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("@mastodon.social", 96, 562);
      context.strokeStyle = "rgba(99,100,255,0.5)";
      context.lineWidth = 4;
      context.beginPath();
      context.moveTo(96, 656);
      context.lineTo(1440, 656);
      context.stroke();
      context.fillStyle = "#e8e9ff";
      context.font = '700 60px "ForkMesh Mono", ui-monospace, monospace';
      [
        "LIVE PUBLIC PROFILE",
        "FOLLOWERS · FOLLOWING · POSTS",
        "BIO AND VERIFIED PROFILE LINKS",
        "FULL POSTS WITH IMAGES",
        "STARS · BOOSTS · REPLY COUNTS",
        "REPLIES WITH AUTHOR ICONS",
        "REFRESHED EVERY 10 MINUTES",
      ].forEach((line, index) => {
        context.fillText(line, 96, 810 + index * 136);
      });
      context.fillStyle = "#8b9bf4";
      context.font = '800 68px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("TAP / CLICK TO OPEN", 96, 3860);
      context.fillStyle = "#7a7ca8";
      context.font = '600 44px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("READ-ONLY · FETCHED FROM MASTODON.SOCIAL", 96, 4012);
      context.strokeStyle = "#6364ff";
      context.lineWidth = 16;
      context.strokeRect(12, 12, 1512, 4072);
      return;
    }
    const image = (url) =>
      typeof resolveImage === "function" ? resolveImage(url) : null;
    // Header banner, cover-cropped like the profile page. Text over the band
    // sits on a darkening gradient so it stays readable on any artwork.
    const header = image(snapshot.headerURL);
    context.save();
    context.beginPath();
    context.rect(16, 16, 1504, 560);
    context.clip();
    if (header?.naturalWidth > 0 && header?.naturalHeight > 0) {
      const scale = Math.max(
        1504 / header.naturalWidth,
        560 / header.naturalHeight,
      );
      const width = header.naturalWidth * scale;
      const height = header.naturalHeight * scale;
      context.drawImage(
        header,
        16 + (1504 - width) / 2,
        16 + (560 - height) / 2,
        width,
        height,
      );
    } else if (snapshot.unavailable) {
      context.fillStyle = "#ffbd6b";
      context.font = '800 62px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("PUBLIC FEED UNAVAILABLE", 96, 820);
      context.fillStyle = "#e8e9ff";
      context.font = '600 48px "ForkMesh Favorit", sans-serif';
      wrapCanvasText(
        context,
        snapshot.reason || "X did not return a public timeline.",
        96,
        920,
        1344,
        60,
        4,
      );
      context.fillStyle = "#8b9bf4";
      context.font = '800 48px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("HUMAN TODO", 96, 1260);
      context.fillStyle = "#c8c9ff";
      context.font = '600 43px "ForkMesh Favorit", sans-serif';
      wrapCanvasText(
        context,
        snapshot.humanTodo || "Publish a public @forkmesh post and try again.",
        96,
        1340,
        1344,
        56,
        4,
      );
      context.fillStyle = "#8b9bf4";
      context.font = '800 68px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("TAP / CLICK TO OPEN", 96, HEIGHT - 128);
      context.fillStyle = "#7a7ca8";
      context.font = '600 44px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("OPENS @FORKMESH ON MASTODON", 96, HEIGHT - 48);
    } else {
      context.fillStyle = "#43389c";
      context.fillRect(16, 16, 1504, 560);
    }
    const shade = context.createLinearGradient(0, 300, 0, 576);
    shade.addColorStop(0, "rgba(15,16,36,0)");
    shade.addColorStop(1, "rgba(15,16,36,0.9)");
    context.fillStyle = shade;
    context.fillRect(16, 16, 1504, 560);
    context.restore();
    // Avatar overlapping the banner edge, then identity beside it.
    const avatar = image(snapshot.avatarURL);
    context.fillStyle = "#191a2e";
    roundedRect(context, 40, 476, 208, 208, 44);
    context.fill();
    context.save();
    roundedRect(context, 52, 488, 184, 184, 36);
    context.clip();
    if (avatar?.naturalWidth > 0) {
      context.drawImage(avatar, 52, 488, 184, 184);
    } else {
      context.fillStyle = "#43389c";
      context.fillRect(52, 488, 184, 184);
      context.fillStyle = "#c8c9ff";
      context.font = '800 112px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("@", 100, 618);
    }
    context.restore();
    context.fillStyle = "#f2f3ff";
    context.font = '800 64px "ForkMesh Favorit", sans-serif';
    context.fillText(String(snapshot.displayName || "ForkMesh"), 280, 652);
    context.fillStyle = "#c8c9ff";
    context.font = '600 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(snapshot.acct || "@forkmesh@mastodon.social"), 280, 710);
    [
      ["FOLLOWERS", snapshot.followers],
      ["FOLLOWING", snapshot.following],
      ["POSTS", snapshot.posts],
      ["JOINED", snapshot.joined],
    ].forEach(([label, value], index) => {
      const x = 56 + index * 366;
      context.fillStyle = "#8b8db8";
      context.font = '700 28px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(label, x, 790);
      context.fillStyle = "#f2f3ff";
      context.font = '800 58px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(String(value ?? "—"), x, 858);
    });
    // Bio, then the profile's own link fields with their verification ticks —
    // the same block the mini-app prints under the stats.
    context.fillStyle = "#c8c9ff";
    context.font = '600 34px "ForkMesh Mono", ui-monospace, monospace';
    wrapCanvasText(context, snapshot.note, 56, 930, 1424, 46, 4);
    const fields = (Array.isArray(snapshot.fields) ? snapshot.fields : [])
      .filter(Boolean)
      .slice(0, 4);
    fields.forEach((field, index) => {
      const y = 1120 + index * 52;
      context.fillStyle = "#8b8db8";
      context.font = '700 28px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        clipCanvasText(context, String(field.name || "").toUpperCase(), 320),
        56,
        y,
      );
      context.fillStyle = field.verified ? "#9ef7c6" : "#e8e9ff";
      context.font = '600 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        clipCanvasText(
          context,
          `${field.value || ""}${field.verified ? " ✓" : ""}`,
          1024,
        ),
        420,
        y,
      );
    });
    context.strokeStyle = "rgba(99,100,255,0.5)";
    context.lineWidth = 4;
    context.beginPath();
    context.moveTo(56, 1290);
    context.lineTo(1480, 1290);
    context.stroke();
    context.fillStyle = "#8b9bf4";
    context.font = '800 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("LATEST TOOTS", 56, 1340);
    const toots = Array.isArray(snapshot.toots) ? snapshot.toots : [];
    const start = clamp(
      Number(offset) || 0,
      0,
      Math.max(0, toots.length - MASTODON_KIOSK_VISIBLE_TOOTS),
    );
    if (toots.length > MASTODON_KIOSK_VISIBLE_TOOTS) {
      context.textAlign = "right";
      context.fillStyle = "#7a7ca8";
      context.font = '700 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `${start + 1}–${Math.min(
          start + MASTODON_KIOSK_VISIBLE_TOOTS,
          toots.length,
        )} / ${toots.length} · SCROLL ▲▼`,
        1480,
        1340,
      );
      context.textAlign = "left";
    }
    const entries = mastodonKioskVisibleToots(snapshot, offset);
    if (!entries.length) {
      context.fillStyle = "#c8c9ff";
      context.font = '600 40px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO PUBLIC TOOTS YET", 56, 1450);
    }
    entries.forEach((toot, index) => {
      const top = MASTODON_KIOSK_TOOT_TOP + index * MASTODON_KIOSK_TOOT_PITCH;
      const images = (Array.isArray(toot.images) ? toot.images : [])
        .map((url) => String(url || ""))
        .filter(Boolean)
        .slice(0, 4);
      context.fillStyle = "#c8c9ff";
      context.font = '700 32px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        clipCanvasText(
          context,
          [toot.author, toot.acct, toot.date].filter(Boolean).join(" · "),
          1424,
        ),
        56,
        top + 36,
      );
      // Pinned / boosted / content-warning markers get their own highlighted
      // line above the body instead of being folded into the text.
      const marker = String(toot.marker || "").trim();
      if (marker) {
        context.fillStyle = "#ffd257";
        context.font = '700 30px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText(clipCanvasText(context, marker, 1424), 56, top + 82);
      }
      context.fillStyle = "#e8e9ff";
      context.font = '600 34px "ForkMesh Mono", ui-monospace, monospace';
      // The full post text runs until it hits the card's image strip; posts
      // longer than the card still end at a whole line rather than mid-word.
      wrapCanvasText(
        context,
        toot.text,
        56,
        marker ? top + 134 : top + 92,
        1424,
        46,
        images.length ? (marker ? 3 : 4) : marker ? 10 : 11,
      );
      if (images.length) {
        // Attachments below the text, cover-cropped into tiles that divide the
        // full card width. Tiles that have not loaded CORS-clean stay as empty
        // plates.
        const gap = 18;
        const height = 360;
        const width = (1424 - gap * (images.length - 1)) / images.length;
        const y = top + 296;
        images.forEach((url, position) => {
          const x = 56 + position * (width + gap);
          context.save();
          roundedRect(context, x, y, width, height, 18);
          context.clip();
          const media = image(url);
          if (media?.naturalWidth > 0 && media?.naturalHeight > 0) {
            const scale = Math.max(
              width / media.naturalWidth,
              height / media.naturalHeight,
            );
            context.drawImage(
              media,
              x + (width - media.naturalWidth * scale) / 2,
              y + (height - media.naturalHeight * scale) / 2,
              media.naturalWidth * scale,
              media.naturalHeight * scale,
            );
          } else {
            context.fillStyle = "#232445";
            context.fillRect(x, y, width, height);
            context.fillStyle = "#7a7ca8";
            context.font = '700 26px "ForkMesh Mono", ui-monospace, monospace';
            context.fillText("IMAGE", x + 24, y + height / 2 + 10);
          }
          context.restore();
        });
      }
      // Engagement counts sit on the card's bottom rule: stars, boosts, and
      // how many people replied to that post.
      context.fillStyle = "#8b9bf4";
      context.font = '700 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        [
          `⭐ ${toot.stars ?? 0} STARS`,
          `🔁 ${toot.boosts ?? 0} BOOSTS`,
          `💬 ${toot.replies ?? 0} REPLIES`,
        ].join("   ·   "),
        56,
        top + 692,
      );
      // Rule between the cards only: the last card runs straight into the
      // replies strip so the tall image tiles keep their clearance.
      if (index < entries.length - 1) {
        context.strokeStyle = "rgba(99,100,255,0.28)";
        context.lineWidth = 3;
        context.beginPath();
        context.moveTo(56, top + 706);
        context.lineTo(1480, top + 706);
        context.stroke();
      }
    });
    // Replies section: the newest public replies other accounts left on those
    // toots, each with the replier's own avatar so the board shows who is
    // talking back rather than just a reply count.
    const replies = (Array.isArray(snapshot.replies) ? snapshot.replies : [])
      .filter(Boolean)
      .slice(0, MASTODON_KIOSK_VISIBLE_REPLIES);
    context.fillStyle = "#8b9bf4";
    context.font = '800 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("REPLIES", 56, 3560);
    if (!replies.length) {
      context.fillStyle = "#7a7ca8";
      context.font = '600 32px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO PUBLIC REPLIES YET", 56, 3622);
    }
    replies.forEach((reply, index) => {
      const top = 3596 + index * 78;
      const icon = image(reply.avatar);
      context.save();
      roundedRect(context, 56, top, 60, 60, 16);
      context.clip();
      if (icon?.naturalWidth > 0) {
        context.drawImage(icon, 56, top, 60, 60);
      } else {
        context.fillStyle = "#43389c";
        context.fillRect(56, top, 60, 60);
        context.fillStyle = "#c8c9ff";
        context.font = '800 40px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText("@", 68, top + 46);
      }
      context.restore();
      context.fillStyle = "#c8c9ff";
      context.font = '700 26px "ForkMesh Mono", ui-monospace, monospace';
      const who = [reply.author, reply.acct, reply.date]
        .map((part) => String(part || "").trim())
        .filter(Boolean)
        .join(" · ");
      context.fillText(clipCanvasText(context, who, 1344), 138, top + 24);
      context.fillStyle = "#e8e9ff";
      context.font = '600 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(clipCanvasText(context, reply.text, 1344), 138, top + 58);
    });
    context.fillStyle = "#8b9bf4";
    context.font = '800 38px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("TAP / CLICK TO OPEN THE FULL PROFILE", 56, 4028);
    context.fillStyle = "#7a7ca8";
    context.font = '600 28px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      "LIVE · READ-ONLY · REFRESHED EVERY 10 MINUTES FROM MASTODON.SOCIAL",
      56,
      4068,
    );
    context.strokeStyle = "#6364ff";
    context.lineWidth = 16;
    context.strokeRect(12, 12, 1512, 4072);
  });
}

// MM:SS left before the next fetch, floored at 00:00.
function mastodonCountdownClock(remainingMs) {
  const seconds = Math.max(0, Math.ceil((Number(remainingMs) || 0) / 1000));
  const minutes = Math.floor(seconds / 60);
  return `${String(minutes).padStart(2, "0")}:${String(seconds % 60).padStart(
    2,
    "0",
  )}`;
}

// The refresh timer that rides on the kiosk frame: a small MM:SS readout of
// the time left in the ten-minute refresh window. It repaints once a second
// on its own small texture so the big board texture is only rebuilt when the
// snapshot itself changes.
export function drawMastodonCountdown(
  context,
  remainingMs = MASTODON_KIOSK_REFRESH_MS,
  totalMs = MASTODON_KIOSK_REFRESH_MS,
  loading = false,
) {
  const total = Math.max(1000, Number(totalMs) || MASTODON_KIOSK_REFRESH_MS);
  const remaining = clamp(Number(remainingMs) || 0, 0, total);
  // Unframed: just the label over a soft backing plate, no border, so it
  // reads as lettering on the stand rather than a badge on the board.
  context.fillStyle = "rgba(15,16,36,0.72)";
  roundedRect(context, 4, 4, 248, 120, 22);
  context.fill();
  context.textAlign = "center";
  context.fillStyle = "#8b8db8";
  context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
  context.fillText(loading ? "REFRESHING" : "NEXT SYNC", 128, 44);
  context.fillStyle = loading ? "#8b9bf4" : "#ffd257";
  context.font = '800 54px "ForkMesh Mono", ui-monospace, monospace';
  context.fillText(
    loading ? "--:--" : mastodonCountdownClock(remaining),
    128,
    100,
  );
}

function mastodonCountdownTexture(
  THREE,
  remainingMs = MASTODON_KIOSK_REFRESH_MS,
  totalMs = MASTODON_KIOSK_REFRESH_MS,
  loading = false,
) {
  return canvasTexture(THREE, 256, 128, (context) =>
    drawMastodonCountdown(context, remainingMs, totalMs, loading),
  );
}

// Posting-cadence thresholds for the last-post plate. Mastodon presence
// guides settle on roughly one post a day for an account that wants to stay
// visible: under a day since the newest toot is healthy green, a missed day
// turns amber ("time to post"), and three silent days reads as an abandoned
// profile and goes red.
const MASTODON_POST_FRESH_MS = 24 * 60 * 60 * 1000;
const MASTODON_POST_STALE_MS = 72 * 60 * 60 * 1000;

// The same cadence rule for the blog board, stretched to how often a feature
// blog actually publishes: a post within the week is green, a quiet week
// turns amber, and three silent weeks read as a stalled blog and go red.
const BLOG_POST_FRESH_MS = 7 * 24 * 60 * 60 * 1000;
const BLOG_POST_STALE_MS = 21 * 24 * 60 * 60 * 1000;

// Compact elapsed readout: minutes under an hour, hours under two days,
// whole days beyond that.
function mastodonLastPostClock(sinceMs) {
  const minutes = Math.max(0, Math.floor((Number(sinceMs) || 0) / 60_000));
  if (minutes < 60) return `${minutes}M AGO`;
  const hours = Math.floor(minutes / 60);
  if (hours < 48) return `${hours}H AGO`;
  return `${Math.floor(hours / 24)}D AGO`;
}

function mastodonLastPostColor(
  sinceMs,
  freshMs = MASTODON_POST_FRESH_MS,
  staleMs = MASTODON_POST_STALE_MS,
) {
  const since = Number(sinceMs) || 0;
  if (since < freshMs) return "#9ef7c6";
  if (since < staleMs) return "#ffb454";
  return "#ff7a7a";
}

// The plate beside the sync clock: how long since @forkmesh last posted,
// tinted green / amber / red by the cadence thresholds above so a glance at
// the stand says whether it is time to post again. sinceMs of null (nothing
// fetched yet, or a feed with no dated posts) renders a neutral placeholder.
// The social banners reuse the same plate with their own label and
// thresholds.
export function drawMastodonLastPost(
  context,
  sinceMs = null,
  label = "LAST POST",
  freshMs = MASTODON_POST_FRESH_MS,
  staleMs = MASTODON_POST_STALE_MS,
) {
  const known = Number.isFinite(Number(sinceMs)) && Number(sinceMs) >= 0;
  context.fillStyle = "rgba(15,16,36,0.72)";
  roundedRect(context, 4, 4, 248, 120, 22);
  context.fill();
  context.textAlign = "center";
  context.fillStyle = "#8b8db8";
  context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
  context.fillText(label, 128, 44);
  context.fillStyle = known
    ? mastodonLastPostColor(sinceMs, freshMs, staleMs)
    : "#8b9bf4";
  context.font = '800 44px "ForkMesh Mono", ui-monospace, monospace';
  context.fillText(known ? mastodonLastPostClock(sinceMs) : "--", 128, 96);
}

function mastodonLastPostTexture(
  THREE,
  sinceMs = null,
  label = "LAST POST",
  freshMs = MASTODON_POST_FRESH_MS,
  staleMs = MASTODON_POST_STALE_MS,
) {
  return canvasTexture(THREE, 256, 128, (context) =>
    drawMastodonLastPost(context, sinceMs, label, freshMs, staleMs),
  );
}

export function createMastodonKiosk(THREE, interactive) {
  const group = new THREE.Group();
  group.name = "forkmesh-mastodon-kiosk";
  // A newsstand beside the Office approach: close enough to read on the walk
  // to the door, far enough not to block the campus bridge.
  group.position.set(33.5, 0, -18.5);
  group.rotation.y = Math.atan2(-group.position.x, -group.position.z);
  const base = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.6, 0.4, 2.2),
    makeSharedMaterial(THREE, "#20213a", { metalness: 0.2, roughness: 0.7 }),
  );
  base.position.y = 0.2;
  const post = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.5, 2.2, 0.5),
    makeSharedMaterial(THREE, "#2c2d4d", { metalness: 0.4, roughness: 0.5 }),
  );
  post.position.y = 1.3;
  // A billboard twice as tall as the old one: the same 0.75-wide stand now
  // carries the live profile header, bio, stats, three full toot cards with
  // deep image strips, and the replies strip without crowding any of them.
  const frame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.9, 20.0, 0.36),
    makeSharedMaterial(THREE, "#43389c", { metalness: 0.35, roughness: 0.45 }),
  );
  frame.position.y = 11.75;
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 7.2, 19.2),
    resources.uniqueMaterial(THREE, {
      map: mastodonKioskTexture(THREE),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  face.name = "forkmesh-mastodon-kiosk-face";
  face.position.set(0, 11.75, 0.2);
  // Sits on the stand under the board, off the artwork entirely, so the board
  // itself is all profile and posts. Wide and short: it reads a MM:SS clock,
  // not a dial.
  const countdown = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 1.6, 0.8),
    resources.uniqueMaterial(THREE, {
      map: mastodonCountdownTexture(THREE),
      transparent: true,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  countdown.name = "forkmesh-mastodon-kiosk-countdown";
  countdown.position.set(-0.95, 1.15, 0.3);
  // Its sibling plate: how long since the last toot, colored by whether we
  // are keeping up a healthy posting cadence.
  const lastPost = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 1.6, 0.8),
    resources.uniqueMaterial(THREE, {
      map: mastodonLastPostTexture(THREE),
      transparent: true,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  lastPost.name = "forkmesh-mastodon-kiosk-lastpost";
  lastPost.position.set(0.95, 1.15, 0.3);
  const makeKioskControl = (label, direction, x, y) => {
    const control = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.7, 0.7),
      resources.uniqueMaterial(THREE, {
        map: canvasTexture(THREE, 256, 256, (context) => {
          context.fillStyle = "#20213a";
          context.fillRect(0, 0, 256, 256);
          context.strokeStyle = "#8b9bf4";
          context.lineWidth = 14;
          context.strokeRect(8, 8, 240, 240);
          context.fillStyle = "#e8e9ff";
          context.font = '800 160px "ForkMesh Mono", ui-monospace, monospace';
          context.textAlign = "center";
          context.textBaseline = "middle";
          context.fillText(label, 128, 136);
        }),
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    control.position.set(x, y, 0.3);
    control.userData.interactive = `mastodon-kiosk-scroll-${direction}`;
    return control;
  };
  // Both scroll controls now live at the bottom of the board, up to the left
  // of down, out of the way of the toot cards above them.
  const scrollUp = makeKioskControl("▲", "up", 2.2, 2.55);
  const scrollDown = makeKioskControl("▼", "down", 3.1, 2.55);
  const makeOpenButton = (name, x, y) => {
    const button = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 0.55, 0.55),
      resources.uniqueMaterial(THREE, {
        map: canvasTexture(THREE, 256, 256, (context) => {
          context.fillStyle = "#20213a";
          context.fillRect(0, 0, 256, 256);
          context.strokeStyle = "#8b9bf4";
          context.lineWidth = 14;
          context.strokeRect(8, 8, 240, 240);
          context.fillStyle = "#e8e9ff";
          context.font = '800 150px "ForkMesh Mono", ui-monospace, monospace';
          context.textAlign = "center";
          context.textBaseline = "middle";
          context.fillText("↗", 128, 136);
        }),
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    button.name = `forkmesh-mastodon-kiosk-open-${name}`;
    button.position.set(x, y, 0.3);
    button.userData.interactive = `mastodon-kiosk-open-${name}`;
    button.userData.href = "";
    button.visible = false;
    return button;
  };
  // One button beside the profile identity, one beside each of the three
  // visible toot cards, each opening that item's mastodon.social page in a
  // new tab instead of the in-app board. The heights track the card headings
  // on the taller board.
  const openProfile = makeOpenButton("profile", 3.25, 18.3);
  const openToot0 = makeOpenButton("toot-0", 3.25, 14.65);
  const openToot1 = makeOpenButton("toot-1", 3.25, 11.3);
  const openToot2 = makeOpenButton("toot-2", 3.25, 7.9);
  group.add(
    base,
    post,
    frame,
    face,
    countdown,
    lastPost,
    scrollUp,
    scrollDown,
    openProfile,
    openToot0,
    openToot1,
    openToot2,
  );
  group.traverse((child) => {
    if (!child.isMesh) return;
    if (!child.userData.interactive) {
      child.userData.interactive = "mastodon-board";
    }
    interactive.push(child);
  });
  setShadows(group);
  return group;
}

// Half-height siblings of the Mastodon kiosk for Twitter/X and Reddit: same
// stand, frame, and sign typography, but the whole board is one click target
// that opens the profile in a new tab. Neither network allows the browser to
// fetch its feed directly (no CORS, unlike mastodon.social), so the Worker
// proxies one edge-cached read via /api/world/social-posts and the board
// repaints from that snapshot; without one it keeps the static sign.
const SOCIAL_BANNER_VISIBLE_POSTS = 4;
const SOCIAL_BANNER_WIDTH = 1536;
const SOCIAL_BANNER_HEIGHT = 2048;
const SOCIAL_BANNER_POST_TOP = 760;
const SOCIAL_BANNER_POST_PITCH = 290;
// A board whose feed carries artwork and preview text (the blog's RSS items)
// draws taller cards: a 16:9 thumbnail beside the headline and the item's
// description under it, so fewer of them fit on the same face.
const SOCIAL_BANNER_ART_VISIBLE_POSTS = 3;
const SOCIAL_BANNER_ART_POST_PITCH = 372;
const SOCIAL_BANNER_ART_WIDTH = 400;
const SOCIAL_BANNER_ART_HEIGHT = 225;

// The blog board continues the same ring past Reddit. Its posts come from
// the blog's own RSS feed (/blog/rss.xml), so each card shows the item's
// artwork and preview text. Feed items carry pubDate, so the plate at the
// bottom of the board reads how long ago the newest article went up, tinted
// by the same green / amber / red rule as the Mastodon kiosk's LAST POST
// plate — only at a blog's slower cadence (a week fresh, three weeks stale).
export const BLOG_BANNER_OPTIONS = Object.freeze({
  id: "blog",
  position: [19.8, 0, -32.8],
  accent: "#3fb950",
  frameColor: "#1d5c33",
  titleColor: "#f0fff5",
  divider: "rgba(63,185,80,0.5)",
  title: "FORKMESH BLOG",
  handle: "forkmesh.com/blog",
  host: "every feature, explained",
  feedHeading: "FROM THE BLOG",
  postArt: true,
  tall: true,
  visiblePosts: 4,
  lines: [
    "FEATURE DEEP DIVES",
    "MIRRORS · AGENTS · CI · CHAT",
    "RSS: FORKMESH.COM/BLOG/RSS.XML",
  ],
  footer: "RSS FEED AT /BLOG/RSS.XML",
  url: "https://forkmesh.com/blog",
  staleness: {
    label: "LAST POST",
    freshMs: BLOG_POST_FRESH_MS,
    staleMs: BLOG_POST_STALE_MS,
  },
});

// One artwork-carrying card on a social board: the post's own image on the
// left (cover-cropped into a 16:9 tile), then its meta line, headline, and
// the preview text the feed item's description carries. An image that has
// not decoded yet — or one a host refuses to serve CORS-clean, which would
// taint the canvas and break the WebGL upload — keeps the placeholder plate.
function drawSocialBannerPostCard(context, post, top, resolveImage) {
  const width = SOCIAL_BANNER_ART_WIDTH;
  const height = SOCIAL_BANNER_ART_HEIGHT;
  const textX = 96 + width + 40;
  const textWidth = 1440 - textX;
  context.save();
  roundedRect(context, 96, top, width, height, 18);
  context.clip();
  const media =
    typeof resolveImage === "function" ? resolveImage(post.image) : null;
  if (media?.naturalWidth > 0 && media?.naturalHeight > 0) {
    const scale = Math.max(
      width / media.naturalWidth,
      height / media.naturalHeight,
    );
    context.drawImage(
      media,
      96 + (width - media.naturalWidth * scale) / 2,
      top + (height - media.naturalHeight * scale) / 2,
      media.naturalWidth * scale,
      media.naturalHeight * scale,
    );
  } else {
    context.fillStyle = "#232445";
    context.fillRect(96, top, width, height);
    context.fillStyle = "#7a7ca8";
    context.font = '700 28px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("IMAGE", 120, top + height / 2 + 10);
  }
  context.restore();
  context.fillStyle = "#8b9bf4";
  context.font = '600 31px "ForkMesh Mono", ui-monospace, monospace';
  context.fillText(
    clipCanvasText(
      context,
      [post.age, post.meta].filter(Boolean).join(" · "),
      textWidth,
    ),
    textX,
    top + 32,
  );
  context.fillStyle = "#65d892";
  context.font = '700 23px "ForkMesh Mono", ui-monospace, monospace';
  context.fillText(
    clipCanvasText(
      context,
      post.distribution || "SOCIAL STATUS UNKNOWN",
      textWidth,
    ),
    textX,
    top + 72,
  );
  context.fillStyle = "#f7c96b";
  context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
  context.fillText(
    clipCanvasText(
      context,
      post.reach || "VIEWS 0 · ~0 UNIQUE · 0 REFERRER SITES · 0 REFERRED",
      textWidth,
    ),
    textX,
    top + 112,
  );
  context.fillStyle = "#e8e9ff";
  context.font = '500 46px "ForkMesh Favorit", sans-serif';
  const headlines = wrapCanvasText(
    context, post.text, textX, top + 164, textWidth, 52, 2);
  context.fillStyle = "#a9abd4";
  context.font = '400 32px "ForkMesh Favorit", sans-serif';
  wrapCanvasText(
    context,
    post.detail,
    textX,
    top + 178 + headlines * 52,
    textWidth,
    38,
    2,
  );
}

export function socialBannerTexture(
  THREE,
  options,
  snapshot = null,
  resolveImage = null,
) {
  const WIDTH = SOCIAL_BANNER_WIDTH;
  const HEIGHT = options.tall ? 2560 : SOCIAL_BANNER_HEIGHT;
  return canvasTexture(THREE, WIDTH, HEIGHT, (context) => {
    context.fillStyle = "#191a2e";
    context.fillRect(0, 0, WIDTH, HEIGHT);
    context.fillStyle = options.accent;
    context.fillRect(12, 12, 1512, 260);
    context.fillStyle = options.titleColor;
    context.font = '800 150px "ForkMesh Favorit", sans-serif';
    context.fillText(options.title, 96, 200);
    context.fillStyle = "#c8c9ff";
    context.font = '700 72px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(options.handle, 96, 470);
    context.font = '600 56px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(options.host, 96, 562);
    context.strokeStyle = options.divider;
    context.lineWidth = 4;
    context.beginPath();
    context.moveTo(96, 656);
    context.lineTo(1440, 656);
    context.stroke();
    if (!snapshot) {
      // No proxy snapshot (still fetching, or the feed is unreachable):
      // fall back to the static sign describing the board.
      context.fillStyle = "#e8e9ff";
      context.font = '700 60px "ForkMesh Mono", ui-monospace, monospace';
      options.lines.forEach((line, index) => {
        context.fillText(line, 96, 810 + index * 136);
      });
      context.fillStyle = "#8b9bf4";
      context.font = '800 68px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("TAP / CLICK TO OPEN", 96, 1760);
      context.fillStyle = "#7a7ca8";
      context.font = '600 44px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(options.footer, 96, 1900);
    } else {
      const art = Boolean(options.postArt);
      const pitch = art
        ? SOCIAL_BANNER_ART_POST_PITCH
        : SOCIAL_BANNER_POST_PITCH;
      const posts = (Array.isArray(snapshot.posts) ? snapshot.posts : [])
        .slice(
          0,
          Number(options.visiblePosts) ||
            (art
              ? SOCIAL_BANNER_ART_VISIBLE_POSTS
              : SOCIAL_BANNER_VISIBLE_POSTS),
        );
      context.fillStyle = "#8b9bf4";
      context.font = '700 48px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(options.feedHeading || "LATEST POSTS", 96, 726);
      if (!posts.length) {
        context.fillStyle = "#e8e9ff";
        context.font = '700 60px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText("NO POSTS YET", 96, 900);
        context.fillStyle = "#7a7ca8";
        context.font = '600 48px "ForkMesh Mono", ui-monospace, monospace';
        context.fillText("BE THE FIRST OVER THERE", 96, 1000);
      }
      posts.forEach((post, index) => {
        const top = SOCIAL_BANNER_POST_TOP + index * pitch;
        if (art) {
          drawSocialBannerPostCard(context, post, top, resolveImage);
        } else {
          context.fillStyle = "#8b9bf4";
          context.font = '600 40px "ForkMesh Mono", ui-monospace, monospace';
          context.fillText(clipCanvasText(context, post.meta, 1344), 96, top);
          context.fillStyle = "#e8e9ff";
          context.font = '500 52px "ForkMesh Favorit", sans-serif';
          wrapCanvasText(context, post.text, 96, top + 76, 1344, 66, 3);
        }
        if (index < posts.length - 1) {
          // Art cards run taller than the text-only ones (three lines of
          // preview text under a two-line headline), so their rule sits
          // closer to the next card rather than through the last line.
          const rule = top + pitch - (art ? 20 : 66);
          context.strokeStyle = "rgba(139,155,244,0.25)";
          context.lineWidth = 2;
          context.beginPath();
          context.moveTo(96, rule);
          context.lineTo(1440, rule);
          context.stroke();
        }
      });
      context.fillStyle = "#8b9bf4";
      context.font = '800 68px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("TAP / CLICK TO OPEN", 96, HEIGHT - 128);
      context.fillStyle = "#7a7ca8";
      context.font = '600 44px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        "LIVE · REFRESHED EVERY 10 MINUTES · " + options.footer,
        96,
        HEIGHT - 48,
      );
    }
    context.strokeStyle = options.accent;
    context.lineWidth = 16;
    context.strokeRect(12, 12, 1512, HEIGHT - 24);
  });
}

export function createSocialBanner(THREE, interactive, options) {
  const group = new THREE.Group();
  group.name = `forkmesh-${options.id}-banner`;
  group.position.set(...options.position);
  group.rotation.y = Math.atan2(-group.position.x, -group.position.z);
  const base = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.6, 0.4, 2.2),
    makeSharedMaterial(THREE, "#20213a", { metalness: 0.2, roughness: 0.7 }),
  );
  base.position.y = 0.2;
  const post = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.5, 2.2, 0.5),
    makeSharedMaterial(THREE, "#2c2d4d", { metalness: 0.4, roughness: 0.5 }),
  );
  post.position.y = 1.3;
  const faceHeight = options.tall ? 12 : 9.6;
  const frameHeight = faceHeight + 0.4;
  const frameY = 1.75 + frameHeight / 2;
  const frame = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 7.9, frameHeight, 0.36),
    makeMaterial(THREE, options.frameColor, { metalness: 0.35, roughness: 0.45 }),
  );
  frame.position.y = frameY;
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 7.2, faceHeight),
    resources.uniqueMaterial(THREE, {
      map: socialBannerTexture(THREE, options),
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  face.name = `forkmesh-${options.id}-banner-face`;
  face.position.set(0, frameY, 0.2);
  // The same stand plates the Mastodon kiosk carries: the MM:SS countdown to
  // the next feed sync on the left, the staleness readout on the right.
  const countdown = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 1.6, 0.8),
    resources.uniqueMaterial(THREE, {
      map: mastodonCountdownTexture(THREE),
      transparent: true,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  countdown.name = `forkmesh-${options.id}-banner-countdown`;
  countdown.position.set(-0.95, 1.15, 0.3);
  const staleness = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 1.6, 0.8),
    resources.uniqueMaterial(THREE, {
      map: mastodonLastPostTexture(THREE, null, options.staleness.label),
      transparent: true,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  staleness.name = `forkmesh-${options.id}-banner-lastpost`;
  staleness.position.set(0.95, 1.15, 0.3);
  group.add(base, post, frame, face, countdown, staleness);
  group.traverse((child) => {
    if (!child.isMesh) return;
    child.userData.interactive = "social-banner-open";
    child.userData.href = options.url;
    interactive.push(child);
  });
  setShadows(group);
  return group;
}
