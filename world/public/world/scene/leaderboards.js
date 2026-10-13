import { sharedGeometry } from "../world-resource-cache.js";
import { canvasTexture, roundedRect } from "./canvas.js";
import { makeSharedMaterial } from "./materials.js";
import * as resources from "./resources.js";

function activeDurationLabel(value) {
  const milliseconds = Number(value);
  if (!Number.isFinite(milliseconds) || milliseconds < 0) {
    return "ACTIVE TIME NOT REPORTED";
  }
  let seconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(seconds / 86400);
  seconds %= 86400;
  const hours = Math.floor(seconds / 3600);
  seconds %= 3600;
  const minutes = Math.floor(seconds / 60);
  seconds %= 60;
  return `${days}d ${hours}h ${minutes}m ${seconds}s`;
}

export function rankedActiveLeaderboardMembers(members = []) {
  return (Array.isArray(members) ? members : [])
    .filter(
      (member) =>
        String(member?.name || "").trim() &&
        (member?.activeNow === true ||
          Number(member?.totalActiveMs ?? member?.activeMs ?? 0) > 0),
    )
    .sort((left, right) => {
      const durationDifference =
        Number(right?.totalActiveMs ?? right?.activeMs ?? 0) -
        Number(left?.totalActiveMs ?? left?.activeMs ?? 0);
      if (durationDifference) return durationDifference;
      return String(left?.name || "").localeCompare(
        String(right?.name || ""),
        undefined,
        { sensitivity: "base" },
      );
    })
    .slice(0, 6);
}

export function activeLeaderboardTexture(THREE, members = []) {
  const rows = rankedActiveLeaderboardMembers(members);
  return canvasTexture(THREE, 768, 512, (context) => {
    context.clearRect(0, 0, 768, 512);
    roundedRect(context, 4, 4, 760, 504, 16);
    context.fillStyle = "rgba(6,17,14,0.95)";
    context.fill();
    context.strokeStyle = "#77d9ff";
    context.lineWidth = 5;
    context.stroke();
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 48px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText("ACTIVE LEADERBOARD", 38, 58);
    context.fillStyle = "#77d9ff";
    context.font = '400 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("REGISTERED USER · TOTAL ACTIVE TIME", 38, 102);
    rows.forEach((member, index) => {
      const top = 148 + index * 56;
      const name = String(member.name).trim().slice(0, 24);
      const duration = activeDurationLabel(
        member.totalActiveMs ?? member.activeMs,
      );
      context.fillStyle = "#d9ffea";
      context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(`${index + 1}. ${name}`, 38, top);
      context.fillStyle = "#9ef7c6";
      context.font = '400 19px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `${member.activeNow === true ? "ACTIVE NOW · " : ""}${duration}`.slice(
          0,
          34,
        ),
        38,
        top + 25,
      );
    });
    if (!rows.length) {
      context.fillStyle = "#91a39a";
      context.font = '400 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO REGISTERED ACTIVITY REPORTED", 38, 176);
    }
  });
}

export function rankedReferralRows(rows = []) {
  return (Array.isArray(rows) ? rows : [])
    .filter(
      (row) =>
        String(row?.name || "").trim() &&
        (Number(row?.clicks) > 0 || Number(row?.signups) > 0),
    )
    .sort((left, right) => {
      const signupDifference =
        (Number(right?.signups) || 0) - (Number(left?.signups) || 0);
      if (signupDifference) return signupDifference;
      const clickDifference =
        (Number(right?.clicks) || 0) - (Number(left?.clicks) || 0);
      if (clickDifference) return clickDifference;
      return String(left?.name || "").localeCompare(
        String(right?.name || ""),
        undefined,
        { sensitivity: "base" },
      );
    })
    .slice(0, 5);
}

export function referralLeaderboardTexture(THREE, rows = [], viewerLink = "") {
  const ranked = rankedReferralRows(rows);
  return canvasTexture(THREE, 768, 512, (context) => {
    context.clearRect(0, 0, 768, 512);
    roundedRect(context, 4, 4, 760, 504, 16);
    context.fillStyle = "rgba(6,17,14,0.95)";
    context.fill();
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 5;
    context.stroke();
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 48px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText("REFERRAL LEADERBOARD", 38, 58);
    context.fillStyle = "#9ef7c6";
    context.font = '400 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("SHARE YOUR /r/ LINK · CLICKS AND SIGNUPS", 38, 102);
    ranked.forEach((row, index) => {
      const top = 148 + index * 52;
      const name = String(row.name).trim().slice(0, 22);
      context.fillStyle = "#d9ffea";
      context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(`${index + 1}. ${name}`, 38, top);
      context.fillStyle = "#77d9ff";
      context.font = '400 20px "ForkMesh Mono", ui-monospace, monospace';
      const clicks = Number(row.clicks) || 0;
      const signups = Number(row.signups) || 0;
      const value = `${signups} SIGNUPS · ${clicks} CLICKS`;
      context.fillText(value, 768 - 38 - context.measureText(value).width, top);
    });
    if (!ranked.length) {
      context.fillStyle = "#91a39a";
      context.font = '400 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO REFERRALS COUNTED YET", 38, 200);
    }
    context.strokeStyle = "rgba(158,247,198,0.4)";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(38, 418);
    context.lineTo(730, 418);
    context.stroke();
    if (viewerLink) {
      context.fillStyle = "#9ef7c6";
      context.font = '400 19px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("YOUR LINK · TAP THE BOARD TO COPY", 38, 446);
      context.fillStyle = "#d9ffea";
      context.font = '700 23px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(String(viewerLink).slice(0, 52), 38, 480);
    } else {
      context.fillStyle = "#91a39a";
      context.font = '400 21px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("SIGN IN TO GET YOUR OWN REFERRAL LINK", 38, 462);
    }
  });
}

export function makeReferralLeaderboardSign(THREE) {
  const sign = new THREE.Group();
  sign.name = "world-referral-leaderboard";
  const base = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 4.8, 0.25, 1.35),
    makeSharedMaterial(THREE, "#1a3032", { roughness: 0.8 }),
  );
  base.position.y = 0.13;
  sign.add(base);
  for (const x of [-1.72, 1.72]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.16, 3.7, 0.16),
      makeSharedMaterial(THREE, "#234d4a", { metalness: 0.22, roughness: 0.52 }),
    );
    post.position.set(x, 1.85, 0);
    sign.add(post);
  }
  const board = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 3.72, 2.72, 0.12),
    makeSharedMaterial(THREE, "#071712", { roughness: 0.55 }),
  );
  board.position.set(0, 2.08, 0.08);
  sign.add(board);
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 3.52, 2.55),
    resources.uniqueMaterial(THREE, {
      map: referralLeaderboardTexture(THREE),
      transparent: true,
    }, "MeshBasicMaterial"),
  );
  face.position.set(0, 2.08, 0.151);
  sign.add(face);
  sign.userData.face = face;
  return sign;
}

export function rankedSiteReferrerRows(rows = []) {
  return (Array.isArray(rows) ? rows : [])
    .filter(
      (row) =>
        String(row?.host || "").trim() && Number(row?.visits) > 0,
    )
    .sort((left, right) => {
      const visitDifference =
        (Number(right?.visits) || 0) - (Number(left?.visits) || 0);
      if (visitDifference) return visitDifference;
      return String(left?.host || "").localeCompare(
        String(right?.host || ""),
        undefined,
        { sensitivity: "base" },
      );
    })
    .slice(0, 5);
}

export function safeSiteReferrerURL(row) {
  const host = String(row?.host || "").trim().toLowerCase();
  if (
    !/^(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$/.test(
      host,
    )
  ) {
    return "";
  }
  try {
    const candidate = new URL(String(row?.url || ""));
    const candidateHost = candidate.hostname
      .toLowerCase()
      .replace(/^www\./, "");
    if (
      !["http:", "https:"].includes(candidate.protocol) ||
      candidateHost !== host ||
      candidate.username ||
      candidate.password ||
      candidate.hash
    ) {
      return "";
    }
    return candidate.href.slice(0, 1024);
  } catch (_) {
    return "";
  }
}

export function siteReferrerLeaderboardTexture(THREE, rows = [], totals = {}) {
  const ranked = rankedSiteReferrerRows(rows);
  const siteCount = Math.max(0, Number(totals?.sites) || 0);
  const visitCount = Math.max(0, Number(totals?.visits) || 0);
  return canvasTexture(THREE, 768, 720, (context) => {
    context.clearRect(0, 0, 768, 720);
    roundedRect(context, 4, 4, 760, 712, 16);
    context.fillStyle = "rgba(6,17,14,0.95)";
    context.fill();
    context.strokeStyle = "#77d9ff";
    context.lineWidth = 5;
    context.stroke();
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 43px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText("HTTP REFERER LEADERBOARD", 38, 58);
    context.fillStyle = "#77d9ff";
    context.font = '400 19px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("GROUPED BY HOSTNAME · LATEST SAFE FULL URL", 38, 102);
    ranked.forEach((row, index) => {
      const top = 142 + index * 96;
      const host = String(row.host).trim().slice(0, 30);
      context.fillStyle = "#d9ffea";
      context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(`${index + 1}. ${host}`, 38, top, 510);
      context.fillStyle = "#9ef7c6";
      context.font = '400 20px "ForkMesh Mono", ui-monospace, monospace';
      const value = `${Number(row.visits) || 0} VISITS`;
      context.fillText(value, 768 - 38 - context.measureText(value).width, top);
      const href = safeSiteReferrerURL(row);
      context.fillStyle = href ? "#77d9ff" : "#91a39a";
      context.font = '500 17px "ForkMesh Mono", ui-monospace, monospace';
      const linkLabel = href || "FULL URL CAPTURES ON THE NEXT REFERRAL";
      context.fillText(linkLabel, 70, top + 35, 630);
      if (href) {
        const width = Math.min(630, context.measureText(linkLabel).width);
        context.strokeStyle = "rgba(119,217,255,0.72)";
        context.lineWidth = 1;
        context.beginPath();
        context.moveTo(70, top + 48);
        context.lineTo(70 + width, top + 48);
        context.stroke();
      }
    });
    if (!ranked.length) {
      context.fillStyle = "#91a39a";
      context.font = '400 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("NO EXTERNAL REFERRERS COUNTED YET", 38, 200);
    }
    context.strokeStyle = "rgba(119,217,255,0.4)";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(38, 624);
    context.lineTo(730, 624);
    context.stroke();
    context.fillStyle = "#77d9ff";
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(`${siteCount} SITES · ${visitCount} VISITS`, 38, 658);
    context.fillStyle = "#91a39a";
    context.font = '400 17px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      "FULL PAGE URL · SENSITIVE QUERY VALUES REDACTED",
      38,
      692,
    );
  });
}

export function makeSiteReferrerLeaderboardSign(THREE) {
  const sign = new THREE.Group();
  sign.name = "world-site-referrer-leaderboard";
  const base = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 4.8, 0.25, 1.35),
    makeSharedMaterial(THREE, "#1a3032", { roughness: 0.8 }),
  );
  base.position.y = 0.13;
  sign.add(base);
  for (const x of [-1.72, 1.72]) {
    const post = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 0.16, 4.45, 0.16),
      makeSharedMaterial(THREE, "#234d4a", { metalness: 0.22, roughness: 0.52 }),
    );
    post.position.set(x, 2.225, 0);
    sign.add(post);
  }
  const board = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 3.72, 3.42, 0.12),
    makeSharedMaterial(THREE, "#071712", { roughness: 0.55 }),
  );
  board.position.set(0, 2.38, 0.08);
  sign.add(board);
  const face = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 3.52, 3.3),
    resources.uniqueMaterial(THREE, {
      map: siteReferrerLeaderboardTexture(THREE),
      transparent: true,
    }, "MeshBasicMaterial"),
  );
  face.position.set(0, 2.38, 0.151);
  sign.add(face);
  sign.userData.face = face;
  return sign;
}

export const WORLD_LEADERBOARD_BOARD_STUBS = Object.freeze([
  {
    id: "uptime",
    title: "MAINNODE UPTIME",
    subtitle: "MOST MINUTES ONLINE",
    valueKind: "minutes",
  },
  {
    id: "node-storage",
    title: "NODE STORAGE",
    subtitle: "PUBLIC MIRROR BYTES",
    valueKind: "bytes",
  },
  {
    id: "repos",
    title: "TOP OWNERS",
    subtitle: "MOST PUBLIC REPOSITORIES",
    valueKind: "repos",
  },
  {
    id: "mirrors",
    title: "MOST MIRRORED",
    subtitle: "REPOSITORIES BY OWNERS",
    valueKind: "mirrors",
  },
  {
    id: "hosted",
    title: "LONGEST HOSTED",
    subtitle: "REPOSITORIES ONLINE",
    valueKind: "age",
  },
  {
    id: "largest",
    title: "LARGEST REPOSITORIES",
    subtitle: "MIRROR DATA BY REPOSITORY",
    valueKind: "bytes",
  },
  {
    id: "data-hosted",
    title: "MOST DATA HOSTED",
    subtitle: "PUBLIC DATA BY OWNER",
    valueKind: "bytes",
  },
  {
    id: "contributors",
    title: "CONTRIBUTORS",
    subtitle: "ISSUES + PRS + COMMITS",
    valueKind: "contributions",
  },
  {
    id: "issue-reporters",
    title: "ISSUE REPORTERS",
    subtitle: "MOST TRACKER REPORTS",
    valueKind: "issues",
  },
  {
    id: "wallets",
    title: "MEMBER SOL WALLETS",
    subtitle: "PUBLISHED ADDRESS BALANCE",
    valueKind: "sol",
  },
  {
    id: "tip-recipients",
    title: "TOP TIP RECIPIENTS",
    subtitle: "MEMBER TIPS IN THE LEDGER",
    valueKind: "sol",
  },
  {
    id: "funds-mainnodes",
    title: "LEGACY · MAINNODES",
    subtitle: "HISTORICAL REPORTING",
    valueKind: "sol",
  },
  {
    id: "funds-contributors",
    title: "LEGACY · CONTRIBUTORS",
    subtitle: "HISTORICAL REPORTING",
    valueKind: "sol",
  },
  {
    id: "funds-projects",
    title: "LEGACY · PROJECTS",
    subtitle: "HISTORICAL REPORTING",
    valueKind: "sol",
  },
]);

// /api/leaderboards also ships "activity", "referrals", and "referring-sites".
// Those three cards are painted by their own richer textures — live world
// activity, the viewer's own referral link, redacted referrer URLs — so the
// shared statistic painter must leave them alone or the two repaints fight
// over the same face on every snapshot.
export const LEADERBOARD_CARDS_WITH_OWN_PAINTER = new Set([
  "active-members",
  "referrals",
  "http-referrers",
]);

function leaderboardStatCompactNumber(value) {
  return Math.max(0, Number(value) || 0).toLocaleString("en-US");
}

function leaderboardStatValue(board = {}, row = {}) {
  const kind = String(board.valueKind || "");
  if (kind === "minutes") {
    const minutes = Math.max(0, Math.round(Number(row.minutes) || 0));
    return minutes >= 60
      ? `${Math.floor(minutes / 60)}H ${minutes % 60}M`
      : `${minutes}M`;
  }
  if (kind === "bytes") {
    let bytes = Math.max(0, Number(row.bytes ?? row.sizeBytes) || 0);
    const units = ["B", "KB", "MB", "GB", "TB", "PB"];
    let unit = 0;
    while (bytes >= 1024 && unit < units.length - 1) {
      bytes /= 1024;
      unit += 1;
    }
    return `${unit ? bytes.toFixed(bytes >= 100 ? 0 : 1) : Math.round(bytes)} ${units[unit]}`;
  }
  if (kind === "repos") {
    return `${leaderboardStatCompactNumber(row.repos)} REPOS`;
  }
  if (kind === "issues") {
    return `${leaderboardStatCompactNumber(row.issues)} ISSUES`;
  }
  if (kind === "mirrors") {
    return `${leaderboardStatCompactNumber(row.mirrors)} OWNERS`;
  }
  if (kind === "age") {
    const milliseconds = Math.max(0, Number(row.ageMs) || 0);
    const days = Math.floor(milliseconds / 86400000);
    return days
      ? `${leaderboardStatCompactNumber(days)}D`
      : `${Math.floor(milliseconds / 3600000)}H`;
  }
  if (kind === "contributions") {
    return `${leaderboardStatCompactNumber(row.total)} TOTAL`;
  }
  if (kind === "sol") {
    const sol = Number(row.sol) || (Number(row.lamports) || 0) / 1e9;
    return `${sol.toFixed(sol >= 1 ? 2 : 4)} SOL`;
  }
  return String(row.value ?? "—").slice(0, 18).toUpperCase();
}

export function leaderboardStatTexture(THREE, board = {}) {
  const rows = Array.isArray(board.rows) ? board.rows.slice(0, 5) : [];
  return canvasTexture(THREE, 768, 512, (context) => {
    context.clearRect(0, 0, 768, 512);
    roundedRect(context, 4, 4, 760, 504, 16);
    context.fillStyle = "rgba(6,17,14,0.96)";
    context.fill();
    context.strokeStyle = "#9ef7c6";
    context.lineWidth = 5;
    context.stroke();
    context.textBaseline = "middle";
    context.fillStyle = "#f1fff6";
    context.font = '700 43px "ForkMesh Favorit", system-ui, sans-serif';
    context.fillText(
      String(board.title || "LEADERBOARD").toUpperCase().slice(0, 30),
      38,
      58,
      692,
    );
    context.fillStyle = "#77d9ff";
    context.font = '400 18px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      String(board.subtitle || "LIVE PUBLIC RANKINGS").toUpperCase().slice(0, 48),
      38,
      101,
      692,
    );
    rows.forEach((row, index) => {
      const top = 154 + index * 65;
      const name = String(row?.name || row?.host || "participant")
        .trim()
        .slice(0, 24);
      context.fillStyle = index < 3 ? "#d9ffea" : "#b7c9c0";
      context.font = '700 23px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(`${index + 1}. ${name}`, 38, top, 430);
      const value = leaderboardStatValue(board, row);
      context.fillStyle = index < 3 ? "#9ef7c6" : "#77d9ff";
      context.font = '700 19px "ForkMesh Mono", ui-monospace, monospace';
      context.textAlign = "right";
      context.fillText(value, 730, top, 250);
      context.textAlign = "left";
    });
    if (!rows.length) {
      context.fillStyle = "#91a39a";
      context.font = '400 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("WAITING FOR PUBLIC DATA", 38, 194);
    }
    context.fillStyle = "#60756b";
    context.font = '400 16px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("LIVE WITH /LEADERBOARDS", 38, 475);
  });
}

export function makeLeaderboardStatSign(THREE, board = {}) {
  const sign = new THREE.Group();
  sign.name = `world-leaderboard-${String(board.id || "stats")}`;
  const base = new THREE.Mesh(
    sharedGeometry(THREE, "BoxGeometry", 4.8, 0.25, 1.35),
    makeSharedMaterial(THREE, "#173136", { roughness: 0.78 }),
  );
  base.position.y = 0.13;
  sign.add(base);
  for (const x of [-1.72, 1.72]) {
    const post = new THREE.Mesh(
      sharedGeometry(THREE, "BoxGeometry", 0.16, 3.7, 0.16),
      makeSharedMaterial(THREE, "#29645e", {
        emissive: "#113d36",
        emissiveIntensity: 0.3,
        metalness: 0.28,
        roughness: 0.48,
      }),
    );
    post.position.set(x, 1.85, 0);
    sign.add(post);
  }
  const boardFrame = new THREE.Mesh(
    sharedGeometry(THREE, "BoxGeometry", 3.72, 2.72, 0.12),
    makeSharedMaterial(THREE, "#071712", { roughness: 0.55 }),
  );
  boardFrame.position.set(0, 2.08, 0.08);
  sign.add(boardFrame);
  const face = new THREE.Mesh(
    sharedGeometry(THREE, "PlaneGeometry", 3.52, 2.55),
    resources.uniqueMaterial(THREE, {
      map: leaderboardStatTexture(THREE, board),
      transparent: true,
    }, "MeshBasicMaterial"),
  );
  face.position.set(0, 2.08, 0.151);
  sign.add(face);
  sign.userData.face = face;
  sign.userData.boardId = String(board.id || "");
  sign.userData.board = board;
  return sign;
}
