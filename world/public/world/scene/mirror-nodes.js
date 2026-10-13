import { AVATAR_EMOJI_SKIN_COLOR } from "./avatar-shared.js";
import {
  canvasTexture,
  roundedRect,
  sharpenCanvasTextTexture,
} from "./canvas.js";
import { NODE_ACTIVE_WINDOW_MS } from "./layout.js";
import { makeSharedMaterial } from "./materials.js";
import { setShadows } from "./object-utils.js";
import * as resources from "./resources.js";

function mirrorNodeIsOnline(node) {
  const health = String(node?.health || node?.status || "").toLowerCase();
  return (
    node?.online === true ||
    node?.healthy === true ||
    ["online", "healthy", "available"].includes(health)
  );
}

// The most recent moment this node was observably doing something: being seen
// by the relay, publishing a signed catalog record, or serving a clone or a
// page. Two stamps are deliberately excluded. A commit date describes the
// repository the node mirrors, not the machine still being alive; and
// `checkedAt` is when the endpoint prober last *tried* the node, which it
// refreshes on failure too, so trusting it would make every dead mirror
// immortal.
function mirrorNodeActiveAt(node) {
  return [
    node?.lastSeen,
    node?.lastSync,
    node?.updatedAt,
    node?.activityUpdatedAt,
    node?.cloneServedAt,
    node?.websiteServedAt,
  ].reduce((latest, value) => {
    const stamp = Number(value);
    return Number.isFinite(stamp) && stamp > latest ? stamp : latest;
  }, 0);
}

// A node belongs in the yard while it is online, or while its last observed
// activity is inside the window. A node that reports no stamps at all and is
// not online has never proven itself active, so it stays out.
export function mirrorNodeIsRecentlyActive(node, now = Date.now()) {
  if (mirrorNodeIsOnline(node)) return true;
  const activeAt = mirrorNodeActiveAt(node);
  return activeAt > 0 && now - activeAt <= NODE_ACTIVE_WINDOW_MS;
}

function mirrorMetric(value, maximum = Number.MAX_SAFE_INTEGER) {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 && number <= maximum
    ? number
    : null;
}

// The two ways a node answers for the repository — a git clone and a served
// repository web request — counted together, so either kind of visit reads as
// one "this cabinet just served somebody" event. Null when the node reports
// neither counter; unreported values are never estimated.
export function mirrorServedTotal(node) {
  const clones = mirrorMetric(node?.clonesServed, 1_000_000_000);
  const website = mirrorMetric(node?.websiteServed, 1_000_000_000);
  if (clones === null && website === null) return null;
  return (clones || 0) + (website || 0);
}

function compactMirrorCount(value) {
  const number = mirrorMetric(value, 1_000_000_000);
  if (number === null) return "—";
  if (number >= 1_000_000) return `${(number / 1_000_000).toFixed(1)}M`;
  if (number >= 1_000) return `${(number / 1_000).toFixed(1)}K`;
  return String(Math.round(number));
}

function compactMirrorBytes(value) {
  const bytes = mirrorMetric(value, 2 ** 50);
  if (bytes === null) return "—";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let amount = bytes;
  let unit = 0;
  while (amount >= 1024 && unit < units.length - 1) {
    amount /= 1024;
    unit += 1;
  }
  return `${amount >= 10 || unit === 0 ? amount.toFixed(0) : amount.toFixed(1)} ${units[unit]}`;
}

function mirrorRatio(used, total) {
  const safeUsed = mirrorMetric(used, 2 ** 50);
  const safeTotal = mirrorMetric(total, 2 ** 50);
  if (safeUsed === null || safeTotal === null || safeTotal <= 0 || safeUsed > safeTotal) {
    return null;
  }
  return safeUsed / safeTotal;
}

export function nodeDataKey(node) {
  return JSON.stringify({
    name: node?.name,
    machineName: node?.machineName,
    ownerUser: node?.ownerUser,
    online: mirrorNodeIsOnline(node),
    healthy: node?.healthy,
    integrity: node?.integrity,
    activity: node?.activity,
    activityUpdatedAt: node?.activityUpdatedAt,
    endpoint: node?.endpoint,
    checkedAt: node?.checkedAt,
    latencyMs: node?.latencyMs,
    region: node?.region,
    endpointHealthy: node?.endpointHealthy,
    endpointIntegrity: node?.endpointIntegrity,
    endpointFresh: node?.endpointFresh,
    abuseBlocked: node?.abuseBlocked,
    operations: node?.operations,
    cloneAvailable: node?.cloneAvailable,
    commit: node?.commit,
    branch: node?.branch,
    lastCommitMessage: node?.lastCommitMessage,
    lastCommitAuthorName: node?.lastCommitAuthorName,
    lastCommitAt: node?.lastCommitAt,
    version: node?.version,
    platform: node?.platform,
    lastSync: node?.lastSync,
    updatedAt: node?.updatedAt,
    sizeBytes: node?.sizeBytes,
    issueCount: node?.issueCount,
    commitCount: node?.commitCount,
    branchCount: node?.branchCount,
    pullCount: node?.pullCount,
    discussionCount: node?.discussionCount,
    worktreeCount: node?.worktreeCount,
    artifactCount: node?.artifactCount,
    clonesServed: node?.clonesServed,
    websiteServed: node?.websiteServed,
    cloneServedAt: node?.cloneServedAt,
    cloneServedAgent: node?.cloneServedAgent,
    websiteServedAt: node?.websiteServedAt,
    websiteServedAgent: node?.websiteServedAgent,
    cpuPercent: node?.cpuPercent,
    memoryUsedBytes: node?.memoryUsedBytes,
    memoryTotalBytes: node?.memoryTotalBytes,
    diskUsedBytes: node?.diskUsedBytes,
    diskTotalBytes: node?.diskTotalBytes,
    repositoryCount: node?.repositoryCount,
    repositories: node?.repositories,
    agentTasks: node?.agentTasks,
    actionRunsAvailable: node?.actionRunsAvailable === true,
    actionRuns: node?.actionRuns,
  });
}

export function mirrorCommitAgeLabel(ageMs) {
  const age = Number(ageMs);
  if (!Number.isFinite(age) || age < 0) return "AGE NOT REPORTED";
  const minute = 60 * 1000;
  const hour = 60 * minute;
  const day = 24 * hour;
  const week = 7 * day;
  const month = 30 * day;
  const year = 365 * day;
  if (age < minute) return "just now";
  if (age < hour) return `${Math.floor(age / minute)}m ago`;
  if (age < day) return `${Math.floor(age / hour)}h ago`;
  if (age < week) return `${Math.floor(age / day)}d ago`;
  if (age < month) return `${Math.floor(age / week)}w ago`;
  if (age < year) return `${Math.floor(age / month)}mo ago`;
  return `${Math.floor(age / year)}y ago`;
}

// How the bounded client classes a node publishes for its last served clone /
// website read are spelled on the cabinet. Anything else is treated as
// unreported rather than printed raw.
const MIRROR_SERVE_AGENT_LABELS = Object.freeze({
  "forkmesh-node": "MESH NODE",
  "git-client": "GIT",
  "bot-tool": "BOT/TOOL",
  browser: "BROWSER",
  client: "CLIENT",
});

// "17h ago · GIT" for the two serve counters: when this node last answered
// that kind of request and who it answered. Null (no second line at all) when
// the node has served none yet or runs a build that never reported it — an
// unserved counter must not borrow the record's publish age.
function mirrorServeStamp(servedAt, agent, now = Date.now()) {
  const stamp = Number(servedAt);
  if (!Number.isFinite(stamp) || stamp <= 0 || stamp > now + 60 * 1000) {
    return null;
  }
  const label = MIRROR_SERVE_AGENT_LABELS[String(agent || "").toLowerCase()];
  const age = mirrorCommitAgeLabel(Math.max(0, now - stamp));
  return label ? `${age} · ${label}` : age;
}

function mirrorCommitSnapshot(node, repo) {
  const nested = node?.lastCommit || node?.commitDetails || repo?.lastCommit || {};
  const message = String(
    node?.lastCommitMessage ||
      node?.commitMessage ||
      node?.commitSubject ||
      repo?.lastCommitMessage ||
      repo?.commitMessage ||
      nested?.message ||
      nested?.subject ||
      "COMMIT SUBJECT NOT REPORTED",
  )
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 80);
  const author = String(
    node?.lastCommitAuthorName ||
      node?.commitAuthorName ||
      repo?.lastCommitAuthorName ||
      repo?.commitAuthorName ||
      nested?.authorName ||
      nested?.author?.name ||
      node?.commitAuthor ||
      "AUTHOR NOT REPORTED",
  )
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 42);
  const rawTimestamp = Number(
    node?.lastCommitAt ||
      node?.commitDate ||
      repo?.lastCommitAt ||
      repo?.commitDate ||
      nested?.committedAt ||
      nested?.date ||
      nested?.author?.date,
  );
  const commitAt = Number.isFinite(rawTimestamp)
    ? rawTimestamp < 100_000_000_000
      ? rawTimestamp * 1000
      : rawTimestamp
    : 0;
  const syncAge = mirrorMetric(node?.syncAgeMs ?? repo?.syncAgeMs, 2 ** 50);
  const ageMs = commitAt > 0
    ? Math.max(0, Date.now() - commitAt)
    : syncAge;
  const initials = author
    .split(/\s+/)
    .filter(Boolean)
    .slice(0, 2)
    .map((part) => part[0])
    .join("")
    .toUpperCase()
    .slice(0, 2) || "?";
  let colorHash = 0;
  for (const character of author) {
    colorHash = (colorHash * 31 + character.charCodeAt(0)) >>> 0;
  }
  const hue = colorHash % 360;
  return {
    message,
    author,
    initials,
    age: mirrorCommitAgeLabel(ageMs),
    avatarColor: `hsl(${hue} 42% 34%)`,
  };
}

function serverPanelTexture(THREE, node) {
  const online = mirrorNodeIsOnline(node);
  const integrity = String(node?.integrity || "unknown").toLowerCase();
  const repo = Array.isArray(node?.repositories) ? node.repositories[0] : null;
  const repositoryLabel =
    repo?.owner && repo?.name
      ? `${String(repo.owner).slice(0, 32)}/${String(repo.name).slice(0, 44)}`
      : "REPOSITORY NOT REPORTED";
  const commitSnapshot = mirrorCommitSnapshot(node, repo);
  const syncAgo = mirrorCommitAgeLabel(node?.syncAgeMs);
  const commit = String(node?.commit || repo?.commit || "").toLowerCase();
  const shortCommit = /^[0-9a-f]{40,64}$/.test(commit)
    ? commit.slice(0, 12)
    : "NOT REPORTED";
  const cpu = mirrorMetric(node?.cpuPercent, 100);
  const memory = mirrorRatio(node?.memoryUsedBytes, node?.memoryTotalBytes);
  const disk = mirrorRatio(node?.diskUsedBytes, node?.diskTotalBytes);
  const pingLatency = mirrorMetric(node?.latencyMs, 60_000);
  const statusColor =
    online && integrity === "ok"
      ? "#73f0ad"
      : integrity === "rejected" || integrity === "degraded"
        ? "#ff7e88"
        : online
          ? "#f7c96b"
          : "#91a39a";
  const routeLabel = !online
    ? "ROUTE OFFLINE"
    : node?.cloneAvailable === true && integrity === "ok"
      ? "CLONE READY"
      : integrity === "rejected" || integrity === "degraded"
        ? "ROUTE BLOCKED"
        : integrity === "healing"
          ? "ROUTE VERIFYING"
          : "ROUTE UNVERIFIED";
  const compactUsage = (ratio, used, total) =>
    ratio === null
      ? "NOT SHARED"
      : `${(ratio * 100).toFixed(1)}% · ${compactMirrorBytes(
          used,
        )}/${compactMirrorBytes(total)}`;
  const relativeStamp = (value) => {
    const stamp = Number(value);
    return Number.isFinite(stamp) && stamp > 0
      ? mirrorCommitAgeLabel(Math.max(0, Date.now() - stamp))
      : "NOT REPORTED";
  };
  const repositoryCount = mirrorMetric(node?.repositoryCount, 1_000_000);
  const healthLabel =
    online && node?.healthy === true
      ? "HEALTHY"
      : online
        ? `ATTENTION · ${integrity.toUpperCase()}`
        : "OFFLINE";
  const reachabilityLabel =
    node?.endpointHealthy === true && node?.endpointFresh === true
      ? "ONLINE · FRESH PROOF"
      : node?.endpointHealthy === true
        ? "RESPONDING · STALE PROOF"
        : online
          ? "RELAY ONLINE"
          : "UNREACHABLE";
  const tunnelLabel = node?.endpoint
    ? `${node?.endpointFresh === true ? "FRESH" : "STALE"} HTTPS${
        node?.region ? ` · ${String(node.region).toUpperCase()}` : ""
      }`
    : "NOT REPORTED";
  const fields = [
    ["OWNER", String(node?.ownerUser || "NOT REPORTED")],
    ["REPOS", compactMirrorCount(repositoryCount)],
    ["COMMIT", `${shortCommit}${node?.branch ? ` · ${node.branch}` : ""}`],
    ["MESSAGE", commitSnapshot.message],
    ["AUTHOR", commitSnapshot.author],
    ["SYNCED", syncAgo],
    ["SIZE", compactMirrorBytes(node?.sizeBytes)],
    ["ISSUES", compactMirrorCount(node?.issueCount)],
    ["COMMITS", compactMirrorCount(node?.commitCount)],
    ["BRANCHES", compactMirrorCount(node?.branchCount)],
    ["PULLS", compactMirrorCount(node?.pullCount)],
    ["DISCUSSIONS", compactMirrorCount(node?.discussionCount)],
    ["HEALTH", healthLabel],
    ["CPU", cpu === null ? "NOT SHARED" : `${cpu.toFixed(1)}%`],
    [
      "RAM",
      compactUsage(memory, node?.memoryUsedBytes, node?.memoryTotalBytes),
    ],
    ["DISK", compactUsage(disk, node?.diskUsedBytes, node?.diskTotalBytes)],
    ["PLATFORM", String(node?.platform || "NOT REPORTED")],
    [
      "VERSION",
      node?.version
        ? `v${String(node.version).replace(/^v/i, "")}`
        : "NOT REPORTED",
    ],
    ["NODE ID", String(node?.nodeId || "NOT REPORTED")],
    ["TUNNEL", tunnelLabel],
    [
      "CLONES",
      [
        compactMirrorCount(node?.clonesServed),
        mirrorServeStamp(node?.cloneServedAt, node?.cloneServedAgent),
      ].filter(Boolean).join(" · "),
    ],
    [
      "WEBSITE",
      [
        compactMirrorCount(node?.websiteServed),
        mirrorServeStamp(node?.websiteServedAt, node?.websiteServedAgent),
      ].filter(Boolean).join(" · "),
    ],
    ["ARTIFACTS", compactMirrorCount(node?.artifactCount)],
    ["REACHABILITY", reachabilityLabel],
    ["PRIMARY REPO", repositoryLabel],
    ["WORKTREES", compactMirrorCount(node?.worktreeCount)],
    ["ACTIVITY", String(node?.activity || "NOT REPORTED").toUpperCase()],
    ["LAST SEEN", relativeStamp(node?.lastSeen)],
    ["HOSTED SINCE", relativeStamp(node?.hostedSince)],
    ["REGION", String(node?.region || "NOT REPORTED").toUpperCase()],
    ["HTTPS ENDPOINT", String(node?.endpoint || "NOT REPORTED")],
    [
      "OPERATIONS",
      Array.isArray(node?.operations) && node.operations.length
        ? node.operations.join(" · ").toUpperCase()
        : "NOT REPORTED",
    ],
  ];

  // Match the panel's portrait aspect ratio and double the old vertical
  // backing resolution. Compact/mobile tiers may still reduce this through
  // canvasTexture(), but they now start from enough source pixels to preserve
  // letter stems instead of scaling a 512-square card twice.
  const texture = canvasTexture(THREE, 768, 1024, (context) => {
    context.fillStyle = "#07110f";
    context.fillRect(0, 0, 768, 1024);
    context.strokeStyle = "#526c61";
    context.lineWidth = 9;
    roundedRect(context, 9, 9, 750, 1006, 24);
    context.stroke();

    context.textAlign = "left";
    context.textBaseline = "middle";
    context.font = '800 44px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#f1fff6";
    context.fillText(
      String(node?.machineName || node?.name || "MIRROR")
        .toUpperCase()
        .slice(0, 24),
      32,
      50,
      704,
    );
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = statusColor;
    context.fillText(
      `${online ? "ONLINE" : "OFFLINE"} · ${integrity.toUpperCase()} · ${routeLabel}`,
      32,
      94,
      704,
    );
    context.font = '600 17px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#8ca99a";
    context.fillText(
      pingLatency === null
        ? "PING NOT REPORTED"
        : `PING ${Math.round(pingLatency)} MS · ${
            node?.endpointFresh === true ? "FRESH" : "STALE"
          }`,
      32,
      126,
      704,
    );

    context.strokeStyle = "#294339";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(32, 151);
    context.lineTo(736, 151);
    context.stroke();

    const fitted = (value, maxWidth) => {
      const raw = String(value || "—").replace(/\s+/g, " ").trim() || "—";
      if (context.measureText(raw).width <= maxWidth) return raw;
      let end = raw.length;
      while (end > 1 && context.measureText(`${raw.slice(0, end)}…`).width > maxWidth) {
        end -= 1;
      }
      return `${raw.slice(0, end)}…`;
    };
    const pairedFields = fields.slice(0, 30);
    pairedFields.forEach(([label, value], index) => {
      const column = index % 2;
      const row = Math.floor(index / 2);
      const x = column === 0 ? 32 : 394;
      const y = 169 + row * 51;
      context.fillStyle = row % 2 ? "#091713" : "#0d1e19";
      context.fillRect(x - 8, y - 10, 350, 47);
      context.font = '700 13px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#8ca99a";
      context.fillText(label, x, y);
      context.font = '700 18px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#f1fff6";
      context.fillText(fitted(value, 328), x, y + 21);
    });

    fields.slice(30).forEach(([label, value], index) => {
      const y = 934 + index * 33;
      context.font = '700 12px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#8ca99a";
      context.fillText(label, 32, y);
      context.font = '700 16px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#f1fff6";
      context.fillText(fitted(value, 548), 176, y);
    });

    context.fillStyle = "#91a39a";
    context.font = '600 12px "ForkMesh Mono", ui-monospace, monospace';
    context.fillText(
      "SIGNED OPERATOR DATA · CLICK FOR FULL VALUES AND EVERY REPOSITORY",
      32,
      1003,
    );
  });
  texture.userData.cabinetFieldLabels = [
    "NODE",
    ...fields.map(([label]) => label),
  ];
  return sharpenCanvasTextTexture(THREE, texture);
}

const MIRROR_AGENT_TASK_STATUS = Object.freeze({
  running: { label: "RUNNING", color: "#77d9ff" },
  stopped: { label: "STOPPED", color: "#91a39a" },
  merged: { label: "MERGED", color: "#73f0ad" },
  attention: { label: "ATTENTION", color: "#ffb56b" },
});

const MIRROR_ACTION_STATUS = Object.freeze({
  "awaiting-approval": { label: "APPROVAL", color: "#ffcc72" },
  queued: { label: "QUEUED", color: "#77d9ff" },
  running: { label: "RUNNING", color: "#77d9ff" },
  success: { label: "DONE", color: "#73f0ad" },
  failed: { label: "ERROR", color: "#ff7189" },
  rejected: { label: "REJECTED", color: "#ff7189" },
  cancelled: { label: "CANCELLED", color: "#91a39a" },
  skipped: { label: "SKIPPED", color: "#91a39a" },
});

function mirrorActionsPanelTexture(THREE, node) {
  const available = node?.actionRunsAvailable === true;
  const runs = (Array.isArray(node?.actionRuns) ? node.actionRuns : [])
    .slice(0, 6);
  const texture = canvasTexture(THREE, 768, 1024, (context, canvas) => {
    context.fillStyle = "#07110f";
    context.fillRect(0, 0, canvas.width, canvas.height);
    context.strokeStyle = "#526c61";
    context.lineWidth = 9;
    roundedRect(context, 9, 9, canvas.width - 18, canvas.height - 18, 24);
    context.stroke();
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.font = '800 42px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#f1fff6";
    context.fillText("ACTIONS RUNS", 36, 54);
    context.font = '700 20px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#73f0ad";
    context.fillText("SIGNED · REDACTED LOG TAILS · CLICK FOR ALL", 36, 94);
    context.strokeStyle = "#294339";
    context.lineWidth = 3;
    context.beginPath();
    context.moveTo(36, 125);
    context.lineTo(732, 125);
    context.stroke();

    if (!available || !runs.length) {
      context.textAlign = "center";
      context.font = '800 30px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = available ? "#91a39a" : "#ffcc72";
      context.fillText(
        available ? "NO RECENT ACTIONS RUNS" : "ACTIONS SUMMARY UNAVAILABLE",
        canvas.width / 2,
        260,
      );
      context.font = '600 21px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#8ca99a";
      context.fillText(
        available
          ? "The protected feed is ready."
          : "Enable the signed actions-status capability.",
        canvas.width / 2,
        310,
      );
      return;
    }

    runs.forEach((run, index) => {
      const status =
        MIRROR_ACTION_STATUS[String(run?.status || "")] ||
        MIRROR_ACTION_STATUS.cancelled;
      const y = 146 + index * 137;
      context.fillStyle = index % 2 ? "#0b1915" : "#0d1e19";
      roundedRect(context, 24, y, 720, 121, 10);
      context.fill();
      context.fillStyle = status.color;
      context.fillRect(24, y, 8, 121);
      context.font = '800 22px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#f1fff6";
      context.fillText(
        `#${Number(run?.id) || 0} ${String(run?.workflow || "Action")
          .replace(/[\u0000-\u001f\u007f]/g, " ")
          .slice(0, 38)}`,
        48,
        y + 27,
      );
      context.textAlign = "right";
      context.fillStyle = status.color;
      context.fillText(status.label, 720, y + 27);
      context.textAlign = "left";
      const logLine = String(run?.logTail || "No log tail reported")
        .replace(/[\r\n\t]+/g, " ")
        .replace(/\s+/g, " ")
        .trim()
        .slice(-76);
      context.font = '600 19px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#a9c0b4";
      context.fillText(logLine, 48, y + 69, 664);
      context.fillStyle = "#6f8579";
      context.font = '600 17px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(
        `${String(run?.ref || "ref unavailable").slice(0, 46)} · ${String(
          run?.commit || "",
        ).slice(0, 12)}`,
        48,
        y + 99,
      );
    });
  });
  return sharpenCanvasTextTexture(THREE, texture);
}

export function normalizeMirrorAgentTask(task) {
  if (!task || typeof task !== "object") return null;
  const title = String(task.title || task.task || "")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, 84);
  if (!title) return null;
  const rawStatus = String(task.status || "").toLowerCase();
  const history = Array.isArray(task.history) ? task.history : [];
  const merged =
    rawStatus === "merged" ||
    history.some(
      (entry) =>
        String(entry?.state || "").toLowerCase() === "merged" ||
        /\bmerged\b/i.test(String(entry?.text || "").slice(0, 240)),
    );
  let status = "stopped";
  if (merged) {
    status = "merged";
  } else if (
    ["running", "queued", "security_pending", "starting"].includes(rawStatus)
  ) {
    status = "running";
  } else if (
    ["failed", "rejected", "attention", "blocked", "error"].includes(rawStatus)
  ) {
    status = "attention";
  }
  return {
    id: String(task.id || "").slice(0, 64),
    title,
    provider:
      String(task.provider || "").toLowerCase() === "codex"
        ? "CODEX"
        : "CLAUDE",
    status,
    displayStatus: rawStatus || status,
    targetNode: String(task.targetNode || task.node || "")
      .trim()
      .slice(0, 64),
    repositoryKey: `${
      String(task.org || task.owner || "forkmesh").trim().toLowerCase()
    }/${String(task.repo || task.repository || "").trim().toLowerCase()}`,
    updatedAt: Math.max(0, Number(task.updatedAt) || 0),
  };
}

function mirrorAgentTaskPanelTexture(THREE, node, provider = "") {
  const providerLabel =
    String(provider || "").toLowerCase() === "codex" ? "CODEX" : "CLAUDE";
  const tasks = (Array.isArray(node?.agentTasks) ? node.agentTasks : [])
    .map(normalizeMirrorAgentTask)
    .filter(Boolean)
    .filter((task) => task.provider === providerLabel)
    .sort((left, right) => right.updatedAt - left.updatedAt)
    .slice(0, 5);
  const texture = canvasTexture(THREE, 512, 1024, (context, canvas) => {
    context.fillStyle = "#07110f";
    context.fillRect(0, 0, canvas.width, canvas.height);
    context.strokeStyle = "#526c61";
    context.lineWidth = 8;
    roundedRect(context, 8, 8, canvas.width - 16, canvas.height - 16, 22);
    context.stroke();
    context.textAlign = "left";
    context.textBaseline = "middle";
    context.font = '800 36px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#f1fff6";
    context.fillText(`${providerLabel} TASKS`, 28, 48);
    context.font = '700 17px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#73f0ad";
    context.fillText("ENGINEERING TEAM VIEW · CLICK TO OPEN", 28, 82);
    context.strokeStyle = "#294339";
    context.lineWidth = 2;
    context.beginPath();
    context.moveTo(28, 108);
    context.lineTo(484, 108);
    context.stroke();

    if (!tasks.length) {
      context.font = '700 25px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#91a39a";
      context.textAlign = "center";
      context.fillText(`NO ${providerLabel} TASKS`, canvas.width / 2, 230);
      context.font = '600 18px "ForkMesh Mono", ui-monospace, monospace';
      context.fillText(`Start ${providerLabel} from Agents`, canvas.width / 2, 270);
      return;
    }

    tasks.forEach((task, index) => {
      const y = 128 + index * 168;
      const status = MIRROR_AGENT_TASK_STATUS[task.status];
      context.fillStyle = index % 2 ? "#0b1915" : "#0d1e19";
      roundedRect(context, 20, y, 472, 148, 12);
      context.fill();
      context.fillStyle = status.color;
      context.fillRect(20, y, 8, 148);
      context.font = '800 20px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#91a39a";
      context.fillText(task.provider, 44, y + 28);
      const badgeWidth = Math.max(108, context.measureText(status.label).width + 24);
      context.fillStyle = `${status.color}26`;
      roundedRect(context, 472 - badgeWidth, y + 12, badgeWidth, 32, 8);
      context.fill();
      context.strokeStyle = status.color;
      context.lineWidth = 2;
      context.stroke();
      context.fillStyle = status.color;
      context.textAlign = "center";
      context.fillText(status.label, 472 - badgeWidth / 2, y + 29);
      context.textAlign = "left";
      context.font = '700 24px "ForkMesh Mono", ui-monospace, monospace';
      context.fillStyle = "#f1fff6";
      const words = task.title.split(/\s+/);
      const lines = ["", ""];
      for (const word of words) {
        const target = !lines[0] || context.measureText(
          `${lines[0]} ${word}`.trim(),
        ).width <= 418 ? 0 : 1;
        if (target === 1 && context.measureText(
          `${lines[1]} ${word}`.trim(),
        ).width > 418) {
          lines[1] = `${lines[1]}…`;
          break;
        }
        lines[target] = `${lines[target]} ${word}`.trim();
      }
      context.fillText(lines[0], 44, y + 76);
      if (lines[1]) context.fillText(lines[1], 44, y + 110);
    });

    context.font = '600 16px "ForkMesh Mono", ui-monospace, monospace';
    context.fillStyle = "#6f8579";
    context.fillText("LIVE FROM THE AUTHORIZED AGENT SESSION FEED", 28, 990);
  });
  return sharpenCanvasTextTexture(THREE, texture);
}

// A pocket-sized stand-in for the agent a node just answered: the little
// figure that shoots up out of a cabinet when it serves a clone or a
// repository page. Unlit and self-owning — every launch builds its own
// materials so it can fade out and dispose without touching shared palettes.
export function createServedVisitorFigure(THREE, accent = "#9ef7c6") {
  const group = new THREE.Group();
  group.name = "served-visitor";
  const skin = resources.uniqueMaterial(THREE, {
    color: AVATAR_EMOJI_SKIN_COLOR,
    transparent: true,
    toneMapped: false,
    depthWrite: false,
  }, "MeshBasicMaterial");
  const suit = resources.uniqueMaterial(THREE, {
    color: accent,
    transparent: true,
    toneMapped: false,
    depthWrite: false,
  }, "MeshBasicMaterial");
  const head = new THREE.Mesh(resources.primitive(THREE, "SphereGeometry", 0.19, 12, 10), skin);
  head.position.y = 1.16;
  group.add(head);
  const torso = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 0.36, 0.5, 0.22), suit);
  torso.position.y = 0.7;
  group.add(torso);
  // Arms swept overhead and legs trailing straight below: the silhouette still
  // reads as a launched visitor at the size it shrinks to high in the sky.
  for (const side of [-1, 1]) {
    const arm = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 0.1, 0.44, 0.1), suit);
    arm.position.set(side * 0.25, 0.86, 0);
    arm.rotation.z = side * 0.5;
    group.add(arm);
    const leg = new THREE.Mesh(resources.primitive(THREE, "BoxGeometry", 0.11, 0.44, 0.11), suit);
    leg.position.set(side * 0.1, 0.22, 0);
    group.add(leg);
  }
  group.userData.figureMaterials = [skin, suit];
  return group;
}

export function createMirrorServerCabinet(THREE, node, id) {
  const group = new THREE.Group();
  group.name = `mirror-server-cabinet:${id}`;
  group.userData.infrastructureKind = "mirror-node";
  group.userData.nodeId = id;
  const online = mirrorNodeIsOnline(node);
  const body = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 2.24, 3.28, 1.42),
    makeSharedMaterial(THREE, online ? "#060707" : "#0e0f10", {
      metalness: 0.28,
      roughness: 0.5,
    }),
  );
  body.position.y = 1.64;
  group.add(body);
  const repeatedParts = [body];

  const trimMaterial = makeSharedMaterial(THREE, "#1c1f21", {
    metalness: 0.5,
    roughness: 0.36,
  });
  for (const x of [-1.04, 1.04]) {
    const rail = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 1, 1, 1),
      trimMaterial,
    );
    rail.scale.set(0.075, 3.14, 1.5);
    rail.position.set(x, 1.64, 0);
    group.add(rail);
    repeatedParts.push(rail);
  }
  for (const x of [-0.84, 0.84]) {
    const foot = new THREE.Mesh(
      resources.primitive(THREE, "BoxGeometry", 1, 1, 1),
      trimMaterial,
    );
    foot.scale.set(0.28, 0.18, 0.8);
    foot.position.set(x, 0.08, 0);
    group.add(foot);
    repeatedParts.push(foot);
  }

  const panelGeometry = resources.primitive(THREE, "PlaneGeometry", 1.9, 2.46);
  const panelMaterial = resources.uniqueMaterial(THREE, {
    map: serverPanelTexture(THREE, node),
    toneMapped: false,
  }, "MeshBasicMaterial");
  const panel = new THREE.Mesh(panelGeometry, panelMaterial);
  // Keep a real depth gap in front of the 0.71 cabinet face. A near-coplanar
  // display flickers at oblique camera angles on mobile GPUs.
  panel.position.set(0, 1.72, -0.735);
  panel.rotation.y = Math.PI;
  panel.name = "mirror-server-rear-panel";
  panel.userData.nodeCabinet = { ...node };
  panel.userData.cabinetFieldLabels = [
    ...(panelMaterial.map.userData?.cabinetFieldLabels || []),
  ];
  group.add(panel);
  // Actions now face the routing-station walkway; technical server detail is
  // on the opposite face so each cabinet has a distinct operational side.
  const actionsPanelMaterial = resources.uniqueMaterial(THREE, {
    map: mirrorActionsPanelTexture(THREE, node),
    toneMapped: false,
  }, "MeshBasicMaterial");
  const rearPanel = new THREE.Mesh(panelGeometry, actionsPanelMaterial);
  rearPanel.position.set(0, 1.72, 0.735);
  rearPanel.name = "mirror-server-front-panel";
  rearPanel.userData.nodeCabinet = { ...node };
  group.add(rearPanel);
  const actionStatuses = (Array.isArray(node?.actionRuns)
    ? node.actionRuns
    : []).map((run) => String(run?.status || ""));
  const actionAttention = actionStatuses.some((status) =>
    ["failed", "rejected"].includes(status),
  );
  const actionRunning = actionStatuses.some((status) =>
    ["awaiting-approval", "queued", "running"].includes(status),
  );
  if (actionAttention || actionRunning) {
    const actionPulseMaterial = resources.uniqueMaterial(THREE, {
      color: actionAttention ? "#ff7189" : "#77d9ff",
      toneMapped: false,
      transparent: true,
      opacity: 0.78,
    }, "MeshBasicMaterial");
    for (const x of [-1.01, 1.01]) {
      const pulse = new THREE.Mesh(
        resources.primitive(THREE, "BoxGeometry", 0.055, 2.5, 0.04),
        actionPulseMaterial,
      );
      pulse.position.set(x, 1.72, 0.758);
      group.add(pulse);
    }
    group.userData.actionPulseMaterial = actionPulseMaterial;
    group.userData.actionPulseAttention = actionAttention;
  }

  for (const side of [-1, 1]) {
    const provider = side < 0 ? "claude-code" : "codex";
    const agentTaskPanel = new THREE.Mesh(
      resources.primitive(THREE, "PlaneGeometry", 1.27, 2.72),
      resources.uniqueMaterial(THREE, {
        map: mirrorAgentTaskPanelTexture(THREE, node, provider),
        toneMapped: false,
      }, "MeshBasicMaterial"),
    );
    agentTaskPanel.position.set(side * 1.125, 1.72, 0);
    agentTaskPanel.rotation.y = side * Math.PI / 2;
    agentTaskPanel.name =
      side > 0
        ? "mirror-server-agent-panel-right"
        : "mirror-server-agent-panel-left";
    agentTaskPanel.userData.nodeCabinet = { ...node };
    group.add(agentTaskPanel);
  }

  const integrity = String(node?.integrity || "unknown").toLowerCase();
  const activity = String(node?.activity || "unknown").toLowerCase();
  // The roof beacon answers one simple physical question: is this node alive?
  // Clone eligibility and refs integrity remain explicit on its front/detail
  // displays. Mixing those two states made a reachable worker look dead or
  // yellow whenever it intentionally exposed no public clone endpoint.
  const statusColor =
    online
      ? "#00cc44"
      : integrity === "rejected" || integrity === "degraded"
        ? "#ff0000"
        : integrity === "healing" || activity === "syncing" ||
            activity === "awaiting-verification"
          ? "#ffcc00"
          : "#71837a";
  // Yellow and red are the two statuses that want attention, so their lamps
  // sweep like a rotating warning beacon; green and offline stay steady.
  const alerting = statusColor === "#ff0000" || statusColor === "#ffcc00";
  // A single beacon lamp sits on the cabinet roof; its color is the status.
  // The lens is an unlit cylinder so the status reads as one flat, solid
  // colour from every camera angle instead of shading into a gradient. The
  // top is left open — the world camera looks down on the yard, so a metal
  // cap would hide the one part of the lamp that carries the status.
  const statusLight = new THREE.Group();
  const beaconBase = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.17, 0.19, 0.08, 12),
    makeSharedMaterial(THREE, "#35463e", {
      metalness: 0.82,
      roughness: 0.3,
    }),
  );
  beaconBase.position.y = 0.04;
  statusLight.add(beaconBase);
  repeatedParts.push(beaconBase);
  const beaconLens = new THREE.Mesh(
    resources.primitive(THREE, "CylinderGeometry", 0.15, 0.15, 0.3, 12),
    resources.material(THREE, { color: statusColor, toneMapped: false }, "MeshBasicMaterial"),
  );
  beaconLens.position.y = 0.23;
  statusLight.add(beaconLens);
  repeatedParts.push(beaconLens);
  // Only a thin collar rings the open mouth so the lens still reads as a
  // fixture rather than a bare peg.
  const beaconCollar = new THREE.Mesh(
    resources.primitive(THREE, "TorusGeometry", 0.152, 0.016, 4, 16),
    makeSharedMaterial(THREE, "#35463e", {
      metalness: 0.82,
      roughness: 0.3,
    }),
  );
  beaconCollar.rotation.x = Math.PI / 2;
  beaconCollar.position.y = 0.378;
  statusLight.add(beaconCollar);
  repeatedParts.push(beaconCollar);
  if (alerting) {
    // Two opposed additive lobes hugging the lens: rotating the group reads as
    // a sweeping light without an actual light source or a per-frame material
    // rebuild, both of which are too expensive for a yard of 64 cabinets.
    const beaconSweep = new THREE.Group();
    const sweepMaterial = resources.material(THREE, {
      color: statusColor,
      toneMapped: false,
      transparent: true,
      opacity: 0.55,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
      side: THREE.DoubleSide,
      forceSinglePass: true,
    }, "MeshBasicMaterial");
    for (const thetaStart of [0, Math.PI]) {
      const lobe = new THREE.Mesh(
        resources.primitive(
          THREE,
          "CylinderGeometry",
          0.24,
          0.24,
          0.26,
          10,
          1,
          true,
          thetaStart,
          Math.PI / 3,
        ),
        sweepMaterial,
      );
      beaconSweep.add(lobe);
    }
    beaconSweep.position.y = 0.23;
    statusLight.add(beaconSweep);
    group.userData.beaconSweep = beaconSweep;
  }
  statusLight.position.set(0, 3.29, 0);
  group.add(statusLight);

  const vent = new THREE.Mesh(
    resources.primitive(THREE, "BoxGeometry", 1, 1, 1),
    makeSharedMaterial(THREE, "#35463e", {
      metalness: 0.76,
      roughness: 0.38,
    }),
  );
  vent.scale.set(1.42, 0.26, 0.05);
  vent.position.set(0, 0.22, 0.75);
  group.add(vent);
  repeatedParts.push(vent);
  group.userData.repeatedParts = repeatedParts;
  group.userData.signalRing = statusLight;
  group.userData.online = online;
  group.userData.dataKey = nodeDataKey(node);
  group.userData.nodeRecord = { ...node };
  setShadows(group);
  // The sweep is a glow, not geometry: shadow-casting it would paint a turning
  // dark band across the cabinet roof.
  if (group.userData.beaconSweep) {
    setShadows(group.userData.beaconSweep, false, false);
  }
  return group;
}

const MIRROR_SERVICE_FAMILIES = Object.freeze([
  Object.freeze({
    id: "git",
    operations: Object.freeze(["git-info-refs", "git-upload-pack", "git-receive-pack"]),
  }),
  Object.freeze({
    id: "browse",
    operations: Object.freeze([
      "tree",
      "blob",
      "blobs",
      "raw",
      "history",
      "commit",
      "compare",
      "branches",
      "search",
      "stats",
      "sizes",
      "release-blob",
    ]),
  }),
  Object.freeze({
    id: "merge",
    operations: Object.freeze(["merge-pull", "merge-preview", "update-pull-branch"]),
  }),
  Object.freeze({
    id: "actions",
    operations: Object.freeze(["actions-status", "actions-control"]),
  }),
]);
export const MIRROR_NODE_GEAR_LIMIT = 6;
const MIRROR_GEAR_CAPACITY = 64 * MIRROR_NODE_GEAR_LIMIT;
const MIRROR_GEAR_RADIUS = 0.17;
const MIRROR_GEAR_SPACING = 0.33;
const MIRROR_GEAR_Y = 3.3 + MIRROR_GEAR_RADIUS;
const MIRROR_GEAR_Z = 0.5;
const MIRROR_GEAR_TEETH = 6;
const MIRROR_GEAR_SPIN = 0.0024;
const MIRROR_GEAR_HEALTHY = "#e0b44c";
const MIRROR_GEAR_STOPPED = "#c8423a";
const MIRROR_SPARK_POOL = 12;
const MIRROR_SPARK_SEGMENTS = 5;
const MIRROR_SPARK_LIFE_MS = 450;
const MIRROR_SPARK_PENDING_LIMIT = 24;

export function mirrorNodeServices(node) {
  const online = mirrorNodeIsOnline(node);
  const integrity = String(node?.integrity || "").toLowerCase();
  const serving =
    online &&
    node?.abuseBlocked !== true &&
    (!node?.endpoint || node?.endpointHealthy === true);
  const operations = new Set(
    (Array.isArray(node?.operations) ? node.operations : []).map((value) =>
      String(value || "").toLowerCase(),
    ),
  );
  const services = [
    {
      id: "sync",
      healthy:
        online &&
        integrity !== "rejected" &&
        integrity !== "degraded" &&
        node?.behind !== true,
    },
  ];
  MIRROR_SERVICE_FAMILIES.forEach((family) => {
    if (family.operations.some((operation) => operations.has(operation))) {
      services.push({ id: family.id, healthy: serving });
    }
  });
  if (
    node?.actionRunsAvailable === true &&
    !services.some((service) => service.id === "actions")
  ) {
    services.push({ id: "actions", healthy: online });
  }
  if (Array.isArray(node?.agentTasks) && node.agentTasks.length) {
    services.push({ id: "agents", healthy: online });
  }
  return services.slice(0, MIRROR_NODE_GEAR_LIMIT);
}

function mirrorGearGeometry(THREE) {
  return resources.geometry(THREE, "mirror-node-gear:v1", () => {
    const segments = MIRROR_GEAR_TEETH * 4;
    const gear = resources
      .primitive(THREE, "CylinderGeometry", 1, 1, 0.36, segments, 1)
      .clone();
    const position = gear.attributes.position;
    const step = (Math.PI * 2) / segments;
    for (let index = 0; index < position.count; index += 1) {
      const x = position.getX(index);
      const z = position.getZ(index);
      if (Math.hypot(x, z) < 0.5) continue;
      const slot =
        ((Math.round(Math.atan2(x, z) / step) % segments) + segments) %
        segments;
      if (Math.floor(slot / 2) % 2 === 0) continue;
      position.setX(index, x * 0.72);
      position.setZ(index, z * 0.72);
    }
    gear.rotateX(Math.PI / 2);
    gear.computeVertexNormals();
    return gear;
  });
}

export function createMirrorNodeMachines(THREE, parent) {
  const gears = new THREE.InstancedMesh(
    mirrorGearGeometry(THREE),
    resources.material(THREE, {
      color: "#ffffff",
      metalness: 0.55,
      roughness: 0.4,
    }),
    MIRROR_GEAR_CAPACITY,
  );
  gears.name = "mirror-node-gears";
  gears.count = 0;
  gears.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  gears.castShadow = false;
  gears.receiveShadow = false;
  const sparks = new THREE.InstancedMesh(
    resources.primitive(THREE, "BoxGeometry", 1, 1, 1),
    resources.uniqueMaterial(THREE, {
      color: "#c4f1ff",
      transparent: true,
      opacity: 0.95,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
      toneMapped: false,
    }, "MeshBasicMaterial"),
    MIRROR_SPARK_POOL * MIRROR_SPARK_SEGMENTS,
  );
  sparks.name = "mirror-node-sparks";
  sparks.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
  sparks.frustumCulled = false;
  sparks.castShadow = false;
  sparks.receiveShadow = false;
  sparks.visible = false;
  parent.add(gears, sparks);

  const hidden = new THREE.Matrix4().makeScale(0, 0, 0);
  for (let index = 0; index < sparks.count; index += 1) {
    sparks.setMatrixAt(index, hidden);
  }
  const up = new THREE.Vector3(0, 1, 0);
  const axisY = new THREE.Vector3(0, 1, 0);
  const axisZ = new THREE.Vector3(0, 0, 1);
  const matrix = new THREE.Matrix4();
  const place = new THREE.Vector3();
  const turn = new THREE.Quaternion();
  const spin = new THREE.Quaternion();
  const size = new THREE.Vector3();
  const from = new THREE.Vector3();
  const to = new THREE.Vector3();
  const bend = new THREE.Vector3();
  const start = new THREE.Vector3();
  const end = new THREE.Vector3();
  const direction = new THREE.Vector3();
  const tint = new THREE.Color();

  const gearCabinet = new Array(MIRROR_GEAR_CAPACITY).fill(null);
  const gearLocal = new Float32Array(MIRROR_GEAR_CAPACITY * 3);
  const gearTurn = new Float32Array(MIRROR_GEAR_CAPACITY);
  const gearPhase = new Float32Array(MIRROR_GEAR_CAPACITY);
  let gearCount = 0;
  let spinning = 0;

  const sparkCabinet = new Array(MIRROR_SPARK_POOL).fill(null);
  const sparkStart = new Float64Array(MIRROR_SPARK_POOL);
  const sparkEnds = new Float32Array(MIRROR_SPARK_POOL * 6);
  const queued = new Set();
  let activeSparks = 0;

  function toParent(cabinet, x, y, z, target) {
    const angle = cabinet.rotation.y;
    const cos = Math.cos(angle);
    const sin = Math.sin(angle);
    return target.set(
      cabinet.position.x + x * cos + z * sin,
      cabinet.position.y + y,
      cabinet.position.z - x * sin + z * cos,
    );
  }

  function writeGear(index, time) {
    const cabinet = gearCabinet[index];
    if (!cabinet || !cabinet.parent || !cabinet.visible) {
      gears.setMatrixAt(index, hidden);
      return;
    }
    const offset = index * 3;
    toParent(
      cabinet,
      gearLocal[offset],
      gearLocal[offset + 1],
      gearLocal[offset + 2],
      place,
    );
    turn.setFromAxisAngle(axisY, cabinet.rotation.y);
    spin.setFromAxisAngle(axisZ, gearPhase[index] + gearTurn[index] * time);
    turn.multiply(spin);
    size.setScalar(MIRROR_GEAR_RADIUS);
    matrix.compose(place, turn, size);
    gears.setMatrixAt(index, matrix);
  }

  function layout(cabinets) {
    gearCount = 0;
    spinning = 0;
    cabinets.forEach((cabinet) => {
      const services = mirrorNodeServices(cabinet.userData?.nodeRecord);
      const first = (services.length - 1) / 2;
      services.forEach((service, slot) => {
        if (gearCount >= MIRROR_GEAR_CAPACITY) return;
        const index = gearCount;
        gearCount += 1;
        gearCabinet[index] = cabinet;
        gearLocal[index * 3] = (slot - first) * MIRROR_GEAR_SPACING;
        gearLocal[index * 3 + 1] = MIRROR_GEAR_Y;
        gearLocal[index * 3 + 2] = MIRROR_GEAR_Z;
        const way = slot % 2 ? -1 : 1;
        gearTurn[index] = service.healthy ? way * MIRROR_GEAR_SPIN : 0;
        gearPhase[index] = slot % 2 ? Math.PI / MIRROR_GEAR_TEETH : 0;
        if (service.healthy) spinning += 1;
        gears.setColorAt(
          index,
          tint.set(service.healthy ? MIRROR_GEAR_HEALTHY : MIRROR_GEAR_STOPPED),
        );
        writeGear(index, 0);
      });
    });
    gearCabinet.fill(null, gearCount);
    gears.count = gearCount;
    gears.instanceMatrix.needsUpdate = true;
    if (gears.instanceColor) gears.instanceColor.needsUpdate = true;
    gears.computeBoundingSphere();
    gears.userData.spinning = spinning;
  }

  function queueSparks(cabinet, count, windowMs, now = performance.now()) {
    if (!cabinet) return;
    const wanted = Math.min(
      MIRROR_SPARK_PENDING_LIMIT,
      Math.max(0, Math.floor(Number(count) || 0)),
    );
    if (!wanted) return;
    const data = cabinet.userData;
    const pending = Math.min(
      MIRROR_SPARK_PENDING_LIMIT,
      (data.sparkPending || 0) + wanted,
    );
    data.sparkPending = pending;
    data.sparkSpacing = Math.min(2500, Math.max(160, windowMs / pending));
    if (!queued.has(cabinet)) {
      data.sparkNextAt = now;
      queued.add(cabinet);
    }
  }

  function fireSpark(cabinet, time) {
    let slot = -1;
    for (let index = 0; index < MIRROR_SPARK_POOL; index += 1) {
      if (!sparkCabinet[index]) {
        slot = index;
        break;
      }
    }
    if (slot < 0) return false;
    sparkCabinet[slot] = cabinet;
    sparkStart[slot] = time;
    const side = Math.random() < 0.5 ? -1 : 1;
    toParent(
      cabinet,
      (Math.random() - 0.5) * 1.6,
      MIRROR_GEAR_Y - 0.12,
      MIRROR_GEAR_Z + 0.24,
      from,
    );
    toParent(cabinet, side * 1.22, 1.6 + Math.random() * 1.4, 0.86, to);
    from.toArray(sparkEnds, slot * 6);
    to.toArray(sparkEnds, slot * 6 + 3);
    activeSparks += 1;
    return true;
  }

  function hideSpark(slot) {
    sparkCabinet[slot] = null;
    activeSparks -= 1;
    for (let segment = 0; segment < MIRROR_SPARK_SEGMENTS; segment += 1) {
      sparks.setMatrixAt(slot * MIRROR_SPARK_SEGMENTS + segment, hidden);
    }
  }

  function drawSpark(slot, age) {
    from.fromArray(sparkEnds, slot * 6);
    to.fromArray(sparkEnds, slot * 6 + 3);
    const fade = 1 - age / MIRROR_SPARK_LIFE_MS;
    start.copy(from);
    for (let segment = 0; segment < MIRROR_SPARK_SEGMENTS; segment += 1) {
      const t = (segment + 1) / MIRROR_SPARK_SEGMENTS;
      end.lerpVectors(from, to, t);
      if (segment < MIRROR_SPARK_SEGMENTS - 1) {
        bend.set(
          Math.random() - 0.5,
          Math.random() - 0.5,
          Math.random() - 0.5,
        );
        end.addScaledVector(bend, 0.34 * Math.sin(Math.PI * t));
      }
      direction.subVectors(end, start);
      const length = direction.length();
      place.addVectors(start, end).multiplyScalar(0.5);
      turn.setFromUnitVectors(up, direction.multiplyScalar(1 / (length || 1)));
      const width = 0.03 + 0.04 * fade;
      size.set(width, length, width);
      matrix.compose(place, turn, size);
      sparks.setMatrixAt(slot * MIRROR_SPARK_SEGMENTS + segment, matrix);
      start.copy(end);
    }
  }

  function update(time, { animate = true, sparksEnabled = true } = {}) {
    if (animate && spinning > 0 && gears.parent) {
      for (let index = 0; index < gearCount; index += 1) {
        writeGear(index, time);
      }
      gears.instanceMatrix.needsUpdate = true;
    }
    if (queued.size) {
      queued.forEach((cabinet) => {
        const data = cabinet.userData;
        if (!cabinet.parent || !sparksEnabled || !(data.sparkPending > 0)) {
          data.sparkPending = 0;
          queued.delete(cabinet);
          return;
        }
        if (time < data.sparkNextAt) return;
        if (fireSpark(cabinet, time)) data.sparkPending -= 1;
        data.sparkNextAt =
          time + data.sparkSpacing * (0.6 + Math.random() * 0.8);
        if (data.sparkPending <= 0) queued.delete(cabinet);
      });
    }
    if (!activeSparks) {
      sparks.visible = false;
      return;
    }
    for (let slot = 0; slot < MIRROR_SPARK_POOL; slot += 1) {
      if (!sparkCabinet[slot]) continue;
      const age = time - sparkStart[slot];
      if (age >= MIRROR_SPARK_LIFE_MS || !sparkCabinet[slot].parent) {
        hideSpark(slot);
      } else {
        drawSpark(slot, age);
      }
    }
    sparks.visible = activeSparks > 0;
    sparks.instanceMatrix.needsUpdate = true;
  }

  return {
    gears,
    sparks,
    layout,
    queueSparks,
    update,
    stats: () => ({
      gears: gearCount,
      spinning,
      sparks: activeSparks,
      queued: queued.size,
    }),
  };
}
