import { hashNumber } from "../world-avatar-face.js";
import { clipCanvasText } from "./boards.js";
import {
  canvasArtTexture,
  canvasTexture,
  roundedRect,
} from "./canvas.js";
import { makeLabelSprite } from "./labels.js";
import {
  REPOSITORY_GROUND_RADIUS,
  REPOSITORY_ISLAND_CENTER_X,
} from "./layout.js";
import { makeMaterial, makeSharedMaterial } from "./materials.js";
import { setShadows } from "./object-utils.js";
import { createDistrictGroundCircle } from "./town.js";
import * as resources from "./resources.js";

const REPOSITORY_SIZE_MAP_COLORS = Object.freeze([
  "#3987e5",
  "#199e70",
  "#c98500",
  "#008300",
  "#9085e9",
  "#e66767",
  "#d55181",
  "#d95926",
]);
const REPOSITORY_SIZE_MAP_GRAY = "#8a8a8a";
const REPOSITORY_SIZE_MAP_MAX_RINGS = 4;
const REPOSITORY_SIZE_MAP_MAX_SEGMENTS = 420;
export const REPOSITORY_CATALOG_MAX = 200;
export const REPOSITORY_EDGE_RADIUS = 68;

export function compactSceneBytes(value) {
  const bytes = Math.max(0, Number(value) || 0);
  if (bytes < 1024) return `${Math.round(bytes)} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let scaled = bytes / 1024;
  let index = 0;
  while (scaled >= 1024 && index < units.length - 1) {
    scaled /= 1024;
    index += 1;
  }
  return `${scaled >= 10 ? scaled.toFixed(0) : scaled.toFixed(1)} ${units[index]}`;
}

export function repositorySizeLabelSprite(THREE, title, subtitle, color) {
  const texture = canvasTexture(THREE, 512, 160, (context) => {
    context.clearRect(0, 0, 512, 160);
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.shadowColor = "rgba(0, 0, 0, 0.86)";
    context.shadowBlur = 9;
    context.shadowOffsetY = 2;
    context.font = '700 34px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#ffffff";
    context.fillText(String(title || "").slice(0, 20), 256, 61);
    context.font = '700 23px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = color || "#d9ffea";
    context.fillText(String(subtitle || "").slice(0, 24), 256, 111);
  });
  const sprite = new THREE.Sprite(
    new THREE.SpriteMaterial({
      map: texture,
      transparent: true,
      depthTest: true,
      depthWrite: false,
      toneMapped: false,
    }),
  );
  sprite.renderOrder = 12;
  return sprite;
}

function repositoryRecordCaptionSprite(THREE, title, subtitle, color) {
  const texture = canvasTexture(THREE, 1024, 180, (context) => {
    context.clearRect(0, 0, 1024, 180);
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.shadowColor = "rgba(0, 0, 0, 0.9)";
    context.shadowBlur = 10;
    context.shadowOffsetY = 2;
    context.fillStyle = "#ffffff";
    context.font = '800 42px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(title || "").slice(0, 58), 512, 66, 990);
    context.fillStyle = color || "#d9ffea";
    context.font = '700 28px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(subtitle || "").slice(0, 82), 512, 128, 990);
  });
  const sprite = new THREE.Sprite(
    new THREE.SpriteMaterial({
      map: texture,
      transparent: true,
      depthTest: true,
      depthWrite: false,
      toneMapped: false,
    }),
  );
  sprite.renderOrder = 12;
  return sprite;
}

export function repositoryRecordCountTexture(THREE, kind, count, detail, color) {
  const isIssue = kind === "issue";
  return canvasTexture(THREE, 1024, 360, (context) => {
    context.clearRect(0, 0, 1024, 360);
    roundedRect(context, 10, 10, 1004, 340, 30);
    context.fillStyle = isIssue
      ? "rgba(7, 27, 21, 0.98)"
      : "rgba(16, 12, 35, 0.98)";
    context.fill();
    context.lineWidth = 9;
    context.strokeStyle = color;
    context.stroke();
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = color;
    context.font = '900 205px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      Math.max(0, Number(count) || 0).toLocaleString("en-US"),
      42,
      184,
      500,
    );
    context.textAlign = "right";
    context.fillStyle = "#f8fff9";
    context.font = '900 58px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(isIssue ? "OPEN ISSUES" : "OPEN PRS", 968, 120);
    context.fillStyle = color;
    context.font = '700 29px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(detail || "").slice(0, 34), 968, 222);
    context.fillStyle = "#91a39a";
    context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(isIssue ? "ISSUE WORKBENCH" : "PULL REVIEW", 968, 286);
  });
}

export function repositoryRecordPageTexture(THREE, pageInfo, color) {
  const start = pageInfo.end > pageInfo.start ? pageInfo.start + 1 : 0;
  return repositoryRecordPagerTexture(
    THREE,
    `${pageInfo.page + 1} / ${pageInfo.pages}`,
    `${start}–${pageInfo.end}`,
    color,
  );
}

export function repositoryRecordPagerTexture(THREE, title, subtitle, color) {
  return canvasTexture(THREE, 512, 160, (context) => {
    context.clearRect(0, 0, 512, 160);
    roundedRect(context, 8, 8, 496, 144, 22);
    context.fillStyle = "rgba(7, 17, 14, 0.96)";
    context.fill();
    context.lineWidth = 7;
    context.strokeStyle = color || "#59655f";
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = "#f8fff9";
    context.font = '800 46px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(title || "").slice(0, 12), 256, 62);
    context.fillStyle = color || "#91a39a";
    context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(subtitle || "").slice(0, 12), 256, 116);
  });
}

export function repositoryIssueAgentProviderTexture(THREE, provider, active = false) {
  const claude = provider === "claude-code";
  const color = claude ? "#ef9f74" : "#8fffe0";
  return canvasTexture(THREE, 256, 256, (context) => {
    context.clearRect(0, 0, 256, 256);
    context.beginPath();
    context.arc(128, 128, 106, 0, Math.PI * 2);
    context.fillStyle = active ? color : "#071b17";
    context.fill();
    context.lineWidth = active ? 18 : 12;
    context.strokeStyle = color;
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = active ? "#07120e" : color;
    context.font = '900 82px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(claude ? "✣" : "<>", 128, 120);
    context.font = '800 28px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(claude ? "CLAUDE" : "CODEX", 128, 190);
  });
}

export function repositoryIssueAgentModelTexture(THREE, model, provider) {
  const color = provider === "claude-code" ? "#ef9f74" : "#8fffe0";
  return canvasTexture(THREE, 320, 144, (context) => {
    context.clearRect(0, 0, 320, 144);
    roundedRect(context, 5, 5, 310, 134, 28);
    context.fillStyle = "#071b17";
    context.fill();
    context.lineWidth = 8;
    context.strokeStyle = color;
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = color;
    context.font = '900 42px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(model || "").toUpperCase().slice(0, 6), 160, 74);
  });
}

export function repositoryStarPlaneTexture(THREE, count, starred = false) {
  // A single, fixed upright plane avoids the chunky extruded-star silhouette
  // at close range. Its transparent texture preserves the actual star shape
  // while keeping the complete repo_stars value readable at its centre.
  return canvasArtTexture(THREE, 512, 512, (context) => {
    context.clearRect(0, 0, 512, 512);
    const outerRadius = 224;
    const innerRadius = 104;
    context.beginPath();
    for (let point = 0; point < 10; point += 1) {
      const angle = -Math.PI / 2 + point * (Math.PI / 5);
      const radius = point % 2 === 0 ? outerRadius : innerRadius;
      const x = 256 + Math.cos(angle) * radius;
      const y = 256 + Math.sin(angle) * radius;
      if (point === 0) context.moveTo(x, y);
      else context.lineTo(x, y);
    }
    context.closePath();
    context.fillStyle = starred ? "#f7c96b" : "#9ef7c6";
    context.fill();
    context.lineWidth = 14;
    context.strokeStyle = starred ? "#b87418" : "#20764f";
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = "#07120e";
    const value = Number.isSafeInteger(count)
      ? count.toLocaleString("en-US")
      : "—";
    let size = 168;
    do {
      context.font = `900 ${size}px "ForkMesh Mono", ui-monospace, monospace`;
      size -= 8;
    } while (size > 40 && context.measureText(value).width > 230);
    context.fillText(value, 256, 265);
  });
}

function repositoryStonePlaqueTexture(THREE, repositoryName) {
  return canvasArtTexture(THREE, 1024, 220, (context) => {
    context.fillStyle = "#89928b";
    context.fillRect(0, 0, 1024, 220);
    context.strokeStyle = "#c9d4c9";
    context.lineWidth = 14;
    context.strokeRect(8, 8, 1008, 204);
    context.fillStyle = "#102019";
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = '800 76px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(repositoryName || "repository").slice(0, 42), 512, 112);
  });
}

export function repositoryCommitActivityTexture(
  THREE,
  activity = {},
  repositoryName = "",
  placardStats = {},
) {
  const weeks = Array.isArray(activity?.weeks)
    ? activity.weeks.slice(-52).map((value) => {
        const count = Number(value);
        return Number.isFinite(count) && count > 0 ? Math.floor(count) : 0;
      })
    : [];
  while (weeks.length < 52) weeks.unshift(0);
  const metrics =
    activity?.metrics && typeof activity.metrics === "object"
      ? activity.metrics
      : {};
  const status = String(activity?.status || "unavailable");
  const max = Math.max(1, ...weeks);
  const total = weeks.reduce((sum, count) => sum + count, 0);
  const runningAgents = Math.max(
    0,
    Number(placardStats?.runningAgents) || 0,
  );
  const followerCount = Number(placardStats?.fediverseFollowers);
  const followers = Number.isFinite(followerCount)
    ? Math.max(0, Math.floor(followerCount))
    : null;
  const rateLabel = (metric) => {
    const rate = Number(metric?.commitsPerHour);
    if (!Number.isFinite(rate) || rate < 0) return "—";
    const formatted =
      rate >= 100
        ? rate.toFixed(0)
        : rate >= 10
          ? rate.toFixed(1)
          : rate.toFixed(2);
    return `${metric?.exact === false ? "≥" : ""}${formatted}/HR`;
  };
  return canvasTexture(THREE, 1536, 512, (context) => {
    context.clearRect(0, 0, 1536, 512);
    roundedRect(context, 8, 8, 1520, 496, 28);
    context.fillStyle = "rgba(5, 17, 14, 0.98)";
    context.fill();
    context.lineWidth = 8;
    context.strokeStyle = "#77d9ff";
    context.stroke();

    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#f8fff9";
    context.font = '900 46px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      String(repositoryName || "repository").slice(0, 42),
      54,
      54,
      820,
    );
    context.fillStyle = "#9ef7c6";
    context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
    const subtitle =
      status === "loading"
        ? "READING PINNED MIRROR HISTORY…"
        : status === "unavailable"
          ? "LIVE COMMIT HISTORY UNAVAILABLE"
          : status === "empty"
            ? "NO COMMIT ACTIVITY IN THIS WINDOW"
            : `52 WEEKS · ${total.toLocaleString("en-US")} COMMITS`;
    context.fillText(`COMMIT ACTIVITY · ${subtitle}`, 56, 104, 820);

    [
      {
        x: 930,
        width: 260,
        label: "AGENT CONTROL",
        value: `${runningAgents.toLocaleString("en-US")} RUNNING`,
        color: "#8fffe0",
      },
      {
        x: 1204,
        width: 276,
        label: "FEDIVERSE",
        value:
          followers === null
            ? "— FOLLOWERS"
            : `${followers.toLocaleString("en-US")} ${
                followers === 1 ? "FOLLOWER" : "FOLLOWERS"
              }`,
        color: "#d5b6ff",
      },
    ].forEach((badge) => {
      roundedRect(context, badge.x, 25, badge.width, 94, 16);
      context.fillStyle = "rgba(119, 217, 255, 0.08)";
      context.fill();
      context.lineWidth = 4;
      context.strokeStyle = badge.color;
      context.stroke();
      context.textAlign = "left";
      context.fillStyle = "#91a39a";
      context.font = '700 18px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(badge.label, badge.x + 18, 52);
      context.fillStyle = badge.color;
      context.font = '900 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(badge.value, badge.x + 18, 88, badge.width - 36);
    });

    const chartLeft = 58;
    const chartTop = 144;
    const chartWidth = 1420;
    const chartHeight = 166;
    const gap = 5;
    const barWidth = (chartWidth - gap * 51) / 52;
    context.fillStyle = "rgba(119, 217, 255, 0.09)";
    roundedRect(context, chartLeft - 12, chartTop - 12, chartWidth + 24, chartHeight + 24, 14);
    context.fill();
    weeks.forEach((count, index) => {
      const height = count > 0
        ? Math.max(10, Math.round((count / max) * chartHeight))
        : 6;
      const x = chartLeft + index * (barWidth + gap);
      const y = chartTop + chartHeight - height;
      roundedRect(context, x, y, barWidth, height, Math.min(5, barWidth / 2));
      context.fillStyle = count > 0
        ? "#77d9ff"
        : status === "loading"
          ? "rgba(145, 163, 154, 0.42)"
          : "rgba(145, 163, 154, 0.24)";
      context.fill();
    });
    context.fillStyle = "#91a39a";
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("52 WEEKS AGO", 58, 340);
    context.textAlign = "right";
    context.fillText("NOW", 1478, 340);

    const metricRows = [
      ["TODAY", metrics.today],
      ["THIS WEEK", metrics.week],
      ["THIS MONTH", metrics.month],
    ];
    metricRows.forEach(([label, metric], index) => {
      const x = 54 + index * 490;
      roundedRect(context, x, 370, 460, 102, 16);
      context.fillStyle = "rgba(119, 217, 255, 0.08)";
      context.fill();
      context.textAlign = "left";
      context.fillStyle = "#91a39a";
      context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(`${label} · UTC`, x + 22, 400);
      context.fillStyle = "#f8fff9";
      context.font = '800 38px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(rateLabel(metric), x + 22, 444);
      context.textAlign = "right";
      context.fillStyle = "#9ef7c6";
      context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
      const commits = Number(metric?.commits);
      context.fillText(
        Number.isFinite(commits)
          ? `${metric?.exact === false ? "≥" : ""}${Math.max(0, commits).toLocaleString("en-US")} COMMIT${commits === 1 ? "" : "S"}`
          : "NO DATA",
        x + 438,
        444,
      );
    });
  });
}

const REPOSITORY_RECORD_STATE_COLORS = {
  open: "#9ef7c6",
  ready: "#70d99d",
  conflict: "#ff7189",
  draft: "#f7c96b",
  closed: "#8fa39a",
  merged: "#d5b6ff",
  unavailable: "#91a39a",
};

export function repositoryPullReviewStatus(pull = {}) {
  const state = String(pull?.state || "").toLowerCase();
  const explicit = String(
    pull?.reviewStatus ||
      pull?.mergeStatus ||
      pull?.mergeability ||
      "",
  ).toLowerCase();
  if (state === "merged" || explicit === "merged") return "merged";
  if (pull?.metadataAvailable === false) return "unavailable";
  if (
    pull?.hasConflicts === true ||
    pull?.conflict === true ||
    /\b(conflict|conflicting|dirty|blocked)\b/.test(explicit)
  ) {
    return "conflict";
  }
  if (pull?.draft === true || explicit === "draft") return "draft";
  if (state === "closed" || explicit === "closed") return "closed";
  if (
    pull?.mergeable === true ||
    /\b(ready|clean|mergeable)\b/.test(explicit)
  ) {
    return "ready";
  }
  // An immutable open record is truthful, but it is not proof that a merge
  // simulation is conflict-free. Keep the status OPEN until a mirror reports
  // an explicit readiness/conflict result.
  if (state === "open") return "open";
  return "unavailable";
}

function repositoryPullMergeabilityScore(pull = {}) {
  const state = String(pull?.state || "").toLowerCase();
  const review = String(
    pull?.reviewStatus || pull?.mergeStatus || pull?.mergeability || "",
  ).toLowerCase();
  const checks = String(
    pull?.checksStatus || pull?.checkStatus || pull?.ciStatus || "",
  ).toLowerCase();
  const factors = [];
  if (state === "merged") {
    return { score: 100, label: "MERGED", factors: ["MERGED+"], terminal: true };
  }
  if (state === "closed") {
    return { score: 0, label: "CLOSED", factors: ["CLOSED"], terminal: true };
  }
  if (pull?.metadataAvailable === false) {
    return {
      score: 0,
      label: "NO DATA",
      factors: ["META-", "REFS?", "CLEAN?", "REVIEW?", "CI?"],
      terminal: false,
    };
  }

  let score = 10;
  factors.push("META+");
  if (state === "open") {
    score += 10;
    factors.push("OPEN+");
  } else {
    factors.push("OPEN?");
  }

  const base = String(pull?.base || "").trim();
  const head = String(pull?.head || "").trim();
  if (base && head && base !== head) {
    score += 10;
    factors.push("REFS+");
  } else {
    factors.push("REFS-");
  }

  const oidPattern = /^(?:[0-9a-f]{40}|[0-9a-f]{64})$/i;
  if (
    oidPattern.test(String(pull?.creationBaseOid || "")) &&
    oidPattern.test(String(pull?.creationHeadOid || ""))
  ) {
    score += 15;
    factors.push("OIDS+");
  } else {
    factors.push("OIDS?");
  }

  if (pull?.draft === true) {
    factors.push("DRAFT-");
  } else {
    score += 10;
    factors.push("DRAFT+");
  }

  const conflicts =
    pull?.hasConflicts === true ||
    pull?.conflict === true ||
    pull?.mergeable === false ||
    /\b(conflict|conflicting|dirty|blocked)\b/.test(review);
  const clean =
    pull?.hasConflicts === false ||
    pull?.mergeable === true ||
    /\b(clean|mergeable)\b/.test(review);
  if (conflicts) {
    factors.push("CLEAN-");
  } else if (clean) {
    score += 25;
    factors.push("CLEAN+");
  } else {
    factors.push("CLEAN?");
  }

  if (/\b(approved|approval|ready)\b/.test(review)) {
    score += 10;
    factors.push("REVIEW+");
  } else if (/\b(changes|rejected|blocked)\b/.test(review)) {
    factors.push("REVIEW-");
  } else {
    factors.push("REVIEW?");
  }

  const checksPassing = /\b(pass|passed|passing|success|successful|green)\b/.test(checks);
  const checksFailing = /\b(fail|failed|failing|error|red|cancel)\b/.test(checks);
  if (checksPassing) {
    score += 10;
    factors.push("CI+");
  } else if (checksFailing) {
    factors.push("CI-");
  } else {
    factors.push("CI?");
  }

  if (conflicts) score = Math.min(score, 24);
  if (checksFailing) score = Math.min(score, 49);
  if (pull?.draft === true) score = Math.min(score, 59);
  score = Math.max(0, Math.min(100, Math.round(score)));
  const label =
    score >= 85 ? "HIGH" : score >= 65 ? "GOOD" : score >= 45 ? "CHECK" : "LOW";
  return { score, label, factors, terminal: false };
}

export function repositoryIssuePageTexture(THREE, issue, expanded, repositoryName) {
  const width = expanded ? 512 : 256;
  const height = expanded ? 672 : 336;
  return canvasTexture(THREE, width, height, (context) => {
    context.fillStyle = "#f6f1e2";
    context.fillRect(0, 0, width, height);
    context.strokeStyle = "#c8bfa4";
    context.lineWidth = expanded ? 6 : 4;
    context.strokeRect(2, 2, width - 4, height - 4);
    const accent =
      REPOSITORY_RECORD_STATE_COLORS[issue.state] || "#f7c96b";
    context.fillStyle = accent;
    context.fillRect(0, 0, width, expanded ? 88 : 52);
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = "#17251d";
    context.font = `900 ${expanded ? 52 : 30}px "ForkMesh Mono", ui-monospace, monospace`;
    context.fillText(`ISSUE #${issue.number}`, width / 2, expanded ? 46 : 27);
    context.fillStyle = "#3d4a41";
    context.font = `700 ${expanded ? 34 : 24}px "ForkMesh Mono", ui-monospace, monospace`;
    context.fillText(
      (issue.state || "recorded").toUpperCase(),
      width / 2,
      expanded ? 140 : 88,
    );
    // Ruled page lines keep the sheet reading as a document, not a button.
    context.strokeStyle = "rgba(84, 96, 88, 0.35)";
    context.lineWidth = expanded ? 3 : 2;
    const firstRule = expanded ? 200 : 120;
    const ruleGap = expanded ? 46 : 30;
    for (let y = firstRule; y < height - (expanded ? 130 : 36); y += ruleGap) {
      context.beginPath();
      context.moveTo(width * 0.12, y);
      context.lineTo(width * 0.88, y);
      context.stroke();
    }
    if (expanded) {
      context.fillStyle = "#17251d";
      context.font = '800 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(String(issue.title || `Issue #${issue.number}`).slice(0, 28), 256, 214);
      context.fillStyle = "#3d4a41";
      context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `BY ${String(issue.author || "Unknown").slice(0, 24)}`,
        256,
        260,
      );
      context.fillStyle = "#3d4a41";
      context.font = '700 26px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(String(repositoryName || "").slice(0, 30), 256, 580);
      context.fillStyle = "#1f6b46";
      context.font = '800 28px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText("CLICK AGAIN TO OPEN", 256, 622);
    }
  });
}

function repositoryRecordDateLabel(value) {
  const stamp = Number(value);
  if (!Number.isFinite(stamp) || stamp <= 0) return "";
  const date = new Date(stamp);
  return Number.isNaN(date.getTime()) ? "" : date.toISOString().slice(0, 10);
}

export function repositoryIssueCardTexture(THREE, issue) {
  return canvasTexture(THREE, 1024, 300, (context) => {
    context.fillStyle = "#101f1a";
    context.fillRect(0, 0, 1024, 300);
    const accent =
      REPOSITORY_RECORD_STATE_COLORS[issue.state] || "#f7c96b";
    context.fillStyle = accent;
    context.fillRect(0, 0, 18, 300);
    context.textBaseline = "middle";
    context.textAlign = "left";
    context.fillStyle = "#f7c96b";
    context.font = '900 56px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(`#${issue.number}`, 52, 58);
    context.textAlign = "right";
    context.fillStyle = accent;
    context.font = '800 34px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText((issue.state || "recorded").toUpperCase(), 710, 58);
    context.fillStyle = "rgba(143,255,224,0.07)";
    context.fillRect(742, 18, 264, 264);
    context.strokeStyle = "rgba(143,255,224,0.28)";
    context.lineWidth = 3;
    context.beginPath();
    context.moveTo(742, 24);
    context.lineTo(742, 276);
    context.stroke();
    context.textAlign = "left";
    context.fillStyle = "#f1fff6";
    context.font = '800 42px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      clipCanvasText(
        context,
        String(issue.title || `Issue #${issue.number}`),
        640,
      ),
      52,
      138,
    );
    context.fillStyle = "#9fb8ad";
    context.font = '700 29px "ForkMesh Mono", ui-monospace, monospace';
    const meta = [
      `BY ${String(issue.author || "Unknown").slice(0, 22)}`,
      repositoryRecordDateLabel(issue.updatedAt || issue.createdAt),
      Array.isArray(issue.labels) && issue.labels.length
        ? issue.labels.slice(0, 2).join(", ")
        : "",
      Array.isArray(issue.assignees) && issue.assignees.length
        ? `→ ${issue.assignees.slice(0, 2).join(", ")}`
        : "",
    ]
      .filter(Boolean)
      .join(" · ");
    context.fillText(clipCanvasText(context, meta, 640), 52, 220);
    context.fillStyle = issue.metadataAvailable === false ? "#f0c66f" : "#70d99d";
    context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      issue.metadataAvailable === false
        ? "NUMBER/STATE ONLY"
        : "COMMIT-PINNED ISSUE RECORD",
      52,
      270,
    );
  });
}

export function repositoryPullCardTexture(THREE, pull) {
  return canvasTexture(THREE, 1024, 300, (context) => {
    const metadataAvailable = pull.metadataAvailable !== false;
    context.fillStyle = "#171429";
    context.fillRect(0, 0, 1024, 300);
    const reviewStatus = repositoryPullReviewStatus(pull);
    const mergeability = repositoryPullMergeabilityScore(pull);
    const accent =
      REPOSITORY_RECORD_STATE_COLORS[reviewStatus] || "#d5b6ff";
    context.fillStyle = accent;
    context.fillRect(0, 0, 18, 300);
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.fillStyle = "#d5b6ff";
    context.font = '900 56px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(`#${pull.number}`, 52, 58);
    context.fillStyle = accent;
    context.font = '800 26px "ForkMesh Mono", ui-monospace, monospace';
    context.textAlign = "right";
    context.fillText(reviewStatus.toUpperCase(), 790, 50);
    context.fillStyle =
      mergeability.score >= 65
        ? "#70d99d"
        : mergeability.score >= 45
          ? "#f7c96b"
          : "#ff7189";
    context.font = '900 34px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(`${mergeability.score}%`, 980, 46);
    context.font = '800 18px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(`${mergeability.label} MERGE`, 980, 78);
    context.textAlign = "left";
    context.fillStyle = "#f1edff";
    context.font = '800 42px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      clipCanvasText(context, String(pull.title || ""), 920),
      52,
      138,
    );
    context.fillStyle = "#b9b3d4";
    context.font = '700 29px "ForkMesh Mono", ui-monospace, monospace';
    const branch = pull.head && pull.base ? `${pull.head} → ${pull.base}` : "";
    const meta = [
      `BY ${String(pull.author || "Unknown").slice(0, 22)}`,
      repositoryRecordDateLabel(pull.createdAt),
      branch,
    ]
      .filter(Boolean)
      .join(" · ");
    context.fillText(clipCanvasText(context, meta, 920), 52, 220);
    context.fillStyle =
      !metadataAvailable || reviewStatus === "unavailable"
        ? "#f0c66f"
        : "#9e8cff";
    context.font = '700 21px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      clipCanvasText(context, mergeability.factors.join(" · "), 920),
      52,
      270,
    );
  });
}

const REPOSITORY_FOLLOWER_ACCENTS = [
  "#d5b6ff",
  "#8c8dff",
  "#ff9eb7",
  "#77d9ff",
  "#9ef7c6",
  "#f7c96b",
];
const REPOSITORY_FOLLOWER_SKINS = [
  "#d59a70",
  "#9a6043",
  "#f0bd91",
  "#704832",
  "#c98255",
];
export const REPOSITORY_FOLLOWERS_VISIBLE = 8;

function repositoryFollowerSeed(follower) {
  return hashNumber(
    `${follower?.handle || ""}|${follower?.profileUrl || ""}`,
  );
}

function repositoryFollowerInitials(follower) {
  const source = String(follower?.name || follower?.handle || "")
    .replace(/^@+/, "")
    .trim();
  const parts = source.split(/[\s._@\-/]+/u).filter(Boolean);
  const initials = parts
    .slice(0, 2)
    .map((part) => [...part][0] || "")
    .join("");
  return (initials || "?").toLocaleUpperCase().slice(0, 2);
}

function repositoryFollowerFollowedLabel(followedAt) {
  const stamp = Number(followedAt);
  if (!Number.isSafeInteger(stamp) || stamp <= 0) return "";
  const date = new Date(stamp);
  if (Number.isNaN(date.getTime())) return "";
  return `FOLLOWING SINCE ${date.toISOString().slice(0, 10)}`;
}

export function canvasReadableImageURL(value) {
  const raw = String(value || "").trim();
  if (!raw || raw.length > 120_000) return "";
  try {
    const url = new URL(raw, window.location.href);
    // Canvas/WebGL may only sample same-origin or embedded images unless the
    // remote response explicitly opts into CORS. Mastodon media commonly does
    // not, and browsers provide no preflight-free way to discover that without
    // logging a CORS failure. Keep the generated fallback for those URLs.
    if (
      url.origin !== window.location.origin &&
      url.protocol !== "data:" &&
      url.protocol !== "blob:"
    ) {
      return "";
    }
    return url.href;
  } catch (_) {
    return "";
  }
}

function loadRepositoryFollowerAvatar(url, onLoad) {
  const readableURL = canvasReadableImageURL(url);
  if (!readableURL) return;
  const image = new Image();
  image.crossOrigin = "anonymous";
  image.referrerPolicy = "no-referrer";
  image.decoding = "async";
  image.onload = () => {
    if (image.naturalWidth > 0 && image.naturalHeight > 0) onLoad(image);
  };
  image.onerror = () => {};
  image.src = readableURL;
}

function drawRepositoryFollowerAvatar(
  context,
  follower,
  accent,
  image,
  x,
  y,
  size,
) {
  const radius = size / 2;
  context.save();
  context.beginPath();
  context.arc(x + radius, y + radius, radius, 0, Math.PI * 2);
  context.closePath();
  context.clip();
  if (image) {
    // Cover-crop the remote avatar into the circle without distorting it.
    const scale = size / Math.min(image.naturalWidth, image.naturalHeight);
    const width = image.naturalWidth * scale;
    const height = image.naturalHeight * scale;
    context.drawImage(
      image,
      x + (size - width) / 2,
      y + (size - height) / 2,
      width,
      height,
    );
  } else {
    context.fillStyle = "#0b1c19";
    context.fillRect(x, y, size, size);
    context.fillStyle = accent;
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.font = `800 ${Math.round(size * 0.42)}px "ForkMesh Mono", ui-monospace, monospace`;
    context.fillText(
      repositoryFollowerInitials(follower),
      x + radius,
      y + radius + size * 0.02,
    );
  }
  context.restore();
  context.beginPath();
  context.arc(x + radius, y + radius, radius - 2, 0, Math.PI * 2);
  context.strokeStyle = accent;
  context.lineWidth = 5;
  context.stroke();
}

function repositoryFollowerCardTexture(THREE, follower, accent, image = null) {
  // One follower, with everything the fediverse publishes about them: avatar,
  // display name, full @handle, home instance, bio, and when the follow landed.
  return canvasTexture(THREE, 560, 320, (context) => {
    context.clearRect(0, 0, 560, 320);
    context.fillStyle = "rgba(7, 18, 15, 0.9)";
    roundedRect(context, 6, 6, 548, 308, 26);
    context.fill();
    context.strokeStyle = accent;
    context.lineWidth = 4;
    context.stroke();
    drawRepositoryFollowerAvatar(context, follower, accent, image, 30, 34, 108);
    context.textAlign = "left";
    context.textBaseline = "middle";
    const name = String(follower?.name || "").trim();
    const handle = String(follower?.handle || "").trim();
    context.font = '800 40px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#f1fff6";
    context.fillText((name || handle || "fediverse account").slice(0, 20), 158, 66);
    context.font = '700 27px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = accent;
    context.fillText((handle || follower?.instance || "").slice(0, 28), 158, 106);
    context.font = '700 22px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#9fb8ad";
    const meta = [
      String(follower?.instance || "").slice(0, 26),
      repositoryFollowerFollowedLabel(follower?.followedAt),
    ]
      .filter(Boolean)
      .join(" · ");
    context.fillText(meta.slice(0, 46), 158, 140);
    const about = String(follower?.about || "").trim();
    context.font = '600 24px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = about ? "#d9ffea" : "#65776f";
    const words = (about || "No public bio.").split(/\s+/u);
    let line = "";
    let row = 0;
    for (const word of words) {
      const candidate = line ? `${line} ${word}` : word;
      if (context.measureText(candidate).width > 494 && line) {
        context.fillText(line, 32, 196 + row * 36);
        row += 1;
        line = word;
        if (row >= 3) break;
      } else {
        line = candidate;
      }
    }
    if (row < 3 && line) context.fillText(line, 32, 196 + row * 36);
  });
}

function repositoryFollowerIconTexture(THREE, follower, accent, image = null) {
  // A pure circular avatar belongs in the orbit. The previous tall profile
  // card hid the file wedges and read like another repository control.
  return canvasTexture(THREE, 256, 256, (context) => {
    context.clearRect(0, 0, 256, 256);
    drawRepositoryFollowerAvatar(
      context,
      follower,
      accent,
      image,
      20,
      20,
      216,
    );
  });
}

// How long a freshly risen portal keeps shimmering while its size tree is
// still being assembled. A map that never arrives fades out instead of leaving
// a permanent decoration on a repository nothing is happening to.
export const REPOSITORY_SPARKLE_MS = 11_000;

// The motes that orbit a portal while its size map is being built. Seeds are
// carried on the object so the animation loop can move every mote from one
// buffer write per frame instead of allocating.
export function makeRepositorySizeMapSparkle(THREE, radius, label) {
  const count = 26;
  const positions = new Float32Array(count * 3);
  const seeds = new Float32Array(count * 3);
  for (let index = 0; index < count; index += 1) {
    const angle = (index / count) * Math.PI * 2;
    // Radius, vertical phase, and angular speed. The staggered radii read as a
    // shell of dust around the disk rather than a single flat ring.
    seeds[index * 3] = radius * (1.06 + (index % 5) * 0.16);
    seeds[index * 3 + 1] = angle;
    seeds[index * 3 + 2] = 0.9 + (index % 7) * 0.14;
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  const sparkle = new THREE.Points(
    geometry,
    new THREE.PointsMaterial({
      color: "#a5f3fc",
      size: 0.17,
      transparent: true,
      opacity: 0,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
      toneMapped: false,
    }),
  );
  sparkle.name = `repository-size-map-sparkle:${label}`;
  sparkle.position.z = 0.14;
  sparkle.userData.repositorySparkleSeeds = seeds;
  return sparkle;
}

export function makeRepositoryFollowerIcon(THREE, follower) {
  const seed = repositoryFollowerSeed(follower);
  const accent =
    REPOSITORY_FOLLOWER_ACCENTS[seed % REPOSITORY_FOLLOWER_ACCENTS.length];
  // Use the same fixed face-aligned plane geometry as the repository star.
  // A Sprite nested under the rotated portal face was present in the scene but
  // could disappear entirely at the elevated camera angles used for the
  // sunburst. The plane remains a real circular portrait at every angle.
  const icon = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 1, 1),
    resources.uniqueMaterial(THREE, {
      map: repositoryFollowerIconTexture(THREE, follower, accent),
      transparent: true,
      // These are UI-like identity badges pinned to the perimeter of a dense
      // stack of file wedges. Letting the depth buffer participate caused the
      // repository disk to erase the badges from common elevated camera
      // angles even though their scene objects were present and visible.
      depthTest: false,
      depthWrite: false,
      side: THREE.DoubleSide,
      forceSinglePass: true,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  icon.name = `repository-follower-icon:${follower.handle || follower.profileUrl}`;
  icon.scale.set(1.14, 1.14, 1);
  icon.renderOrder = 40;
  icon.userData.repositoryFollower = { ...follower };
  loadRepositoryFollowerAvatar(follower.avatar, (image) => {
    if (!icon.parent) return;
    const next = repositoryFollowerIconTexture(THREE, follower, accent, image);
    icon.material.map?.dispose?.();
    icon.material.map = next;
    icon.material.needsUpdate = true;
  });
  return icon;
}

function repositoryContributorIconTexture(THREE, contributor, image = null) {
  const label = String(contributor?.label || "Contributor").trim();
  const initials = label
    .split(/\s+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((part) => part[0]?.toUpperCase() || "")
    .join("") || "C";
  return canvasTexture(THREE, 192, 192, (context) => {
    context.clearRect(0, 0, 192, 192);
    if (image) {
      context.save();
      context.beginPath();
      context.arc(96, 96, 78, 0, Math.PI * 2);
      context.clip();
      const ratio = Math.max(156 / image.width, 156 / image.height);
      const width = image.width * ratio;
      const height = image.height * ratio;
      context.drawImage(image, 96 - width / 2, 96 - height / 2, width, height);
      context.restore();
    } else {
      const seed = repositoryFollowerSeed({
        handle: label,
        name: label,
      });
      context.beginPath();
      context.arc(96, 96, 78, 0, Math.PI * 2);
      context.fillStyle =
        REPOSITORY_FOLLOWER_ACCENTS[
          seed % REPOSITORY_FOLLOWER_ACCENTS.length
        ];
      context.fill();
      context.fillStyle = "#07120e";
      context.textAlign = "center";
      context.textBaseline = "middle";
      context.font = '900 62px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(initials.slice(0, 2), 96, 99);
    }
    context.beginPath();
    context.arc(96, 96, 84, 0, Math.PI * 2);
    context.lineWidth = 12;
    context.strokeStyle = "#9ef7c6";
    context.stroke();
  });
}

export function makeRepositoryContributorIcon(THREE, contributor) {
  const icon = new THREE.Mesh(
    resources.primitive(THREE, "PlaneGeometry", 1, 1),
    resources.uniqueMaterial(THREE, {
      map: repositoryContributorIconTexture(THREE, contributor),
      transparent: true,
      depthTest: false,
      depthWrite: false,
      side: THREE.DoubleSide,
      forceSinglePass: true,
      toneMapped: false,
    }, "MeshBasicMaterial"),
  );
  icon.name = `repository-contributor-icon:${contributor?.id || contributor?.label || ""}`;
  icon.scale.set(0.64, 0.64, 1);
  icon.renderOrder = 39;
  icon.userData.landmark = "repositories";
  icon.userData.graphNode = {
    id: String(contributor?.id || "").slice(0, 96),
    kind: "contributor",
    label: String(contributor?.label || "Contributor").slice(0, 100),
    detail: String(contributor?.detail || "").slice(0, 240),
    href: "",
  };
  loadRepositoryFollowerAvatar(contributor?.avatarUrl, (image) => {
    if (!icon.parent) return;
    const next = repositoryContributorIconTexture(
      THREE,
      contributor,
      image,
    );
    icon.material.map?.dispose?.();
    icon.material.map = next;
    icon.material.needsUpdate = true;
  });
  return icon;
}

export function repositoryFollowButtonTexture(THREE, handle = "") {
  return canvasTexture(THREE, 512, 224, (context) => {
    context.clearRect(0, 0, 512, 224);
    roundedRect(context, 12, 12, 488, 200, 74);
    context.fillStyle = "#6364ff";
    context.fill();
    context.lineWidth = 12;
    context.strokeStyle = "#b8b9ff";
    context.stroke();
    context.textAlign = "center";
    context.textBaseline = "middle";
    context.fillStyle = "#ffffff";
    context.font = '900 68px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText("FOLLOW", 256, 88);
    context.fillStyle = "#e8e8ff";
    context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(String(handle || "ON MASTODON").slice(0, 34), 256, 154);
  });
}

function makeRepositoryFollowerFigure(THREE, follower) {
  // A seated visitor on the ground under the repository circle, leaning back to
  // look up at it. Purpose-built (not the player avatar) so the crowd stays
  // small, cheap, and clearly a gallery rather than another walking body.
  const seed = repositoryFollowerSeed(follower);
  const accent =
    REPOSITORY_FOLLOWER_ACCENTS[seed % REPOSITORY_FOLLOWER_ACCENTS.length];
  const group = new THREE.Group();
  group.name = `repository-follower:${follower.handle || follower.profileUrl}`;
  const cloth = makeMaterial(THREE, accent, {
    emissive: accent,
    emissiveIntensity: 0.14,
    roughness: 0.74,
    metalness: 0.06,
  });
  const skin = makeMaterial(
    THREE,
    REPOSITORY_FOLLOWER_SKINS[seed % REPOSITORY_FOLLOWER_SKINS.length],
    { roughness: 0.92 },
  );
  const dark = makeMaterial(THREE, "#12241f", { roughness: 0.86 });

  const legs = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 1.12, 0.34, 0.94), dark);
  legs.position.set(0, 0.19, -0.2);
  group.add(legs);
  const torso = new THREE.Mesh(
    resources.primitive(THREE, "CapsuleGeometry", 0.38, 0.56, 6, 14),
    cloth,
  );
  torso.position.set(0, 0.82, 0.06);
  // Leaning back is what makes the pose read as "looking up at the circle".
  torso.rotation.x = 0.22;
  group.add(torso);
  const armGeometry = resources.primitive(THREE, "CapsuleGeometry", 0.12, 0.44, 5, 10);
  const leftArm = new THREE.Mesh(armGeometry, cloth);
  leftArm.position.set(-0.46, 0.62, -0.1);
  leftArm.rotation.set(0.5, 0, 0.42);
  group.add(leftArm);
  const rightArm = new THREE.Mesh(armGeometry, cloth);
  rightArm.position.set(0.46, 0.62, -0.1);
  rightArm.rotation.set(0.5, 0, -0.42);
  group.add(rightArm);
  const head = new THREE.Mesh(resources.primitive(THREE, "SphereGeometry", 0.33, 20, 16), skin);
  head.position.set(0, 1.44, 0.16);
  group.add(head);
  const hair = new THREE.Mesh(
    resources.primitive(THREE, "SphereGeometry", 0.35, 18, 12, 0, Math.PI * 2, 0, Math.PI * 0.55),
    dark,
  );
  hair.position.set(0, 1.5, 0.14);
  // Tilted back with the head, so the face stays clear while they look up.
  hair.rotation.x = 0.3;
  group.add(hair);
  const halo = new THREE.Mesh(
    resources.primitive(THREE, "TorusGeometry", 0.42, 0.03, 6, 28),
    makeMaterial(THREE, accent, {
      emissive: accent,
      emissiveIntensity: 1.1,
      metalness: 0.3,
      roughness: 0.3,
    }),
  );
  halo.position.set(0, 0.03, -0.16);
  halo.rotation.x = Math.PI / 2;
  group.add(halo);
  setShadows(group, true, true);

  const card = new THREE.Sprite(
    new THREE.SpriteMaterial({
      map: repositoryFollowerCardTexture(THREE, follower, accent),
      transparent: true,
      depthTest: true,
      depthWrite: false,
      toneMapped: false,
    }),
  );
  card.name = `repository-follower-card:${follower.handle || follower.profileUrl}`;
  card.scale.set(2.45, 1.4, 1);
  card.position.set(0, 2.05, 0);
  card.renderOrder = 13;
  group.add(card);
  loadRepositoryFollowerAvatar(follower.avatar, (image) => {
    // The layer may have been rebuilt (catalog refresh) while the avatar was
    // in flight; a detached card is dropped instead of repainted.
    if (!card.parent) return;
    const next = repositoryFollowerCardTexture(THREE, follower, accent, image);
    card.material.map?.dispose?.();
    card.material.map = next;
    card.material.needsUpdate = true;
  });
  group.userData.repositoryFollower = { ...follower };
  return group;
}

export function safeRepositoryPath(value) {
  const path = String(value || "");
  if (!path || path.length > 1000 || /[\u0000-\u001f\u007f]/u.test(path)) {
    return "";
  }
  const parts = path.split("/");
  if (
    parts.length > 64 ||
    parts.some((part) => !part || part === "." || part === "..")
  ) {
    return "";
  }
  return path;
}

function repositorySizeMapShade(hex, depth, index) {
  const factor = Math.min(
    0.5,
    Math.max(0, (depth - 1) * 0.13 + (depth > 1 ? (index % 2) * 0.055 : 0)),
  );
  const channel = (offset) => {
    const value = Number.parseInt(hex.slice(1 + offset * 2, 3 + offset * 2), 16);
    return Math.round(value + (235 - value) * factor);
  };
  return `rgb(${channel(0)}, ${channel(1)}, ${channel(2)})`;
}

function repositorySizeMapColors(root) {
  const colors = new Map();
  const walk = (node, depth, base, index) => {
    const hue =
      depth === 1
        ? node?.name !== "…" && index < REPOSITORY_SIZE_MAP_COLORS.length
          ? REPOSITORY_SIZE_MAP_COLORS[index]
          : REPOSITORY_SIZE_MAP_GRAY
        : base;
    colors.set(node, repositorySizeMapShade(hue, depth, index));
    const children = Array.isArray(node?.children) ? node.children : [];
    children.forEach((child, childIndex) =>
      walk(child, depth + 1, hue, childIndex),
    );
  };
  (Array.isArray(root?.children) ? root.children : []).forEach((child, index) =>
    walk(child, 1, REPOSITORY_SIZE_MAP_GRAY, index),
  );
  return colors;
}

function repositorySizeMapFocus(root, requestedPath) {
  const parts = safeRepositoryPath(requestedPath).split("/").filter(Boolean);
  let node = root;
  const trail = [];
  for (const part of parts) {
    const next = (Array.isArray(node?.children) ? node.children : []).find(
      (child) =>
        child?.type === "directory" && String(child.name || "") === part,
    );
    if (!next) return { node: root, path: "", trail: [] };
    node = next;
    trail.push(part);
  }
  return { node, path: trail.join("/"), trail };
}

export function repositorySizeMapSegments(root, requestedPath) {
  const focus = repositorySizeMapFocus(root, requestedPath);
  const colors = repositorySizeMapColors(root);
  const segments = [];
  const minSpan = (Math.PI * 2) / 900;
  let deepest = 1;
  const walk = (node, depth, from, span, parentPath) => {
    if (
      depth > REPOSITORY_SIZE_MAP_MAX_RINGS ||
      segments.length >= REPOSITORY_SIZE_MAP_MAX_SEGMENTS
    ) {
      return;
    }
    const size = Math.max(0, Number(node?.size) || 0);
    if (!(size > 0)) return;
    let at = from;
    const children = Array.isArray(node?.children) ? node.children : [];
    children.forEach((child) => {
      const childSize = Math.max(0, Number(child?.size) || 0);
      const childSpan = span * Math.min(1, childSize / size);
      const reportedPath = safeRepositoryPath(child?.path);
      const childName = String(child?.name || "item").slice(0, 100);
      const childPath =
        reportedPath ||
        safeRepositoryPath([parentPath, childName].filter(Boolean).join("/"));
      if (
        childSpan >= minSpan &&
        segments.length < REPOSITORY_SIZE_MAP_MAX_SEGMENTS
      ) {
        const segment = {
          node: child,
          name: childName,
          path: childPath,
          depth,
          from: at,
          to: at + childSpan,
          size: childSize,
          color: colors.get(child) || REPOSITORY_SIZE_MAP_GRAY,
          type:
            child?.type === "directory"
              ? "directory"
              : child?.type === "file"
                ? "file"
                : "summary",
        };
        segments.push(segment);
        deepest = Math.max(deepest, depth);
        if (segment.type === "directory") {
          walk(child, depth + 1, at, childSpan, childPath);
        }
      }
      at += childSpan;
    });
  };
  walk(focus.node, 1, -Math.PI / 2, Math.PI * 2, focus.path);
  return {
    ...focus,
    segments,
    ringCount: Math.max(1, Math.min(REPOSITORY_SIZE_MAP_MAX_RINGS, deepest)),
  };
}

export function repositoryWedgeGeometry(
  THREE,
  innerRadius,
  outerRadius,
  from,
  to,
  extrusion,
) {
  const span = Math.min(to - from, Math.PI * 2 - 0.0004);
  const gap = Math.min(0.018, span * 0.14);
  const start = from + gap * 0.5;
  const end = from + span - gap * 0.5;
  if (end <= start) return null;
  const shape = new THREE.Shape();
  shape.moveTo(
    Math.cos(start) * innerRadius,
    Math.sin(start) * innerRadius,
  );
  shape.lineTo(
    Math.cos(start) * outerRadius,
    Math.sin(start) * outerRadius,
  );
  shape.absarc(0, 0, outerRadius, start, end, false);
  shape.lineTo(Math.cos(end) * innerRadius, Math.sin(end) * innerRadius);
  shape.absarc(0, 0, innerRadius, end, start, true);
  shape.closePath();
  const geometry = new THREE.ExtrudeGeometry(shape, {
    depth: extrusion,
    steps: 1,
    curveSegments: 10,
    bevelEnabled: true,
    bevelSegments: 1,
    bevelSize: 0.012,
    bevelThickness: 0.018,
  });
  geometry.translate(0, 0, -extrusion * 0.5);
  return geometry;
}

export function createRepositoryDistrict(THREE, position, interactive, animated) {
  const group = new THREE.Group();
  const content = new THREE.Group();
  content.name = "repository-district-content";
  group.add(content);
  // Match the concrete apron to the live repository ring instead of leaving
  // an oversized blue foundation behind at the former landmark position.
  content.add(
    createDistrictGroundCircle(
      THREE,
      "repositories",
      REPOSITORY_GROUND_RADIUS,
    ),
  );
  const ringMaterial = makeMaterial(THREE, "#77d9ff", {
    metalness: 0.2,
    roughness: 0.28,
    emissive: "#1a6d8b",
    emissiveIntensity: 0.65,
  });
  const frameMaterial = makeMaterial(THREE, "#102a32", { roughness: 0.7 });

  const portal = new THREE.Group();
  const repositoryCore = new THREE.Group();
  repositoryCore.position.y = 3.35;
  const globe = new THREE.Mesh(
    resources.primitive(THREE, "IcosahedronGeometry", 0.72, 3),
    makeSharedMaterial(THREE, "#184a59", {
      metalness: 0.22,
      roughness: 0.35,
      emissive: "#1b7896",
      emissiveIntensity: 0.72,
      transparent: true,
      opacity: 0.92,
    }),
  );
  globe.name = "repository-world-core";
  repositoryCore.add(globe);
  [0.91, 1.08].forEach((radius, index) => {
    const orbit = new THREE.Mesh(
      new THREE.TorusGeometry(radius, 0.025, 6, 48),
      ringMaterial,
    );
    orbit.rotation.x = index ? Math.PI / 2.7 : Math.PI / 5.4;
    orbit.rotation.y = index ? Math.PI / 4 : -Math.PI / 5;
    repositoryCore.add(orbit);
  });
  portal.add(repositoryCore);

  const frameLeft = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 0.38, 6.6, 0.54),
    frameMaterial,
  );
  frameLeft.position.set(-4.25, 3.3, 0);
  portal.add(frameLeft);
  const frameRight = frameLeft.clone();
  frameRight.position.x = 4.25;
  portal.add(frameRight);

  const legacyFiles = new THREE.Group();
  legacyFiles.name = "repository-legacy-file-graph";
  const files = [];
  const fileColors = ["#77d9ff", "#9ef7c6", "#d5b6ff", "#f7c96b", "#ff9eb7"];
  for (let index = 0; index < 17; index += 1) {
    const size = 0.22 + (index % 5) * 0.07;
    const fileColor = fileColors[index % fileColors.length];
    const file = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", size, size * 1.22, 0.08),
      makeSharedMaterial(THREE, fileColor, {
        emissive: fileColor,
        emissiveIntensity: 0.18,
      }),
    );
    const layer = index % 3;
    const angle = (index / 17) * Math.PI * 2;
    const radius = 1.15 + layer * 0.5;
    file.position.set(Math.cos(angle) * radius, 3.1 + Math.sin(angle * 2) * 0.4, Math.sin(angle) * 0.3);
    file.userData = { angle, radius, layer, fileColor };
    files.push(file);
    legacyFiles.add(file);
  }
  portal.add(legacyFiles);
  // Repository navigation now lives on the perimeter portals and their
  // interactive sunbursts. Keep the plaque, but remove the obsolete central
  // globe/ring/upright centerpiece from the district.
  portal.visible = false;
  portal.userData.legacyPortalHidden = true;
  content.add(portal);

  group.position.set(...position);
  group.userData.landmark = "repositories";
  group.userData.fileMeshes = files;
  group.userData.legacyFiles = legacyFiles;
  group.userData.portal = portal;
  group.userData.repositoryContent = content;
  portal.userData.repositoryCore = repositoryCore;
  portal.userData.repositoryOrbitLayer = null;
  portal.userData.repositorySizeLayer = null;
  portal.userData.repositorySizeMeshes = [];

  // A physical import kiosk remains at the repository district even though
  // the old central repository portal is retired. Each provider pad opens the
  // same secure import flow with that provider preselected.
  const importKiosk = new THREE.Group();
  importKiosk.name = "repository-import-kiosk";
  importKiosk.position.set(0, 0, 0);
  const kioskBase = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 2.15, 2.35, 0.42, 8),
    makeSharedMaterial(THREE, "#102a32", {
      emissive: "#123d49",
      emissiveIntensity: 0.35,
      metalness: 0.25,
      roughness: 0.52,
    }),
  );
  kioskBase.position.y = 0.21;
  importKiosk.add(kioskBase);
  const kioskColumn = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.24, 0.34, 2.15, 10),
    frameMaterial,
  );
  kioskColumn.position.y = 1.45;
  importKiosk.add(kioskColumn);
  const providerPads = [
    { id: "github", label: "GITHUB", color: "#f0f6fc" },
    { id: "gitlab", label: "GITLAB", color: "#fc8d45" },
    { id: "codeberg", label: "CODEBERG", color: "#77d9ff" },
  ];
  providerPads.forEach((provider, index) => {
    const angle = -0.72 + index * 0.72;
    const pad = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 1.25, 0.2, 0.88),
      makeMaterial(THREE, provider.color, {
        emissive: provider.color,
        emissiveIntensity: 0.72,
        metalness: 0.22,
        roughness: 0.34,
      }),
    );
    pad.name = `repository-import-provider:${provider.id}`;
    pad.position.set(Math.sin(angle) * 1.45, 0.58, -Math.cos(angle) * 1.45);
    pad.rotation.y = -angle;
    pad.userData.landmark = "repositories";
    pad.userData.createRepository = provider.id;
    importKiosk.add(pad);
    const label = makeLabelSprite(
      THREE,
      provider.label,
      "IMPORT FROM",
      provider.color,
    );
    label.scale.set(1.18, 0.4, 1);
    label.position.copy(pad.position);
    label.position.y += 0.42;
    importKiosk.add(label);
  });
  const importBeam = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.08, 0.68, 3.5, 18, 1, true),
    makeMaterial(THREE, "#77d9ff", {
      emissive: "#39bfe8",
      emissiveIntensity: 1.1,
      transparent: true,
      opacity: 0.14,
      depthWrite: false,
      side: THREE.DoubleSide,
    }),
  );
  importBeam.name = "repository-import-beam";
  importBeam.position.y = 2.7;
  importBeam.visible = false;
  importKiosk.add(importBeam);
  const importSpark = new THREE.Mesh(
    resources.primitive(THREE, "IcosahedronGeometry", 0.28, 1),
    makeMaterial(THREE, "#9ef7c6", {
      emissive: "#42e99a",
      emissiveIntensity: 1.8,
      transparent: true,
      opacity: 0.92,
    }),
  );
  importSpark.name = "repository-import-spark";
  importSpark.position.y = 3.55;
  importSpark.visible = false;
  importKiosk.add(importSpark);
  const kioskSign = makeLabelSprite(
    THREE,
    "IMPORT REPOSITORY",
    "GITHUB · GITLAB · CODEBERG",
    "#9ef7c6",
  );
  kioskSign.scale.set(2.8, 0.78, 1);
  kioskSign.position.set(0, 4.35, 0);
  importKiosk.add(kioskSign);
  content.add(importKiosk);
  group.userData.repositoryImportKiosk = importKiosk;
  importKiosk.userData.importBeam = importBeam;
  importKiosk.userData.importSpark = importSpark;
  importKiosk.userData.importing = false;
  group.traverse((child) => {
    if (child.isMesh) {
      child.userData.landmark = "repositories";
      interactive.push(child);
    }
  });
  setShadows(group);
  animated.push((time) => {
    portal.rotation.y = portal.userData.repositorySizeLayer
      ? -0.12
      : Math.sin(time * 0.00018) * 0.09;
    globe.rotation.y = time * 0.00012;
    globe.rotation.x = Math.sin(time * 0.00009) * 0.12;
    if (importKiosk.userData.importing) {
      importBeam.visible = true;
      importSpark.visible = true;
      importBeam.rotation.y = time * 0.0018;
      importBeam.material.opacity = 0.11 + Math.sin(time * 0.006) * 0.045;
      importSpark.rotation.y = time * 0.003;
      importSpark.rotation.x = time * 0.0017;
      importSpark.position.y = 3.45 + Math.sin(time * 0.004) * 0.28;
    } else {
      importBeam.visible = false;
      importSpark.visible = false;
    }
    if (portal.userData.repositoryOrbitLayer) {
      portal.userData.repositoryOrbitLayer.rotation.z = time * 0.000012;
    }
    files.forEach((file, index) => {
      if (file.userData.layoutPosition) {
        const base = file.userData.layoutPosition;
        const phase = time * 0.00045 + index * 1.71;
        file.position.set(
          base.x + Math.sin(phase) * 0.035,
          base.y + Math.cos(phase * 0.83) * 0.045,
          base.z + Math.cos(phase) * 0.025,
        );
        file.rotation.z = Math.sin(phase) * 0.08;
        return;
      }
      const phase = time * 0.0002 * (1 + file.userData.layer * 0.3);
      const angle = file.userData.angle + phase;
      file.position.x = Math.cos(angle) * file.userData.radius;
      file.position.y = 3.1 + Math.sin(angle * 2 + index) * 0.42;
      file.rotation.z = Math.sin(phase * 2 + index) * 0.25;
    });
  });
  return group;
}
