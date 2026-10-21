import {
  createWorldBackoff,
  markWorldHTTPFailure,
} from "./world-backoff.js";

const OFFICE_TASKS_PATH = "/api/tasks";
const MARKETING_TASKS_PATH = "/api/world/office/marketing-tasks";
// Mutations refresh immediately and opening the board refreshes after five
// seconds, so a 60-second foreground poll keeps the wall current without
// making three D1-backed requests a minute per visitor. If an assignee leaves
// the Office with a timer running, only a five-minute reconciliation poll is
// needed; elapsed time continues locally from the server clock.
// The whole organization catalog the relay is willing to return (MAX_TASKS in
// world_office_tasks.py). Rows are rendered a page at a time, TASK_PAGE_SIZE
// per page, so the board is not capped at its first page.
// The board's reads go through the World root's fetchJSON, which already backs
// off. This gate covers the one raw keepalive POST below, which cannot.
const departureBackoff = createWorldBackoff();
const MAX_TASKS = 2000;
const TASK_PAGE_SIZE = 100;
const OFFICE_TASKS_POLL_MS = 60_000;
const OFFICE_TASKS_BACKGROUND_POLL_MS = 5 * 60_000;
const OFFICE_TASKS_TICK_MS = 1_000;
const OFFICE_CHECKIN_MIN_MS = 4 * 60 * 1_000;
const OFFICE_CHECKIN_MAX_MS = 9 * 60 * 1_000;

function text(value, limit = 160) {
  return String(value || "")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, limit);
}

function escapeHTML(value, limit = 500) {
  return escapeHTMLRun(text(value, limit));
}

function escapeHTMLRun(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

// The same handle shape the relay pings on, so what is highlighted here is
// exactly what gets a notification. An email address is never a mention.
const TASK_MENTION_RE = /(^|[^A-Za-z0-9._%+-])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)/gi;
const TASK_ACCOUNT_NAME_RE = /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
const TASK_BOT_NAMES = new Set(["forkbot", "agent", "claude", "codex"]);

export function officeTaskProfileUrl(name) {
  const handle = String(name || "").trim().replace(/^@/, "").toLowerCase();
  if (!TASK_ACCOUNT_NAME_RE.test(handle) || TASK_BOT_NAMES.has(handle)) return "";
  const host = String(globalThis.location?.hostname || "").toLowerCase();
  const origin = ["app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"].includes(host)
    ? "https://forkmesh.com"
    : String(globalThis.location?.origin || "");
  return `${origin}/@${encodeURIComponent(handle)}`;
}

export function officeTaskProfileHTML(name) {
  const handle = text(name, 64).replace(/^@/, "");
  const label = `@${escapeHTML(handle, 64)}`;
  const url = officeTaskProfileUrl(handle);
  return url
    ? `<a class="world-task-profile-link" href="${escapeHTML(url)}">${label}</a>`
    : label;
}

export function mentionHTML(value, limit = 2000, viewer = "") {
  const source = text(value, limit);
  let out = "";
  let cursor = 0;
  TASK_MENTION_RE.lastIndex = 0;
  for (
    let match = TASK_MENTION_RE.exec(source);
    match;
    match = TASK_MENTION_RE.exec(source)
  ) {
    const start = match.index + match[1].length;
    out += escapeHTMLRun(source.slice(cursor, start));
    const name = match[2].toLowerCase();
    const mark = `<mark class="world-task-mention"${
      viewer && name === viewer ? ' data-self="true"' : ""
    }>@${escapeHTML(name, 64)}</mark>`;
    const url = officeTaskProfileUrl(name);
    out += url
      ? `<a class="world-task-mention-link" href="${escapeHTML(url)}">${mark}</a>`
      : mark;
    cursor = start + 1 + match[2].length;
  }
  return out + escapeHTMLRun(source.slice(cursor));
}

// Every recorded thing that has happened to one task, keyed by kind so the
// timeline can label an entry without parsing its summary.
const TASK_EVENT_LABELS = {
  created: ["✦", "created this task"],
  updated: ["✎", "edited this task"],
  started: ["▶", "started work"],
  stopped: ["❚❚", "paused work"],
  checkin: ["◎", "checked in"],
  replied: ["💬", "commented"],
  completed: ["✓", "completed this task"],
  reopened: ["↺", "reopened this task"],
  returned: ["↩", "returned this task to the list"],
  qa_requested: ["⚗", "sent this to QA"],
  qa_reviewed: ["🛡", "reviewed this in QA"],
  deleted: ["🗑", "deleted this task"],
  reminded: ["🔔", "sent a reminder"],
  reminder_seen: ["👁", "saw the reminder"],
};

function safeTaskId(value) {
  const candidate = String(value || "").trim().toLowerCase();
  return /^[a-f0-9-]{16,64}$/.test(candidate) ? candidate : "";
}

function safeIssueHref(value) {
  try {
    const url = new URL(String(value || ""), window.location.origin);
    if (
      url.origin !== window.location.origin ||
      !url.pathname.startsWith("/") ||
      url.username ||
      url.password
    ) {
      return "";
    }
    return `${url.pathname}${url.search}${url.hash}`.slice(0, 512);
  } catch (_) {
    return "";
  }
}

function normalizedRecentIssue(issue) {
  if (!issue || typeof issue !== "object") return null;
  const id = text(issue.id, 160);
  const title = text(issue.title, 160);
  if (!id || !title) return null;
  return {
    id,
    title,
    repo: text(issue.repo, 128),
    href: safeIssueHref(issue.href),
    assignedAt: timestampMs(issue.assignedAt),
  };
}

function timestampMs(value) {
  if (typeof value === "string" && value.trim() && !/^\d+$/.test(value.trim())) {
    const parsed = Date.parse(value);
    return Number.isFinite(parsed) ? Math.max(0, parsed) : 0;
  }
  const numeric = Number(value) || 0;
  return Math.max(0, numeric > 0 && numeric < 10 ** 11 ? numeric * 1000 : numeric);
}

// Provenance a desktop stamps on a task it opened from a prompt: the bot that
// launched the run, the bot that reported it finished, and the model,
// permission mode, and reasoning strength it ran with.
function normalizedAgentRun(agent) {
  if (!agent || typeof agent !== "object") return null;
  const run = {
    provider: text(agent.provider, 40).toLowerCase(),
    startedBy: text(agent.startedBy, 64).toLowerCase(),
    finishedBy: text(agent.finishedBy, 64).toLowerCase(),
    model: text(agent.model, 64),
    mode: text(agent.mode, 40),
    strength: text(agent.strength, 32).toLowerCase(),
    sessionId: text(agent.sessionId, 64),
    status: ["queued", "running", "waiting", "success", "failed", "stopped"]
      .includes(String(agent.status || "").toLowerCase())
      ? String(agent.status).toLowerCase()
      : "",
  };
  return Object.values(run).some(Boolean) ? run : null;
}

// The one-line "how this ran" summary shown under a bot-assigned task.
function agentRunSummary(run) {
  if (!run) return "";
  return [
    run.startedBy ? `started by ${run.startedBy}` : "",
    run.finishedBy ? `finished by ${run.finishedBy}` : "",
    run.model ? `model ${run.model}` : "",
    run.mode ? `mode ${run.mode}` : "",
    run.strength ? `strength ${run.strength}` : "",
  ]
    .filter(Boolean)
    .join(" · ");
}

function taskDateLabel(value) {
  const timestamp = timestampMs(value);
  if (!timestamp) return "—";
  return new Date(timestamp).toLocaleString([], {
    month: "short",
    day: "numeric",
    hour: "numeric",
    minute: "2-digit",
  });
}

function taskAvatarHue(value) {
  let hash = 0;
  for (const character of String(value || "task")) {
    hash = (Math.imul(hash, 31) + character.charCodeAt(0)) >>> 0;
  }
  return hash % 360;
}

function normalizedTask(task) {
  if (!task || typeof task !== "object") return null;
  const id = safeTaskId(task.id);
  const title = text(task.title);
  const assignee = text(task.assignee, 64).toLowerCase();
  // One general bot replaced the Claude/Codex choice; legacy rows still
  // arrive with the old kind and are shown as the same bot.
  const assigneeKind = ["agent", "claude", "codex"].includes(task.assigneeKind)
    ? "agent"
    : ["user", "unassigned"].includes(task.assigneeKind)
      ? task.assigneeKind
      : "user";
  const status = ["active", "done"].includes(task.status)
    ? task.status
    : "idle";
  const kind = task.kind === "bid" ? "bid" : "task";
  const bountyAmountSol =
    kind === "bid" &&
    /^(?:0|[1-9][0-9]{0,6})(?:\.[0-9]{1,9})?$/.test(
      String(task.bountyRequest?.amountSol || ""),
    )
      ? String(task.bountyRequest.amountSol)
      : "";
  if (!id || !title || (assigneeKind === "user" && !assignee)) return null;
  if (kind === "bid" && !bountyAmountSol) return null;
  return {
    id,
    kind,
    title,
    details: text(task.details, 4000),
    attachments: (Array.isArray(task.attachments) ? task.attachments : [])
      .slice(0, 4)
      .map((attachment) => ({
        id: text(attachment?.id, 64).toLowerCase(),
        name: text(attachment?.name, 180),
        mime: text(attachment?.mime, 100),
        size: Math.max(0, Math.min(1024 * 1024, Number(attachment?.size) || 0)),
        thumbnail:
          /^data:image\/(?:png|jpeg|webp);base64,[A-Za-z0-9+/=]+$/.test(
            String(attachment?.thumbnail || ""),
          ) && String(attachment.thumbnail).length <= 10_000
            ? String(attachment.thumbnail)
            : "",
      }))
      .filter((attachment) => attachment.name && attachment.size),
    completionNote: text(task.completionNote, 4000),
    createdBy: text(task.createdBy, 64).toLowerCase(),
    bountyRequest:
      kind === "bid"
        ? {
            currency: "SOL",
            amountSol: bountyAmountSol,
            status: "requested",
          }
        : null,
    assignee,
    assigneeKind,
    agentSessionId: safeTaskId(task.agentSessionId),
    department: text(task.department, 64).toLowerCase() || "general",
    team: text(task.team, 64).toLowerCase(),
    destination: text(task.destination, 32).toLowerCase() || "department",
    priority: Math.max(1, Math.min(99, Number(task.priority) || 50)),
    parentTaskId: safeTaskId(task.parentTaskId),
    repository: text(task.repository, 201),
    agent: normalizedAgentRun(task.agent),
    qa:
      task.qa && typeof task.qa === "object"
        ? {
            status: ["passed", "failed"].includes(task.qa.status)
              ? task.qa.status
              : "unknown",
            reviewer: text(task.qa.reviewer, 64).toLowerCase(),
            reviewedAt: timestampMs(task.qa.reviewedAt),
            requestedAt: timestampMs(task.qa.requestedAt),
          }
        : {
            status: "unknown",
            reviewer: "",
            reviewedAt: 0,
            requestedAt: 0,
          },
    replyCount: Math.max(0, Math.floor(Number(task.replyCount) || 0)),
    lastReplyAt: timestampMs(task.lastReplyAt),
    status,
    elapsedMs: Math.max(0, Number(task.elapsedMs) || 0),
    createdAt: timestampMs(task.createdAt),
    startedAt: timestampMs(task.startedAt),
    updatedAt: timestampMs(task.updatedAt),
    nextCheckinAt: timestampMs(task.nextCheckinAt),
    lastCheckin:
      task.lastCheckin && typeof task.lastCheckin === "object"
        ? {
            state: ["going_well", "blocked", "needs_help"].includes(
              task.lastCheckin.state,
            )
              ? task.lastCheckin.state
              : "",
            at: timestampMs(task.lastCheckin.at),
          }
        : null,
  };
}

function normalizedProof(proof) {
  if (!proof || typeof proof !== "object") return null;
  const id = safeTaskId(proof.id);
  const member = text(proof.member, 64).toLowerCase();
  const host = text(proof.host, 120).toLowerCase();
  const label = text(proof.label, 120);
  if (!id || !member || !host) return null;
  return {
    id,
    member,
    host,
    label: label || host,
    createdAt: timestampMs(proof.createdAt),
  };
}

function normalizedInitiative(initiative) {
  if (!initiative || typeof initiative !== "object") return null;
  const id = safeTaskId(initiative.id);
  const title = text(initiative.title, 160);
  const repo = text(initiative.repo, 201);
  const number = Math.max(0, Math.floor(Number(initiative.number) || 0));
  const href = safeIssueHref(initiative.href);
  if (!id || !title || !repo || !number || !href) return null;
  return {
    id,
    title,
    repo,
    number,
    href,
    createdAt: timestampMs(initiative.createdAt),
  };
}

export function formatOfficeTaskElapsed(value) {
  const totalSeconds = Math.max(0, Math.floor((Number(value) || 0) / 1000));
  const hours = Math.floor(totalSeconds / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  const seconds = totalSeconds % 60;
  if (hours) {
    return `${hours}:${String(minutes).padStart(2, "0")}:${String(
      seconds,
    ).padStart(2, "0")}`;
  }
  return `${minutes}:${String(seconds).padStart(2, "0")}`;
}

export function officeTaskCheckinDelay(randomValue = Math.random()) {
  const ratio = Math.min(1, Math.max(0, Number(randomValue) || 0));
  return Math.round(
    OFFICE_CHECKIN_MIN_MS +
      (OFFICE_CHECKIN_MAX_MS - OFFICE_CHECKIN_MIN_MS) * ratio,
  );
}

export function createWorldOfficeTasksController({
  root,
  world,
  fetchJSON,
  postJSON,
  getSession = () => null,
  notify = () => {},
  onQaVerdict = async () => false,
  random = Math.random,
}) {
  const panel = root.querySelector("[data-world-office-task-panel]");
  const heading = root.querySelector("#world-office-task-title");
  const status = root.querySelector("[data-world-office-task-status]");
  const list = root.querySelector("[data-world-office-task-list]");
  const manager = root.querySelector("[data-world-office-task-manager]");
  const form = root.querySelector("[data-world-office-task-form]");
  const titleInput = root.querySelector("[data-world-office-task-name]");
  const assigneeInput = root.querySelector("[data-world-office-task-assignee]");
  const assigneeOptions = root.querySelector(
    "[data-world-office-task-assignees]",
  );
  const checkin = root.querySelector("[data-world-office-task-checkin]");
  // The same board is mirrored into the Local controls "Work" tab so an
  // assignee can start and stop their own work without entering the Office.
  const workList = root.querySelector("[data-world-work-list]");
  const organizationList = root.querySelector(
    "[data-world-organization-task-list]",
  );
  const organizationHeading = root.querySelector(
    "[data-world-organization-task-heading]",
  );
  const organizationPager = root.querySelector(
    "[data-world-organization-task-pager]",
  );
  const workStatus = root.querySelector("[data-world-work-status]");
  const workTotal = root.querySelector("[data-world-work-total]");
  const workActive = root.querySelector("[data-world-work-active]");
  const workTracked = root.querySelector("[data-world-work-tracked]");
  const workIssueList = root.querySelector("[data-world-work-issue-list]");
  const quickForm = root.querySelector("[data-world-work-task-form]");
  const quickTitle = root.querySelector("[data-world-work-task-title]");
  const quickAssignee = root.querySelector("[data-world-work-task-assignee]");
  const quickDepartment = root.querySelector(
    "[data-world-work-task-department]",
  );
  const quickRepository = root.querySelector(
    "[data-world-work-task-repository]",
  );
  const quickPriority = root.querySelector("[data-world-work-task-priority]");
  const quickPriorityWrap = root.querySelector(
    "[data-world-work-task-priority-wrap]",
  );
  const quickSubmit = root.querySelector("[data-world-work-task-submit]");
  const quickStatus = root.querySelector(
    "[data-world-work-task-form-status]",
  );
  const taskSearch = root.querySelector("[data-world-task-search]");
  const taskFilter = root.querySelector("[data-world-task-filter]");
  const taskSort = root.querySelector("[data-world-task-sort]");
  const taskSortDirection = root.querySelector(
    "[data-world-task-sort-direction]",
  );
  const taskRefresh = root.querySelector("[data-world-task-refresh]");
  const taskBatchSize = root.querySelector("[data-world-task-batch-size]");
  const taskBatchSend = root.querySelector("[data-world-task-batch-send]");
  const checkinCopy = root.querySelector(
    "[data-world-office-task-checkin-copy]",
  );
  let officeActive = false;
  let personalView = false;
  let monitoring = false;
  let opened = false;
  let loading = false;
  let busyTaskId = "";
  let lastMutationError = "";
  let noticeOrigin = null;
  let noticeInPanel = false;
  let actor = "";
  let canManage = false;
  let authorized = false;
  let tasks = [];
  let recentIssues = [];
  let assignable = [];
  let marketingMembers = [];
  let organizationMembers = [];
  let departments = ["general"];
  let attendanceDays = [];
  let proofs = [];
  let initiatives = [];
  const announcedAgentTaskIds = new Set();
  let agentTasksInitialized = false;
  let syncedAt = performance.now();
  let serverNowAtSync = Date.now();
  let lastRefreshAt = 0;
  let refreshPromise = null;
  // Monotonic read counter. Only the newest read may write the task list, so a
  // slower reconciliation started before a delete can never resurrect the row
  // a faster forced read already dropped.
  let refreshSequence = 0;
  let pollTimer = 0;
  let tickTimer = 0;
  let checkinTimer = 0;
  let checkinTaskId = "";
  let physicalTickBucket = -1;
  let workSearch = "";
  let workFilter = "all";
  let workSort = "priority";
  let workSortAscending = true;
  // The board paints one page of rows at a time. The whole catalog stays in
  // memory (search, sort, stats and the Bot batch still see every task) — only
  // the DOM is bounded, so an organization with thousands of tasks pages
  // through them instead of silently losing everything past the first 100.
  let workPage = 0;
  const avatarCache = new Map();
  const replyWindows = new Map();
  const historyWindows = new Map();

  function ownTasks() {
    return tasks.filter(
      (task) => task.assigneeKind === "user" && task.assignee === actor,
    );
  }

  function marketingTasks() {
    return tasks.filter((task) => task.department === "marketing");
  }

  function ownActiveTasks() {
    return ownTasks().filter((task) => task.status === "active");
  }

  function keepMonitoring() {
    return officeActive || personalView || ownActiveTasks().length > 0;
  }

  function currentElapsed(task) {
    return Math.max(
      0,
      task.elapsedMs +
        (task.status === "active" ? performance.now() - syncedAt : 0),
    );
  }

  function currentServerNow() {
    return serverNowAtSync + Math.max(0, performance.now() - syncedAt);
  }

  function setStatus(message, tone = "") {
    if (!status) return;
    status.textContent = text(message, 240);
    if (tone) status.dataset.tone = tone;
    else delete status.dataset.tone;
  }

  function trackNoticeOrigin(event) {
    noticeOrigin = event?.target instanceof Element ? event.target : null;
    noticeInPanel = Boolean(noticeOrigin && panel?.contains(noticeOrigin));
  }

  function report(message, tone = "") {
    if (noticeInPanel && opened) {
      setStatus(message, tone);
      return;
    }
    const options = noticeOrigin?.isConnected ? { at: noticeOrigin } : {};
    if (tone) options.kind = tone;
    if (!noticeOrigin) options.at = "[data-world-hud-notice]";
    notify(message, options);
  }

  function selfWorkState(state, message = "") {
    // The owner-only plate on the back of your avatar carries the same work,
    // narrowed to your assignments plus recent private assignment notices. It
    // never enters a presence frame, socket, storage record, or analytics.
    const own = ownTasks();
    world.setSelfWorkBoard?.({
      state:
        ["ready", "loading"].includes(state) ||
        (recentIssues.length && getSession()?.sessionToken)
          ? state === "loading"
            ? "loading"
            : "ready"
          : "locked",
      message: text(message, 64),
      total: own.length + recentIssues.length,
      active: own.filter((task) => task.status === "active").length,
      tracked: formatOfficeTaskElapsed(
        own.reduce((sum, task) => sum + currentElapsed(task), 0),
      ),
      items: [
        ...own.map((task) => ({
          kind: "task",
          title: task.title,
          status: task.status,
          elapsed: formatOfficeTaskElapsed(currentElapsed(task)),
          checkin: checkinLabel(task.lastCheckin?.state),
        })),
        ...recentIssues.map((issue) => ({
          kind: "issue",
          title: issue.title,
          status: "idle",
          elapsed: "",
          checkin: issue.repo,
        })),
      ].slice(0, 6),
    });
  }

  function physicalState(state, message = "") {
    world.updateDesktopAgentBots?.(
      tasks
        .filter((task) => task.agent?.sessionId)
        .slice()
        .sort((left, right) => right.createdAt - left.createdAt)
        .slice(0, 24)
        .map((task) => ({
          id: task.agent.sessionId,
          taskId: task.id,
          title: task.title,
          provider: task.agent.provider,
          model: task.agent.model,
          strength: task.agent.strength,
          status:
            task.agent.status ||
            (task.status === "done"
              ? "success"
              : task.status === "active"
                ? "running"
                : "queued"),
        })),
    );
    world.updateOfficeMarketingTasks?.({
      authorized,
      state,
      message: text(message, 80),
      actor,
      canManage,
      tasks: marketingTasks().map((task) => ({
        id: task.id,
        title: task.title,
        assignee: task.assignee,
        status: task.status,
        elapsed: formatOfficeTaskElapsed(currentElapsed(task)),
        canStartStop: task.assignee === actor && task.status !== "done",
        canComplete:
          (task.assignee === actor || canManage) && task.status !== "done",
        canDelete: canManage,
      })),
      members: canManage ? assignable : marketingMembers,
      attendanceDays,
      proofs,
      initiatives,
    });
    selfWorkState(state, message);
  }

  function announceAgentTasks() {
    const activeAgentTasks = tasks.filter(
      (task) => task.assigneeKind === "agent" && task.status !== "done",
    );
    const unseen = activeAgentTasks.filter(
      (task) => !announcedAgentTaskIds.has(task.id),
    );
    activeAgentTasks.forEach((task) => announcedAgentTaskIds.add(task.id));
    const announcements = agentTasksInitialized
      ? unseen.slice().reverse()
      : unseen.slice(0, 1);
    agentTasksInitialized = true;
    announcements.forEach((task) => {
      world.showAgentTaskBubble?.(task.assigneeKind, task.title);
    });
  }

  function checkinLabel(value) {
    if (value === "blocked") return "Blocked";
    if (value === "needs_help") return "Needs help";
    if (value === "going_well") return "Going well";
    return "";
  }

  function taskAvatarHTML(task) {
    if (task.assigneeKind === "agent") {
      return '<span class="world-task-avatar" data-kind="agent" aria-label="Bot assignee">🤖</span>';
    }
    if (task.assigneeKind === "unassigned") {
      return '<span class="world-task-avatar" data-kind="unassigned" aria-label="Unassigned">?</span>';
    }
    const name = task.assignee || "member";
    return `<span
      class="world-task-avatar"
      data-kind="user"
      data-world-task-avatar="${escapeHTML(name, 64)}"
      style="--task-avatar-hue:${taskAvatarHue(name)}"
      aria-label="@${escapeHTML(name, 64)} avatar"
    >${escapeHTML(name.slice(0, 1).toUpperCase(), 1)}</span>`;
  }

  function taskBoardControlsHTML(
    task,
    { own, activeTask, doneTask, qaVerdict, canReturn, canUndo },
  ) {
    const waiting = busyTaskId === task.id;
    return `
      <span class="world-office-task-controls world-task-row-controls" role="group" aria-label="Task actions">
        ${
          canManage || own || task.createdBy === actor
            ? `<button
                type="button"
                data-world-office-task-action="follow-up"
                data-world-office-task-id="${task.id}"
                ${waiting ? "disabled" : ""}
              >Follow-up</button>`
            : ""
        }
        ${
          own && !doneTask
            ? `<button
                type="button"
                data-world-office-task-action="${activeTask ? "stop" : "start"}"
                data-world-office-task-id="${task.id}"
                aria-label="${activeTask ? "Stop" : "Start"} ${escapeHTML(task.title)}"
                title="${activeTask ? "Stop timer" : "Start timer"}"
                ${waiting ? "disabled" : ""}
              >${waiting ? '<span class="world-task-button-spinner" aria-hidden="true"></span>' : activeTask ? "Stop" : "Start"}</button>`
            : ""
        }
        ${
          own && activeTask
            ? `<button
                type="button"
                data-world-office-task-checkin-open="${task.id}"
                aria-label="Check in on ${escapeHTML(task.title)}"
                title="Check in — tells your team how this is going"
                ${waiting ? "disabled" : ""}
              >Check in</button>`
            : ""
        }
        ${
          !own && !doneTask && task.assigneeKind === "user" && task.assignee
            ? `<button
                type="button"
                data-world-office-task-action="remind"
                data-world-office-task-id="${task.id}"
                aria-label="Remind @${escapeHTML(task.assignee, 64)} to check in on ${escapeHTML(task.title)}"
                title="Remind — pings @${escapeHTML(task.assignee, 64)} to check in"
                ${waiting ? "disabled" : ""}
              >Remind</button>`
            : ""
        }
        ${
          (own || canManage) && !doneTask
            ? `<button
                type="button"
                class="world-task-done-button"
                data-world-office-task-action="complete"
                data-world-office-task-id="${task.id}"
                aria-label="Mark ${escapeHTML(task.title)} done"
                title="Mark done"
                ${waiting ? "disabled" : ""}
              >${waiting ? '<span class="world-task-button-spinner" aria-hidden="true"></span>' : "Done"}</button>`
            : ""
        }
        ${
          doneTask
            ? `<span class="world-office-task-qa-actions" role="group" aria-label="QA verdict">
                ${["pass", "fail", "unsure"]
                  .map(
                    (verdict) => `<button
                      type="button"
                      class="world-office-task-qa-${verdict}"
                      data-world-office-task-action="qa-${verdict}"
                      data-world-office-task-id="${task.id}"
                      aria-pressed="${qaVerdict === verdict}"
                      aria-label="QA ${verdict}"
                      title="QA ${verdict}"
                      ${waiting ? "disabled" : ""}
                    >${verdict === "pass" ? "Pass" : verdict === "fail" ? "Fail" : "Unsure"}</button>`,
                  )
                  .join("")}
              </span>`
            : ""
        }
        ${
          canUndo
            ? `<button
                type="button"
                class="world-office-task-undo"
                data-world-office-task-action="undo"
                data-world-office-task-id="${task.id}"
                aria-label="Mark ${escapeHTML(task.title)} undone"
                title="Mark undone and resume routing"
                ${waiting ? "disabled" : ""}
              >Mark undone</button>`
            : ""
        }
        ${
          canReturn
            ? `<button
                type="button"
                class="world-office-task-return"
                data-world-office-task-action="return"
                data-world-office-task-id="${task.id}"
                aria-label="Return ${escapeHTML(task.title)} to tasks"
                title="Return to tasks"
                ${waiting ? "disabled" : ""}
              >Return</button>`
            : ""
        }
        ${
          canManage
            ? `<button
                type="button"
                class="world-office-task-delete"
                data-world-office-task-action="delete"
                data-world-office-task-id="${task.id}"
                ${waiting ? "disabled" : ""}
                aria-label="Delete ${escapeHTML(task.title)}"
                title="Delete task"
              >Delete</button>`
            : ""
        }
      </span>`;
  }

  function taskSortHeaderHTML(label, sort, column) {
    const active = workSort === sort;
    return `<button
      type="button"
      class="${column}"
      data-world-task-column-sort="${sort}"
      aria-label="Sort by ${label}"
      aria-pressed="${active}"
    >${label}${active ? `<span aria-hidden="true">${workSortAscending ? "↑" : "↓"}</span>` : ""}</button>`;
  }

  function taskTableHeaderHTML() {
    return `<div class="world-task-table-header" role="row">
      ${taskSortHeaderHTML("Task", "title", "world-task-cell-title")}
      ${taskSortHeaderHTML("Status", "status", "world-task-cell-status")}
      ${taskSortHeaderHTML("P", "priority", "world-task-cell-priority")}
      ${taskSortHeaderHTML("Owner", "assignee", "world-task-cell-owner")}
      <span class="world-task-cell-agent" role="columnheader">Agent</span>
      <span class="world-task-cell-model" role="columnheader">Model</span>
      <span class="world-task-cell-speed" role="columnheader">Speed</span>
      ${taskSortHeaderHTML("Routing", "department", "world-task-cell-route")}
      ${taskSortHeaderHTML("Updated", "updated", "world-task-cell-updated")}
      ${taskSortHeaderHTML("Time", "tracked", "world-task-cell-time")}
      ${taskSortHeaderHTML("QA", "qa", "world-task-cell-qa")}
      <span class="world-task-cell-actions" role="columnheader">Actions</span>
    </div>`;
  }

  function taskRepliesHTML(task) {
    const replies = replyWindows.get(task.id) || [];
    return `<form class="world-office-task-completion-editor world-office-task-replies" data-world-office-task-reply-form="${task.id}">
      <span>Private comments (${task.replyCount}) · newest 50 retained
        ${task.replyCount ? `<button type="button" data-world-office-task-replies="${task.id}">${replies.length ? "Hide comments" : "Load comments"}</button>` : ""}
      </span>
      ${replies.length ? `<ol>${replies.map((reply) => `<li><strong>${officeTaskProfileHTML(reply.author)}</strong> ${mentionHTML(reply.body, 2000, actor)}</li>`).join("")}</ol>` : ""}
      <textarea maxlength="2000" rows="2" name="body" placeholder="Comment with context, a decision, or a blocker — type @name to ping…"></textarea>
      <button type="submit" ${busyTaskId === task.id ? "disabled" : ""}>Comment</button>
    </form>`;
  }

  // The task's own timeline: created, started, checked in, commented, done.
  // Loaded on demand so opening a row never costs an extra read per task.
  function taskHistoryHTML(task) {
    const history = historyWindows.get(task.id);
    const open = Array.isArray(history);
    return `<section class="world-office-task-history">
      <span>Activity
        <button type="button" data-world-office-task-history="${task.id}">${
          open ? "Hide activity" : "Show activity"
        }</button>
      </span>
      ${
        !open
          ? ""
          : history.length
            ? `<ol class="world-task-timeline">${history
                .map((entry) => {
                  const [glyph, label] =
                    TASK_EVENT_LABELS[entry.kind] || ["·", entry.kind];
                  return `<li data-kind="${escapeHTML(entry.kind, 32)}">
                    <span class="world-task-timeline-glyph" aria-hidden="true">${glyph}</span>
                    <span class="world-task-timeline-copy">
                      <strong>${entry.actor ? "@" + escapeHTML(entry.actor, 64) : "Someone"}</strong>
                      ${escapeHTML(label, 80)}${
                        entry.summary
                          ? ` — ${mentionHTML(entry.summary, 200, actor)}`
                          : ""
                      }
                    </span>
                    <time datetime="${new Date(entry.createdAt || 0).toISOString()}">${escapeHTML(taskDateLabel(entry.createdAt))}</time>
                  </li>`;
                })
                .join("")}</ol>`
            : '<p class="world-task-timeline-empty">No activity recorded on this task yet.</p>'
      }
    </section>`;
  }

  function taskBoardHTML(task) {
    const own = task.assigneeKind === "user" && task.assignee === actor;
    const activeTask = task.status === "active";
    const doneTask = task.status === "done";
    const botTask = task.assigneeKind === "agent";
    const qaVerdict =
      task.qa.status === "passed"
        ? "pass"
        : task.qa.status === "failed"
          ? "fail"
          : task.qa.reviewer
            ? "unsure"
            : "";
    const canReturn =
      botTask && !doneTask && (canManage || task.createdBy === actor);
    const canUndo =
      doneTask &&
      (canManage || own || task.createdBy === actor || task.qa.reviewer === actor);
    const statusLabel = activeTask
      ? "Running"
      : doneTask
        ? "Done"
        : botTask && task.agentSessionId
          ? "Queued"
          : "Ready";
    const statusIcon = activeTask
      ? '<i class="world-task-inline-spinner" aria-hidden="true"></i>'
      : doneTask
        ? '<i class="world-task-status-icon" data-icon="done" aria-hidden="true">✓</i>'
        : botTask && task.agentSessionId
          ? '<i class="world-task-status-icon" data-icon="queued" aria-hidden="true">≡</i>'
          : '<i class="world-task-status-icon" data-icon="ready" aria-hidden="true">◆</i>';
    const assigneeLabel =
      task.assigneeKind === "user"
        ? `@${task.assignee}`
        : task.assigneeKind === "agent"
          ? "Bot"
          : "Unassigned";
    const assigneeHTML =
      task.assigneeKind === "user"
        ? officeTaskProfileHTML(task.assignee)
        : escapeHTML(assigneeLabel);
    const checkinState = checkinLabel(task.lastCheckin?.state);
    const qaLabel =
      qaVerdict === "pass"
        ? "Passed"
        : qaVerdict === "fail"
          ? "Failed"
          : qaVerdict === "unsure"
            ? "Unsure"
            : doneTask || task.qa.requestedAt
              ? "Ready"
              : "—";
    const routeLabel = [
      task.department,
      task.team,
      task.repository,
    ].filter(Boolean).join(" · ");
    const agentLabel =
      task.agent?.provider || task.agent?.startedBy || (botTask ? "Bot" : "—");
    const modelLabel = task.agent?.model || "—";
    const speedLabel = task.agent?.strength || task.agent?.mode || "—";
    const metadata = [
      ["Priority", `P${task.priority}`],
      ["Status", statusLabel],
      ["Assignee", assigneeLabel],
      ["Created by", task.createdBy ? `@${task.createdBy}` : "—"],
      ["Department", task.department],
      ["Team", task.team || "—"],
      ["Destination", task.destination],
      ["Repository", task.repository || "—"],
      ["Created", taskDateLabel(task.createdAt)],
      ["Updated", taskDateLabel(task.updatedAt)],
      ["Started", taskDateLabel(task.startedAt)],
      ["Next check-in", taskDateLabel(task.nextCheckinAt)],
      ["Last check-in", checkinState || "—"],
      ["Check-in time", taskDateLabel(task.lastCheckin?.at)],
      ["QA", task.qa.status || "unknown"],
      ["QA reviewer", task.qa.reviewer ? `@${task.qa.reviewer}` : "—"],
      ["QA requested", taskDateLabel(task.qa.requestedAt)],
      ["QA reviewed", taskDateLabel(task.qa.reviewedAt)],
      [
        "Bounty",
        task.bountyRequest
          ? `${task.bountyRequest.amountSol} ${task.bountyRequest.currency} · ${task.bountyRequest.status}`
          : "—",
      ],
      ["Agent session", task.agentSessionId || "—"],
      ["Follows task", task.parentTaskId || "—"],
      ["Agent provider", task.agent?.provider || "—"],
      ["Agent model", task.agent?.model || "—"],
      ["Agent mode", task.agent?.mode || "—"],
      ["Agent strength", task.agent?.strength || "—"],
      ["Run session", task.agent?.sessionId || "—"],
      ["Started by", task.agent?.startedBy || "—"],
      ["Finished by", task.agent?.finishedBy || "—"],
      ["Task ID", task.id],
    ];
    return `
      <details
        class="world-office-task world-task-row${own ? " world-task-row--mine" : ""}"
        data-kind="${task.kind}"
        data-status="${task.status}"
        data-priority="${task.priority}"
        role="row"
      >
        <summary class="world-task-row-summary">
          <span class="world-task-cell-title" role="cell" title="${escapeHTML(task.title, 500)} · ${escapeHTML(task.details || "No description", 1000)}">
            ${taskAvatarHTML(task)}
            ${task.attachments
              .filter((attachment) => attachment.thumbnail)
              .slice(0, 3)
              .map(
                (attachment) =>
                  `<img class="world-task-attachment-thumbnail" src="${attachment.thumbnail}" alt="" title="${escapeHTML(attachment.name, 180)}" />`,
              )
              .join("")}
            <strong>${escapeHTML(task.title)}</strong>
            ${task.attachments.length ? `<i aria-label="${task.attachments.length} attachment(s)" title="${task.attachments.length} attachment(s)">📎</i>` : ""}
          </span>
          <span class="world-task-cell-status" role="cell" data-tone="${activeTask ? "active" : doneTask ? "done" : "ready"}">
            ${statusIcon}${statusLabel}
          </span>
          <span class="world-task-cell-priority" role="cell">
            ${
              canManage && !doneTask && !activeTask
                ? `<label class="world-task-inline-editor" title="Edit priority">
                    <span class="world-visually-hidden">Priority for ${escapeHTML(task.title)}</span>
                    P<input type="number" min="1" max="99" value="${task.priority}"
                      data-world-task-priority="${task.id}" aria-label="Priority for ${escapeHTML(task.title)}" />
                  </label>`
                : `P${task.priority}`
            }
          </span>
          <span class="world-task-cell-owner" role="cell" title="${escapeHTML(assigneeLabel)}">
            ${
              canManage && !doneTask && !activeTask
                ? `<label class="world-task-inline-editor" title="Change assignee">
                    <span class="world-visually-hidden">Assignee for ${escapeHTML(task.title)}</span>
                    <select data-world-task-assignee="${task.id}" aria-label="Assignee for ${escapeHTML(task.title)}">
                      <option value="agent" ${task.assigneeKind === "agent" ? "selected" : ""}>Bot</option>
                      <option value="unassigned" ${task.assigneeKind === "unassigned" ? "selected" : ""}>Unassigned</option>
                      ${organizationMembers.map((name) => `<option value="user:${escapeHTML(name)}" ${task.assigneeKind === "user" && task.assignee === name ? "selected" : ""}>@${escapeHTML(name)}</option>`).join("")}
                    </select>
                  </label>`
                : assigneeHTML
            }
          </span>
          <span class="world-task-cell-agent" role="cell" title="${escapeHTML(agentLabel)}">${escapeHTML(agentLabel)}</span>
          <span class="world-task-cell-model" role="cell" title="${escapeHTML(modelLabel)}">${escapeHTML(modelLabel)}</span>
          <span class="world-task-cell-speed" role="cell" title="${escapeHTML(speedLabel)}">${escapeHTML(speedLabel)}</span>
          <span class="world-task-cell-route" role="cell" title="${escapeHTML(routeLabel || "—", 500)}">${escapeHTML(routeLabel || "—")}</span>
          <time class="world-task-cell-updated" role="cell" datetime="${new Date(task.updatedAt || task.createdAt || 0).toISOString()}">${escapeHTML(taskDateLabel(task.updatedAt || task.createdAt))}</time>
          <time
            class="world-task-cell-time"
            role="cell"
            data-world-office-task-elapsed="${task.id}"
            datetime="PT${Math.floor(currentElapsed(task) / 1000)}S"
            title="Tracked time"
          >${formatOfficeTaskElapsed(currentElapsed(task))}</time>
          <span class="world-task-cell-qa" role="cell" data-verdict="${qaVerdict || (qaLabel === "Ready" ? "ready" : "")}">${qaLabel}</span>
          <span class="world-task-cell-actions" role="cell">
            ${taskBoardControlsHTML(task, {
              own,
              activeTask,
              doneTask,
              qaVerdict,
              canReturn,
              canUndo,
            })}
            <span class="world-task-detail-chevron" aria-hidden="true">›</span>
          </span>
        </summary>
        <section class="world-task-chat-bubble">
          <header class="world-task-row-heading">
            ${taskAvatarHTML(task)}
            <div class="world-task-row-title">
              <strong>${escapeHTML(task.title)}</strong>
              <div class="world-task-row-badges">
              <span data-tone="priority">P${task.priority}</span>
              <span data-tone="${activeTask ? "active" : doneTask ? "done" : "ready"}">
                ${statusIcon}${statusLabel}
              </span>
              <span>${assigneeHTML}</span>
              <span data-tone="department">${escapeHTML(task.department)}</span>
              ${task.team ? `<span>${escapeHTML(task.team)}</span>` : ""}
              ${task.kind === "bid" ? '<span data-tone="bid">SOL bid</span>' : ""}
            </div>
          </div>
          </header>
        ${
          task.details
            ? `<p class="world-task-row-description">${mentionHTML(task.details, 4000, actor)}</p>`
            : '<p class="world-task-row-description world-task-row-description--empty">No additional description.</p>'
        }
        <dl class="world-task-row-metadata">
          ${metadata
            .map(
              ([label, value]) =>
                `<div><dt>${escapeHTML(label)}</dt><dd title="${escapeHTML(value, 500)}">${escapeHTML(value, 500)}</dd></div>`,
            )
            .join("")}
        </dl>
        ${
          task.attachments.length
            ? `<ul class="world-office-task-attachments" aria-label="Task attachments">${task.attachments
                .map(
                  (attachment) =>
                    `<li title="${escapeHTML(attachment.mime)} · ${attachment.size.toLocaleString()} bytes">📎 ${escapeHTML(attachment.name, 180)}</li>`,
                )
                .join("")}</ul>`
            : ""
        }
        ${
          agentRunSummary(task.agent)
            ? `<p class="world-task-agent-run"><span aria-hidden="true">⚡</span>${escapeHTML(
                agentRunSummary(task.agent),
                300,
              )}</p>`
            : ""
        }
        ${
          doneTask && task.completionNote
            ? `<section class="world-office-task-completion">
                <span>Completion report</span>
                <p>${escapeHTML(task.completionNote, 4000)}</p>
              </section>`
            : ""
        }
        ${
          (own || canManage) && !doneTask
            ? `<details class="world-task-row-completion">
                <summary>Add completion notes</summary>
                <label class="world-office-task-completion-editor">
                  <span>What was changed, decided, and verified?</span>
                  <textarea
                    maxlength="4000"
                    rows="3"
                    data-world-office-task-completion-note="${task.id}"
                    placeholder="Summarize the completed work for QA…"
                  >${escapeHTML(task.completionNote, 4000)}</textarea>
                </label>
              </details>`
            : ""
        }
        ${taskHistoryHTML(task)}
        ${taskRepliesHTML(task)}
        </section>
      </details>`;
  }

  function visibleWorkTasks() {
    const query = workSearch.toLowerCase();
    const statusRank = { active: 0, idle: 1, done: 2 };
    const filtered = tasks.filter((task) => {
      if (workFilter === "mine" && task.assignee !== actor) return false;
      if (workFilter === "agent" && task.assigneeKind !== "agent") return false;
      if (
        workFilter.startsWith("user:") &&
        !(
          task.assigneeKind === "user" &&
          task.assignee === workFilter.slice(5)
        )
      ) {
        return false;
      }
      if (
        workFilter === "queued" &&
        !(
          task.status === "idle" &&
          task.assigneeKind === "agent" &&
          task.agentSessionId
        )
      ) {
        return false;
      }
      if (
        ["active", "idle", "done"].includes(workFilter) &&
        task.status !== workFilter
      ) {
        return false;
      }
      if (!query) return true;
      return [
        task.title,
        task.details,
        task.assignee,
        task.createdBy,
        task.department,
        task.team,
        task.destination,
        task.repository,
        task.agent?.provider,
        task.agent?.model,
        task.agent?.mode,
        task.agent?.strength,
        task.id,
      ].some((value) => String(value || "").toLowerCase().includes(query));
    });
    const value = (task) => {
      if (workSort === "status") return statusRank[task.status] ?? 9;
      if (workSort === "updated") return task.updatedAt || task.createdAt || 0;
      if (workSort === "assignee") {
        return task.assigneeKind === "agent"
          ? "bot"
          : task.assignee || "zz-unassigned";
      }
      if (workSort === "department") return task.department;
      if (workSort === "repository") return task.repository || "";
      if (workSort === "tracked") return currentElapsed(task);
      if (workSort === "qa") return task.qa.status || "unknown";
      if (workSort === "title") return task.title.toLowerCase();
      return task.priority;
    };
    return filtered.sort((left, right) => {
      const leftValue = value(left);
      const rightValue = value(right);
      const compared =
        typeof leftValue === "number" && typeof rightValue === "number"
          ? leftValue - rightValue
          : String(leftValue).localeCompare(String(rightValue));
      return (workSortAscending ? 1 : -1) * (
        compared || left.title.localeCompare(right.title)
      );
    });
  }

  function hydrateTaskAvatars() {
    root.querySelectorAll("[data-world-task-avatar]").forEach((avatar) => {
      const name = text(avatar.dataset.worldTaskAvatar, 64).toLowerCase();
      if (!name || avatar.dataset.hydrated === "true") return;
      let pending = avatarCache.get(name);
      if (!pending) {
        pending = fetchJSON(
          `/api/accounts/${encodeURIComponent(name)}`,
          { cache: "force-cache", timeout: 5000 },
        )
          .then((profile) => {
            const png = String(profile?.avatarPng || "");
            return (
              profile?.exists === true &&
              profile?.profilePrivate !== true &&
              png.length <= 350_000 &&
              /^[A-Za-z0-9+/=]+$/.test(png)
            )
              ? png
              : "";
          })
          .catch(() => "");
        avatarCache.set(name, pending);
      }
      void pending.then((png) => {
        if (!png || !avatar.isConnected) return;
        const image = document.createElement("img");
        image.src = `data:image/png;base64,${png}`;
        image.alt = "";
        avatar.replaceChildren(image);
        avatar.dataset.hydrated = "true";
      });
    });
  }

  function taskHTML(task) {
    const own = task.assigneeKind === "user" && task.assignee === actor;
    const activeTask = task.status === "active";
    const doneTask = task.status === "done";
    const qaReady = doneTask || task.qa.requestedAt > 0;
    const qaVerdict =
      task.qa.status === "passed"
        ? "pass"
        : task.qa.status === "failed"
          ? "fail"
          : task.qa.reviewer
            ? "unsure"
            : "";
    const qaLabel =
      qaVerdict === "pass"
        ? "QA passed"
        : qaVerdict === "fail"
          ? "QA failed"
          : qaVerdict === "unsure"
            ? "QA unsure"
            : qaReady
              ? "Ready for QA"
              : "";
    const checkinState = checkinLabel(task.lastCheckin?.state);
    // A bot task waits on a node queue until it reports back. Its author, or
    // an organization manager, can pull it back onto the task list.
    const botTask = task.assigneeKind === "agent";
    const canReturn =
      botTask && !doneTask && (canManage || task.createdBy === actor);
    const canUndo =
      doneTask &&
      (canManage || own || task.createdBy === actor || task.qa.reviewer === actor);
    const bid = task.kind === "bid";
    const bidder = task.createdBy || task.assignee;
    return `
      <li class="world-office-task" data-kind="${bid ? "bid" : "task"}" data-status="${doneTask ? "done" : activeTask ? "active" : "idle"}">
        <div class="world-office-task-copy">
          <span
            class="world-office-task-state"
            aria-hidden="true"
            title="${activeTask ? "In progress" : doneTask ? "Done" : "Ready"}"
          ></span>
          <div>
            <strong>${
              bid
                ? '<span class="world-office-task-bid-badge">Bid</span>'
                : ""
            }${escapeHTML(task.title)}</strong>
            <span class="world-office-task-priority">Global P${task.priority}</span>
            ${
              task.details
                ? `<p class="world-office-task-details">${escapeHTML(
                    task.details,
                    1000,
                  )}</p>`
                : ""
            }
            ${
              task.attachments.length
                ? `<ul class="world-office-task-attachments" aria-label="Task attachments">${task.attachments
                    .map(
                      (attachment) =>
                        attachment.url
                          ? `<li class="world-office-task-image"><a href="${escapeHTML(
                              attachment.url,
                            )}" target="_blank" rel="noopener"><img src="${escapeHTML(
                              attachment.url,
                            )}" alt="${escapeHTML(
                              attachment.name,
                              180,
                            )}" loading="lazy" decoding="async"><span>${escapeHTML(
                              attachment.name,
                              180,
                            )}</span></a></li>`
                          : `<li title="${escapeHTML(attachment.mime)}">📎 ${escapeHTML(
                              attachment.name,
                              180,
                            )}</li>`,
                    )
                    .join("")}</ul>`
                : ""
            }
            <small>
              ${
                bid
                  ? `${escapeHTML(`${task.bountyRequest.amountSol} SOL bounty requested · bidder`)} ${officeTaskProfileHTML(bidder)}`
                  : task.assigneeKind === "user"
                    ? officeTaskProfileHTML(task.assignee)
                    : escapeHTML(
                        task.assigneeKind === "unassigned" ? "Unassigned" : "Bot",
                      )
              }
              · ${escapeHTML(task.department)}
              ${task.team ? ` / ${escapeHTML(task.team)}` : ""}
              ${botTask && task.agentSessionId ? " · queued on a node" : ""}
              ${qaLabel ? ` · ${escapeHTML(qaLabel)}` : ""}
              ${task.qa.reviewer ? ` by ${officeTaskProfileHTML(task.qa.reviewer)}` : ""}
              ${doneTask ? " · done" : ""}
              ${checkinState ? ` · last check-in: ${escapeHTML(checkinState)}` : ""}
            </small>
            ${
              agentRunSummary(task.agent)
                ? `<small class="world-office-task-agent-run">${escapeHTML(
                    agentRunSummary(task.agent),
                    300,
                  )}</small>`
                : ""
            }
            ${
              activeTask
                ? '<span class="world-office-task-progress-label">In progress</span>'
                : ""
            }
            ${
              doneTask && task.completionNote
                ? `<section class="world-office-task-completion">
                    <span>What I did to complete this task</span>
                    <p>${escapeHTML(task.completionNote, 4000)}</p>
                  </section>`
                : ""
            }
            ${
              (own || canManage) && !doneTask
                ? `<label class="world-office-task-completion-editor">
                    <span>What I did to complete this task</span>
                    <textarea
                      maxlength="4000"
                      rows="3"
                      data-world-office-task-completion-note="${task.id}"
                      placeholder="Summarize the changes, decisions, and verification completed…"
                    >${escapeHTML(task.completionNote, 4000)}</textarea>
                  </label>`
                : ""
            }
            ${taskRepliesHTML(task)}
          </div>
        </div>
        <div class="world-office-task-controls">
          <time
            data-world-office-task-elapsed="${task.id}"
            datetime="PT${Math.floor(currentElapsed(task) / 1000)}S"
          >${formatOfficeTaskElapsed(currentElapsed(task))}</time>
          ${
            own && !doneTask
              ? `<button
                  type="button"
                  data-world-office-task-action="${activeTask ? "stop" : "start"}"
                  data-world-office-task-id="${task.id}"
                  ${busyTaskId === task.id ? "disabled" : ""}
                >${activeTask ? "Stop" : "Start"}</button>`
              : ""
          }
          ${
            (own || canManage) && !doneTask
              ? `<button
                  type="button"
                  data-world-office-task-action="complete"
                  data-world-office-task-id="${task.id}"
                  ${busyTaskId === task.id ? "disabled" : ""}
                >Done</button>`
              : ""
          }
          ${
            doneTask
              ? `<span class="world-office-task-qa-actions" role="group" aria-label="QA verdict">
                  ${["pass", "fail", "unsure"]
                    .map(
                      (verdict) => `<button
                        type="button"
                        class="world-office-task-qa-${verdict}"
                        data-world-office-task-action="qa-${verdict}"
                        data-world-office-task-id="${task.id}"
                        aria-pressed="${qaVerdict === verdict}"
                        ${busyTaskId === task.id ? "disabled" : ""}
                      >${verdict[0].toUpperCase() + verdict.slice(1)}</button>`,
                    )
                    .join("")}
                </span>`
              : ""
          }
          ${
            canUndo
              ? `<button
                  type="button"
                  class="world-office-task-undo"
                  data-world-office-task-action="undo"
                  data-world-office-task-id="${task.id}"
                  ${busyTaskId === task.id ? "disabled" : ""}
                >Mark undone</button>`
              : ""
          }
          ${
            canReturn
              ? `<button
                  type="button"
                  class="world-office-task-return"
                  data-world-office-task-action="return"
                  data-world-office-task-id="${task.id}"
                  ${busyTaskId === task.id ? "disabled" : ""}
                >Return to tasks</button>`
              : ""
          }
          ${
            canManage
              ? `<button
                  type="button"
                  class="world-office-task-delete"
                  data-world-office-task-action="delete"
                  data-world-office-task-id="${task.id}"
                  ${busyTaskId === task.id ? "disabled" : ""}
                >Delete</button>`
              : ""
          }
        </div>
      </li>`;
  }

  function issueHTML(issue) {
    const link = issue.href
      ? `<a class="world-work-issue-link" href="${escapeHTML(
          issue.href,
        )}">Open</a>`
      : "";
    return `
      <li class="world-office-task world-work-issue" data-status="issue">
        <div class="world-office-task-copy">
          <span class="world-office-task-state" aria-hidden="true"></span>
          <div>
            <strong>${escapeHTML(issue.title)}</strong>
            <small>${escapeHTML(
              issue.repo
                ? `${issue.repo} · recently assigned`
                : "Recently assigned issue",
            )}</small>
          </div>
        </div>
        ${link}
      </li>`;
  }

  function render() {
    if (manager) manager.hidden = !canManage;
    if (assigneeOptions) {
      const selected = text(assigneeInput?.value, 64).toLowerCase();
      assigneeOptions.innerHTML =
        `<option value="">Select a Marketing team member</option>` +
        assignable
          .map(
            (name) =>
              `<option value="${escapeHTML(name)}">${escapeHTML(name)}</option>`,
          )
          .join("");
      if (selected && assignable.includes(selected)) {
        assigneeInput.value = selected;
      }
    }
    if (list) {
      const marketing = marketingTasks();
      list.innerHTML = loading
        ? `<li class="world-office-task-empty">Loading marketing tasks…</li>`
        : marketing.length
          ? marketing.map(taskHTML).join("")
          : `<li class="world-office-task-empty">No marketing tasks are assigned here yet.</li>`;
    }
    if (panel) {
      panel.dataset.open = String(opened);
      panel.setAttribute("aria-hidden", String(!opened));
    }
    renderWorkPane();
    if (!loading) {
      setStatus(
        canManage
          ? `${marketingTasks().length} task${marketingTasks().length === 1 ? "" : "s"} · assignment is limited to Marketing team members`
          : marketingTasks().length
            ? "Only tasks assigned to you are shown."
            : "No tasks are currently assigned to you.",
      );
    }
  }

  // Stats only. The per-second tick calls this instead of renderWorkPane so
  // the row markup — and the Start/Stop button under the pointer — survives.
  function updateWorkStats() {
    const own = ownTasks();
    if (workTotal) workTotal.textContent = String(own.length);
    if (workActive) {
      workActive.textContent = String(
        own.filter((task) => task.status === "active").length,
      );
    }
    if (workTracked) {
      workTracked.textContent = formatOfficeTaskElapsed(
        own.reduce((sum, task) => sum + currentElapsed(task), 0),
      );
    }
    return own;
  }

  // Pages are clamped on read: a filter or a refresh can shrink the visible
  // set under the page the board was left on, and that must land on the last
  // real page rather than an empty one.
  function taskPageCount(visible) {
    return Math.max(1, Math.ceil(visible / TASK_PAGE_SIZE));
  }

  function currentTaskPage(visible) {
    return Math.min(Math.max(0, workPage), taskPageCount(visible) - 1);
  }

  function taskPagerHTML(visible) {
    if (visible < 1) return "";
    const pages = taskPageCount(visible);
    const page = currentTaskPage(visible);
    const first = page * TASK_PAGE_SIZE + 1;
    const last = Math.min(visible, (page + 1) * TASK_PAGE_SIZE);
    return `
      <button type="button" data-world-task-page="prev"${
        page < 1 ? " disabled" : ""
      } aria-label="Previous task page">‹ Prev</button>
      <span data-world-task-page-status>${first}–${last} of ${visible} · page ${
        page + 1
      } of ${pages}</span>
      <button type="button" data-world-task-page="next"${
        page + 1 >= pages ? " disabled" : ""
      } aria-label="Next task page">Next ›</button>`;
  }

  function renderWorkPane() {
    const own = updateWorkStats();
    const visibleTasks = visibleWorkTasks();
    const page = currentTaskPage(visibleTasks.length);
    workPage = page;
    const pageTasks = visibleTasks.slice(
      page * TASK_PAGE_SIZE,
      (page + 1) * TASK_PAGE_SIZE,
    );
    if (organizationHeading) {
      // Open/closed over the whole catalog, counted the way the desktop app
      // and the dashboard count it, so all three surfaces report the same
      // "84 open" (adhoc #56). The narrowed count only shows when a filter or
      // a search is actually hiding something.
      const openCount = tasks.reduce(
        (sum, task) => sum + (task.status === "done" ? 0 : 1),
        0,
      );
      const narrowed =
        visibleTasks.length === tasks.length
          ? ""
          : ` · ${visibleTasks.length} shown`;
      organizationHeading.textContent =
        `Tasks · ${openCount} open · ${tasks.length - openCount} closed${narrowed}`;
    }
    if (workList) {
      workList.innerHTML = loading
        ? `<li class="world-office-task-empty">Loading your assigned work\u2026</li>`
        : own.length
          ? own.map(taskHTML).join("")
          : `<li class="world-office-task-empty">${
              authorized
                ? "Nothing is assigned to you right now."
                : "Sign in to load the work assigned to you."
            }</li>`;
    }
    if (organizationPager) {
      organizationPager.innerHTML =
        loading || visibleTasks.length <= TASK_PAGE_SIZE
          ? ""
          : taskPagerHTML(visibleTasks.length);
      organizationPager.hidden = !organizationPager.innerHTML;
    }
    if (organizationList) {
      organizationList.innerHTML = loading
        ? `<div class="world-task-loading" role="status"><span class="world-task-loading-spinner" aria-hidden="true"></span><strong>Syncing organization tasks</strong><small>Reading private task state, timers, QA, and routing…</small></div>`
        : visibleTasks.length
          ? taskTableHeaderHTML() + pageTasks.map(taskBoardHTML).join("")
          : `<div class="world-office-task-empty">${
              authorized && tasks.length
                ? "No tasks match this search and filter."
                : authorized
                  ? "No organization tasks have been created yet."
                  : "Organization membership is required."
            }</div>`;
    }
    if (workStatus && !loading) {
      workStatus.textContent = own.length
        ? `${own.length} assigned \u00b7 ${
            own.filter((task) => task.status === "active").length
          } running`
        : "";
    }
    if (workIssueList) {
      workIssueList.innerHTML = recentIssues.length
        ? recentIssues.map(issueHTML).join("")
        : `<li class="world-office-task-empty">No recent issue assignments.</li>`;
    }
    renderQuickEntry();
    if (taskSearch && taskSearch.value !== workSearch) {
      taskSearch.value = workSearch;
    }
    if (taskFilter) {
      taskFilter.innerHTML = `
        <option value="all">All statuses and assignees</option>
        <optgroup label="Status">
          <option value="active">Running</option>
          <option value="queued">Queued</option>
          <option value="idle">Ready</option>
          <option value="done">Done</option>
        </optgroup>
        <optgroup label="Assignee">
          <option value="mine">Assigned to me</option>
          ${organizationMembers
            .map((name) => `<option value="user:${escapeHTML(name)}">@${escapeHTML(name)}</option>`)
            .join("")}
          <option value="agent">Bot tasks</option>
        </optgroup>`;
      taskFilter.value = workFilter;
    }
    root.querySelectorAll("[data-world-task-quick-filter]").forEach((button) => {
      button.setAttribute(
        "aria-pressed",
        String(button.dataset.worldTaskQuickFilter === workFilter),
      );
    });
    if (taskSort) taskSort.value = workSort;
    if (taskSortDirection) {
      taskSortDirection.value = workSortAscending ? "asc" : "desc";
      taskSortDirection.textContent = workSortAscending ? "↑" : "↓";
      taskSortDirection.setAttribute(
        "aria-label",
        workSortAscending ? "Sort ascending" : "Sort descending",
      );
    }
    if (taskRefresh) {
      taskRefresh.disabled = loading;
      taskRefresh.classList.toggle("is-loading", loading);
    }
    if (taskBatchSize && !taskBatchSize.value) taskBatchSize.value = "10";
    if (taskBatchSend) {
      taskBatchSend.disabled =
        loading ||
        !canManage ||
        !tasks.some(
          (task) =>
            task.status === "idle" &&
            task.assigneeKind === "unassigned" &&
            Boolean(task.repository),
        );
      taskBatchSend.hidden = !canManage;
      if (taskBatchSize) taskBatchSize.closest("label").hidden = !canManage;
    }
    hydrateTaskAvatars();
  }

  function renderQuickEntry() {
    if (!quickForm) return;
    quickForm.hidden = !authorized;
    if (!authorized) return;
    const selectedAssignee = quickAssignee?.value || "self";
    const assignees = [
      { value: "self", label: actor ? `You (@${actor})` : "You" },
      {
        value: "agent",
        label: "Bot",
      },
      { value: "unassigned", label: "Unassigned" },
    ];
    if (canManage) {
      organizationMembers
        .filter((name) => name && name !== actor)
        .forEach((name) => {
          assignees.push({ value: `user:${name}`, label: `@${name}` });
        });
    }
    if (quickAssignee) {
      quickAssignee.innerHTML = assignees
        .map(
          (item) =>
            `<option value="${escapeHTML(item.value)}">${escapeHTML(item.label)}</option>`,
        )
        .join("");
      quickAssignee.value = assignees.some(
        (item) => item.value === selectedAssignee,
      )
        ? selectedAssignee
        : "self";
    }
    const selectedDepartment = quickDepartment?.value || "general";
    if (quickDepartment) {
      quickDepartment.innerHTML = departments
        .map(
          (name) =>
            `<option value="${escapeHTML(name)}">${escapeHTML(
              name.replaceAll("-", " "),
            )}</option>`,
        )
        .join("");
      quickDepartment.value = departments.includes(selectedDepartment)
        ? selectedDepartment
        : "general";
    }
    if (quickPriorityWrap) quickPriorityWrap.hidden = !canManage;
    if (quickSubmit) quickSubmit.disabled = loading;
  }

  function updateElapsedLabels() {
    tasks.forEach((task) => {
      const elapsed = currentElapsed(task);
      // The same row can be painted twice (Office board plus Work tab), so
      // every matching clock is advanced, not just the first one found.
      root
        .querySelectorAll(`[data-world-office-task-elapsed="${task.id}"]`)
        .forEach((node) => {
          node.textContent = formatOfficeTaskElapsed(elapsed);
          node.setAttribute("datetime", `PT${Math.floor(elapsed / 1000)}S`);
        });
    });
    if (personalView) updateWorkStats();
    const nextBucket = Math.floor(performance.now() / 5000);
    if (ownActiveTasks().length && nextBucket !== physicalTickBucket) {
      physicalTickBucket = nextBucket;
      if (officeActive) physicalState("ready");
      else selfWorkState("ready");
    }
  }

  function clearCheckinTimer() {
    if (checkinTimer) window.clearTimeout(checkinTimer);
    checkinTimer = 0;
  }

  function closeCheckin() {
    checkinTaskId = "";
    if (checkin) {
      checkin.dataset.open = "false";
      checkin.setAttribute("aria-hidden", "true");
      // A dismissed note must not ride along on the next task's check-in.
      const note = checkin.querySelector(
        "[data-world-office-task-checkin-note]",
      );
      if (note) note.value = "";
    }
  }

  function showCheckin(taskId = "") {
    const candidates = ownActiveTasks();
    const task =
      candidates.find((item) => item.id === safeTaskId(taskId)) ||
      candidates[0];
    if (!monitoring || !task) return false;
    checkinTaskId = task.id;
    if (checkinCopy) {
      checkinCopy.textContent = `How is “${task.title}” going?`;
    }
    if (checkin) {
      checkin.dataset.open = "true";
      checkin.setAttribute("aria-hidden", "false");
      window.requestAnimationFrame(() =>
        checkin
          .querySelector("[data-world-office-task-checkin-state]")
          ?.focus(),
      );
    }
    return true;
  }

  function scheduleCheckin() {
    clearCheckinTimer();
    if (!monitoring || !ownActiveTasks().length) return;
    const serverDueAt = ownActiveTasks()
      .map((task) => task.nextCheckinAt)
      .filter((value) => value > 0)
      .sort((left, right) => left - right)[0];
    const delay = serverDueAt
      ? Math.max(500, serverDueAt - currentServerNow())
      : officeTaskCheckinDelay(random());
    checkinTimer = window.setTimeout(() => {
      checkinTimer = 0;
      if (document.hidden || !showCheckin()) {
        scheduleCheckin();
      }
    }, Math.min(delay, 2_147_000_000));
  }

  function schedulePoll() {
    if (pollTimer) window.clearTimeout(pollTimer);
    pollTimer = 0;
    if (!monitoring) return;
    const delay =
      document.hidden || (!officeActive && !opened && !personalView)
        ? OFFICE_TASKS_BACKGROUND_POLL_MS
        : OFFICE_TASKS_POLL_MS;
    pollTimer = window.setTimeout(async () => {
      pollTimer = 0;
      if (!document.hidden) await refresh({ quiet: true });
      schedulePoll();
    }, delay);
  }

  function onVisibilityChange() {
    if (!monitoring || document.hidden) return;
    if (Date.now() - lastRefreshAt < OFFICE_TASKS_POLL_MS) {
      schedulePoll();
      return;
    }
    if (pollTimer) window.clearTimeout(pollTimer);
    pollTimer = 0;
    void refresh({ quiet: true }).finally(schedulePoll);
  }

  // `force` reconciles right after a confirmed mutation: it may not join a
  // read that was already in flight before the change (that response predates
  // the delete and would repaint the removed row), and it runs even while the
  // low-frequency monitor is paused.
  async function fetchTaskCatalog(options) {
    let payload = null;
    const catalog = [];
    let offset = 0;
    do {
      const page = await fetchJSON(
        `${OFFICE_TASKS_PATH}${offset ? `?offset=${offset}` : ""}`,
        options,
      );
      if (!payload) payload = page;
      catalog.push(...(Array.isArray(page?.tasks) ? page.tasks : []));
      const next = Number(page?.nextOffset);
      if (page?.hasMore !== true || !Number.isInteger(next) || next <= offset) {
        break;
      }
      offset = next;
    } while (catalog.length < Number(payload?.taskLimit || MAX_TASKS));
    return { ...(payload || {}), tasks: catalog };
  }

  async function refresh({ quiet = false, force = false } = {}) {
    if ((!monitoring && !force) || typeof fetchJSON !== "function") return false;
    if (refreshPromise && !force) return refreshPromise;
    if (!getSession()?.sessionToken) {
      tasks = [];
      actor = "";
      canManage = false;
      authorized = false;
      assignable = [];
      marketingMembers = [];
      attendanceDays = [];
      proofs = [];
      initiatives = [];
      loading = false;
      if (!quiet) setStatus("Sign in to access organization marketing tasks.");
      physicalState("locked", "Organization access required");
      render();
      if (!officeActive) stopMonitoring();
      return false;
    }
    const sequence = ++refreshSequence;
    const request = (async () => {
      loading = !quiet;
      if (!quiet) {
        physicalState("loading", "Syncing organization tasks");
        render();
      }
      try {
        const [payload, marketingPayload] = await Promise.all([
          fetchTaskCatalog({
            cache: "no-store",
            timeout: 8000,
            dedupe: !force,
          }),
          fetchJSON(MARKETING_TASKS_PATH, {
            cache: "no-store",
            timeout: 8000,
            dedupe: !force,
          }).catch(() => ({})),
        ]);
        if (sequence !== refreshSequence) return false;
        actor = text(payload?.actor, 64).toLowerCase();
        canManage = payload?.canManage === true;
        authorized = payload?.authorized !== false;
        organizationMembers = Array.isArray(payload?.members)
          ? payload.members
              .map((name) => text(name, 64).toLowerCase())
              .filter(Boolean)
              .slice(0, 1000)
          : [];
        departments = Array.isArray(payload?.departments)
          ? payload.departments
              .map((name) => text(name, 64).toLowerCase())
              .filter(Boolean)
          : ["general"];
        assignable = Array.isArray(marketingPayload?.members)
          ? marketingPayload.members
              .map((name) => text(name, 64).toLowerCase())
              .filter(Boolean)
              .slice(0, 1000)
          : [];
        marketingMembers = Array.isArray(marketingPayload?.marketingMembers)
          ? marketingPayload.marketingMembers
              .map((name) => text(name, 64).toLowerCase())
              .filter(Boolean)
              .slice(0, 100)
          : [];
        attendanceDays = Array.isArray(marketingPayload?.attendanceDays)
          ? marketingPayload.attendanceDays.slice(-7)
          : [];
        proofs = Array.isArray(marketingPayload?.proofs)
          ? marketingPayload.proofs
              .map(normalizedProof)
              .filter(Boolean)
              .slice(0, 5000)
          : [];
        initiatives = Array.isArray(marketingPayload?.initiatives)
          ? marketingPayload.initiatives
              .map(normalizedInitiative)
              .filter(Boolean)
              .slice(0, 250)
          : [];
        tasks = Array.isArray(payload?.tasks)
          ? payload.tasks
              .map(normalizedTask)
              .filter(Boolean)
              .slice(0, MAX_TASKS)
          : [];
        announceAgentTasks();
        syncedAt = performance.now();
        serverNowAtSync = timestampMs(payload?.serverNow) || Date.now();
        lastRefreshAt = Date.now();
        loading = false;
        physicalState("ready");
        render();
        scheduleCheckin();
        if (!keepMonitoring()) stopMonitoring();
        return true;
      } catch (_) {
        if (sequence !== refreshSequence) return false;
        tasks = [];
        canManage = false;
        authorized = false;
        assignable = [];
        marketingMembers = [];
        attendanceDays = [];
        proofs = [];
        initiatives = [];
        organizationMembers = [];
        departments = ["general"];
        loading = false;
        render();
        setStatus(
          "Marketing tasks are unavailable or your account is not an authorized organization member.",
          "error",
        );
        physicalState("locked", "Organization access required");
        if (!officeActive) stopMonitoring();
        return false;
      } finally {
        if (refreshPromise === request) refreshPromise = null;
      }
    })();
    refreshPromise = request;
    return request;
  }

  // Drop a task the server has confirmed is gone. The reconciling read below
  // is authoritative, but it can be slow, superseded, or skipped entirely
  // while the monitor is paused — the row must leave every list the moment the
  // delete is acknowledged.
  function dropTask(taskId) {
    const id = safeTaskId(taskId);
    if (!id) return false;
    replyWindows.delete(id);
    historyWindows.delete(id);
    const before = tasks.length;
    tasks = tasks.filter((task) => task.id !== id);
    return tasks.length !== before;
  }

  async function mutate(path, body, taskId = "", options = {}) {
    if (typeof postJSON !== "function") return false;
    const {
      removeOnSuccess = false,
      silent = false,
      ...requestOptions
    } = options;
    lastMutationError = "";
    busyTaskId = safeTaskId(taskId);
    render();
    let errorMessage = "";
    try {
      await postJSON(path, body, { timeout: 10_000, ...requestOptions });
      if (removeOnSuccess) dropTask(taskId);
      await refresh({ quiet: true, force: true });
      return true;
    } catch (error) {
      lastMutationError =
        String(error?.message || "").trim() ||
        "The task change could not be saved.";
      errorMessage = lastMutationError;
      return false;
    } finally {
      busyTaskId = "";
      render();
      if (errorMessage && !silent) report(errorMessage, "error");
    }
  }

  async function onSubmit(event) {
    trackNoticeOrigin(event);
    const replyForm = event.target.closest?.(
      "[data-world-office-task-reply-form]",
    );
    if (replyForm) {
      event.preventDefault();
      const id = safeTaskId(replyForm.dataset.worldOfficeTaskReplyForm);
      const body = text(replyForm.elements?.body?.value, 2000);
      if (!id || !body) return;
      const saved = await mutate(
        `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}/replies`,
        { body },
        id,
      );
      if (saved) {
        replyWindows.delete(id);
        // The comment is now on the timeline too, so a window opened before
        // the post is stale rather than merely short.
        historyWindows.delete(id);
        const mentioned = [...new Set(
          [...body.matchAll(TASK_MENTION_RE)].map((match) =>
            match[2].toLowerCase(),
          ),
        )].filter((name) => name !== actor);
        report(
          mentioned.length
            ? `Comment saved · pinged ${mentioned.map((name) => "@" + name).join(", ")}.`
            : "Private task comment saved.",
        );
      }
      return;
    }
    if (event.target === quickForm) {
      event.preventDefault();
      const title = text(quickTitle?.value);
      const assignment = text(quickAssignee?.value, 80).toLowerCase();
      const department = text(
        quickDepartment?.value || "general",
        64,
      ).toLowerCase();
      const repository = text(quickRepository?.value, 201);
      if (!title) return;
      if (assignment === "agent" && !repository) {
        if (quickStatus) {
          quickStatus.textContent =
            "Choose an owner/repository before assigning agent work.";
        }
        quickRepository?.focus();
        return;
      }
      const body = {
        title,
        department,
        destination: assignment === "agent" ? "agent" : "department",
        assigneeKind:
          assignment === "agent"
            ? "agent"
            : assignment === "unassigned"
              ? "unassigned"
              : "user",
      };
      if (assignment.startsWith("user:")) {
        body.assignee = assignment.slice(5);
      } else if (assignment === "self") {
        body.assignee = actor;
      }
      if (repository) body.repository = repository;
      if (canManage && quickPriority?.value) {
        body.priority = Number(quickPriority.value);
      }
      if (quickStatus) quickStatus.textContent = "Creating task…";
      const saved = await mutate(OFFICE_TASKS_PATH, body, "", { silent: true });
      if (saved) {
        quickForm.reset();
        renderQuickEntry();
        if (quickStatus) {
          quickStatus.textContent =
            assignment === "agent"
              ? "Agent task saved to the organization task list."
              : "Organization task created.";
        }
      } else if (quickStatus) {
        quickStatus.textContent =
          lastMutationError || "Task could not be created.";
      }
      return;
    }
    if (event.target === form) {
      event.preventDefault();
      const title = text(titleInput?.value);
      const assignee = text(assigneeInput?.value, 64).toLowerCase();
      if (!title || !assignee) {
        setStatus("Enter a task and an assignee.", "error");
        return;
      }
      const saved = await mutate(
        MARKETING_TASKS_PATH,
        { title, assignee },
      );
      if (saved) {
        form.reset();
        report("Marketing task added to the Office wall.");
      }
    }
  }

  async function onClick(event) {
    trackNoticeOrigin(event);
    if (event.target.closest("[data-world-task-refresh]")) {
      loading = true;
      renderWorkPane();
      void refresh({ quiet: false, force: true });
      return;
    }
    const checkinOpenButton = event.target.closest(
      "[data-world-office-task-checkin-open]",
    );
    if (checkinOpenButton) {
      const id = safeTaskId(
        checkinOpenButton.dataset.worldOfficeTaskCheckinOpen,
      );
      // The timed prompt already knows how to post a check-in; the button
      // simply lets an assignee raise it on demand instead of waiting for it.
      if (id && !showCheckin(id)) {
        report("Start the task before checking in on it.");
      }
      return;
    }
    const historyButton = event.target.closest(
      "[data-world-office-task-history]",
    );
    if (historyButton) {
      const id = safeTaskId(historyButton.dataset.worldOfficeTaskHistory);
      if (!id) return;
      if (historyWindows.has(id)) {
        historyWindows.delete(id);
        render();
        return;
      }
      try {
        const payload = await fetchJSON(
          `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}/history`,
          { cache: "no-store", timeout: 8000 },
        );
        historyWindows.set(
          id,
          Array.isArray(payload?.history) ? payload.history.slice(-250) : [],
        );
        render();
      } catch (_) {
        report("Task activity could not be loaded.");
      }
      return;
    }
    const repliesButton = event.target.closest(
      "[data-world-office-task-replies]",
    );
    if (repliesButton) {
      const id = safeTaskId(repliesButton.dataset.worldOfficeTaskReplies);
      if (!id) return;
      if (replyWindows.has(id)) {
        replyWindows.delete(id);
        render();
        return;
      }
      try {
        const payload = await fetchJSON(
          `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}/replies`,
          { cache: "no-store", timeout: 8000 },
        );
        const replies = Array.isArray(payload?.task?.replies)
          ? payload.task.replies.slice(-50)
          : [];
        replyWindows.set(id, replies);
        render();
      } catch (_) {
        report("Private task replies could not be loaded.");
      }
      return;
    }
    const pager = event.target.closest("[data-world-task-page]");
    if (pager) {
      const pages = taskPageCount(visibleWorkTasks().length);
      workPage =
        pager.dataset.worldTaskPage === "next"
          ? Math.min(pages - 1, workPage + 1)
          : Math.max(0, workPage - 1);
      renderWorkPane();
      organizationList?.scrollIntoView?.({ block: "nearest" });
      return;
    }
    const quickFilter = event.target.closest("[data-world-task-quick-filter]");
    if (quickFilter) {
      workFilter = [
        "all",
        "active",
        "queued",
        "idle",
      ].includes(quickFilter.dataset.worldTaskQuickFilter)
        ? quickFilter.dataset.worldTaskQuickFilter
        : "all";
      workPage = 0;
      renderWorkPane();
      return;
    }
    if (event.target.closest("[data-world-task-batch-send]")) {
      if (!canManage || typeof postJSON !== "function") return;
      const limit = Math.max(
        1,
        Math.min(50, Math.round(Number(taskBatchSize?.value) || 10)),
      );
      const candidates = visibleWorkTasks()
        .filter(
          (task) =>
            task.status === "idle" &&
            task.assigneeKind === "unassigned" &&
            Boolean(task.repository),
        )
        .slice(0, limit);
      if (!candidates.length) {
        report("No ready unassigned repository tasks match this view.");
        return;
      }
      taskBatchSend.disabled = true;
      let moved = 0;
      try {
        for (const task of candidates) {
          await postJSON(
            `${OFFICE_TASKS_PATH}/${encodeURIComponent(task.id)}`,
            { assigneeKind: "agent" },
            { method: "PATCH", timeout: 10_000 },
          );
          moved += 1;
        }
        await refresh({ quiet: true, force: true });
      } catch (error) {
        report(
          `${moved} task${moved === 1 ? "" : "s"} queued before the batch stopped: ${
            text(error?.message, 120) || "request failed"
          }.`,
        );
        await refresh({ quiet: true, force: true });
        return;
      }
      report(`${moved} task${moved === 1 ? "" : "s"} sent to the Bot queue.`);
      return;
    }
    if (event.target.closest("[data-world-task-sort-direction]")) {
      workSortAscending = !workSortAscending;
      renderWorkPane();
      return;
    }
    const columnSort = event.target.closest("[data-world-task-column-sort]");
    if (columnSort) {
      const nextSort = columnSort.dataset.worldTaskColumnSort;
      if (workSort === nextSort) {
        workSortAscending = !workSortAscending;
      } else {
        workSort = nextSort;
        workSortAscending = true;
      }
      workPage = 0;
      renderWorkPane();
      return;
    }
    if (event.target.closest("[data-world-office-task-close]")) {
      close();
      return;
    }
    if (event.target.closest("[data-world-office-task-checkin-later]")) {
      closeCheckin();
      scheduleCheckin();
      return;
    }
    const checkinButton = event.target.closest(
      "[data-world-office-task-checkin-state]",
    );
    if (checkinButton && checkinTaskId) {
      const state = checkinButton.dataset.worldOfficeTaskCheckinState;
      const id = checkinTaskId;
      const note = text(
        checkin?.querySelector("[data-world-office-task-checkin-note]")?.value,
        500,
      );
      closeCheckin();
      const saved = await mutate(
        `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}/checkin`,
        note ? { state, note } : { state },
        id,
      );
      if (saved) historyWindows.delete(id);
      const mentioned = [...new Set(
        [...note.matchAll(TASK_MENTION_RE)].map((match) =>
          match[2].toLowerCase(),
        ),
      )].filter((name) => name !== actor);
      report(
        !saved
          ? "That check-in could not be recorded."
          : mentioned.length
            ? `Check-in sent · pinged ${mentioned.map((name) => "@" + name).join(", ")}.`
            : "Check-in sent to your team.",
      );
      scheduleCheckin();
      return;
    }
    const actionButton = event.target.closest(
      "[data-world-office-task-action]",
    );
    if (!actionButton) return;
    event.preventDefault();
    event.stopPropagation();
    const action = actionButton.dataset.worldOfficeTaskAction;
    const id = safeTaskId(actionButton.dataset.worldOfficeTaskId);
    if (
      !id ||
      ![
        "start", "stop", "complete", "delete", "return", "follow-up",
        "undo", "qa-pass", "qa-fail", "qa-unsure", "remind",
      ].includes(action)
    ) {
      return;
    }
    if (action.startsWith("qa-")) {
      const verdict = action.slice(3);
      const task = tasks.find((item) => item.id === id);
      if (!task || task.status !== "done") return;
      busyTaskId = id;
      render();
      let saved = false;
      try {
        saved = await onQaVerdict({ task, verdict });
        if (saved) await refresh({ quiet: true, force: true });
      } finally {
        busyTaskId = "";
        render();
      }
      if (saved) report(`QA verdict saved: ${verdict}.`);
      return;
    }
    if (action === "follow-up") {
      const parent = tasks.find((task) => task.id === id);
      if (!parent) return;
      const entered = window.prompt(`Follow-up to “${parent.title}”:`, "");
      const title = text(entered, 160);
      if (!title) return;
      const saved = await mutate(
        OFFICE_TASKS_PATH,
        { title, parentTaskId: parent.id },
        parent.id,
      );
      if (saved) report("Follow-up task created with the same assignee.");
      return;
    }
    if (action === "remind") {
      const task = tasks.find((item) => item.id === id);
      if (!task?.assignee) return;
      const entered = window.prompt(
        `Remind @${task.assignee} to check in on “${task.title}”. Optional note:`,
        "",
      );
      if (entered === null) return;
      const note = text(entered, 500);
      const saved = await mutate(
        `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}/remind`,
        note ? { note } : {},
        id,
      );
      if (!saved) return;
      const reminded = tasks.find((item) => item.id === id);
      if (reminded?.lastReminder?.delivered === false) {
        report(`The reminder was saved, but @${task.assignee} could not be pinged.`);
      } else {
        report(`Reminder sent. @${task.assignee} was pinged to check in.`);
      }
      return;
    }
    const completionNote =
      action === "complete"
        ? text(
            actionButton
              .closest(".world-office-task")
              ?.querySelector("[data-world-office-task-completion-note]")
              ?.value,
            4000,
          )
        : "";
    const saved = await mutate(
      action === "delete"
        ? `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}`
        : `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}/${action}`,
      action === "complete" ? { completionNote } : {},
      id,
      action === "delete"
        ? { method: "DELETE", removeOnSuccess: true }
        : {},
    );
    if (saved) {
      report(
        action === "start"
          ? "Task timer started."
          : action === "stop"
            ? "Task timer stopped."
            : action === "complete"
              ? "Task marked done and ready for QA."
              : action === "undo"
                ? "Task marked undone; its prior routing is active again."
              : action === "return"
                ? "Task returned to the task list."
                : "Task deleted.",
      );
    }
  }

  function onWorkControl(event) {
    trackNoticeOrigin(event);
    if (event.target === taskSearch) {
      workSearch = text(taskSearch.value, 160);
      workPage = 0;
      renderWorkPane();
      taskSearch.focus();
      return;
    }
    if (event.target === taskFilter) {
      const selectedFilter = text(taskFilter.value, 80).toLowerCase();
      const memberFilter =
        selectedFilter.startsWith("user:") &&
        organizationMembers.includes(selectedFilter.slice(5));
      workFilter = memberFilter || [
        "all",
        "active",
        "idle",
        "done",
        "mine",
        "agent",
        "queued",
      ].includes(selectedFilter)
        ? selectedFilter
        : "all";
      workPage = 0;
      renderWorkPane();
      return;
    }
    if (event.target === taskSort) {
      workSort = [
        "priority",
        "status",
        "updated",
        "assignee",
        "department",
        "title",
      ].includes(taskSort.value)
        ? taskSort.value
        : "priority";
      workSortAscending = workSort !== "updated";
      workPage = 0;
      renderWorkPane();
      return;
    }
    const priorityInput = event.target.closest("[data-world-task-priority]");
    if (priorityInput && event.type === "change") {
      const id = safeTaskId(priorityInput.dataset.worldTaskPriority);
      const priority = Math.max(
        1,
        Math.min(99, Math.round(Number(priorityInput.value) || 50)),
      );
      void mutate(
        `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}`,
        { priority },
        id,
        { method: "PATCH" },
      );
      return;
    }
    const assigneeSelect = event.target.closest("[data-world-task-assignee]");
    if (assigneeSelect && event.type === "change") {
      const id = safeTaskId(assigneeSelect.dataset.worldTaskAssignee);
      const value = text(assigneeSelect.value, 80).toLowerCase();
      const body = value.startsWith("user:")
        ? { assigneeKind: "user", assignee: value.slice(5) }
        : { assigneeKind: value };
      void mutate(
        `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}`,
        body,
        id,
        { method: "PATCH" },
      );
    }
  }

  async function physicalAction(payload = {}) {
    trackNoticeOrigin(null);
    const action = text(payload?.action, 24).toLowerCase();
    if (action === "initiative") {
      const href = safeIssueHref(payload?.href);
      if (!href || !initiatives.some((item) => item.href === href)) {
        report("That Marketing initiative is no longer available.");
        return false;
      }
      window.open(href, "_blank", "noopener,noreferrer");
      return true;
    }
    if (action === "proof") {
      const member = text(payload?.member, 64).toLowerCase();
      if (!marketingMembers.includes(actor) || member !== actor) {
        report("Only a Marketing member can add proof to their own desk.");
        return false;
      }
      const entered = window.prompt(
        "Paste the public HTTPS social post link:",
        "",
      );
      const url = String(entered || "").trim().slice(0, 500);
      if (!url) return false;
      const label = text(
        window.prompt("Short label for this post (optional):", "") || "",
        120,
      );
      const saved = await mutate(
        `${MARKETING_TASKS_PATH}/proofs`,
        { url, label },
      );
      if (saved) report("Proof of work added to your Marketing desk.");
      return saved;
    }
    if (action === "create") {
      if (!canManage) {
        report("Only an organization manager can create Marketing tasks.");
        return false;
      }
      const assignee = text(payload?.assignee, 64).toLowerCase();
      if (!assignable.includes(assignee)) {
        report("Choose a current Marketing team member.");
        return false;
      }
      const entered = window.prompt(`New task for @${assignee}:`, "");
      const title = text(entered);
      if (!title) return false;
      const saved = await mutate(
        MARKETING_TASKS_PATH,
        { title, assignee },
      );
      if (saved) report("Marketing task added to the physical wall.");
      return saved;
    }
    const id = safeTaskId(payload?.id);
    const task = tasks.find((item) => item.id === id);
    if (
      !task ||
      !["start", "stop", "complete", "undo", "delete"].includes(action)
    ) {
      return false;
    }
    if (
      ["start", "stop"].includes(action) &&
      (task.assignee !== actor || task.status === "done")
    ) {
      report("Only the assignee can run this timer.");
      return false;
    }
    if (
      action === "complete" &&
      task.assignee !== actor &&
      !canManage
    ) {
      report("Only the assignee or an organization manager can finish it.");
      return false;
    }
    if (
      action === "undo" &&
      (task.status !== "done" ||
        (!canManage &&
          task.assignee !== actor &&
          task.createdBy !== actor &&
          task.qa.reviewer !== actor))
    ) {
      report("Only the task owner, assignee, reviewer, or manager can undo it.");
      return false;
    }
    if (action === "delete" && !canManage) return false;
    let completionNote = "";
    if (action === "complete") {
      const entered = window.prompt(
        "What did you do to complete this task? This note will remain with the task for QA.",
        task.completionNote || "",
      );
      if (entered === null) return false;
      completionNote = text(entered, 4000);
    }
    const saved = await mutate(
      action === "delete"
        ? `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}`
        : `${OFFICE_TASKS_PATH}/${encodeURIComponent(id)}/${action}`,
      action === "complete" ? { completionNote } : {},
      id,
      action === "delete"
        ? { method: "DELETE", removeOnSuccess: true }
        : {},
    );
    if (saved) {
      report(
        action === "start"
          ? "Task timer started."
          : action === "stop"
            ? "Task timer stopped."
            : action === "complete"
              ? "Task marked done."
              : action === "undo"
                ? "Task marked undone; its prior routing is active again."
              : "Task deleted.",
      );
    }
    return saved;
  }

  function open() {
    if (!officeActive) {
      report("Enter the ForkMesh Office to use the marketing task board.");
      return false;
    }
    opened = true;
    render();
    if (heading) window.requestAnimationFrame(() => heading.focus());
    if (Date.now() - lastRefreshAt > 5000) {
      void refresh({ quiet: tasks.length > 0 });
    }
    return true;
  }

  function close() {
    opened = false;
    render();
    return true;
  }

  function stopMonitoring() {
    monitoring = false;
    closeCheckin();
    clearCheckinTimer();
    if (pollTimer) window.clearTimeout(pollTimer);
    pollTimer = 0;
    if (tickTimer) window.clearInterval(tickTimer);
    tickTimer = 0;
  }

  // The Local controls "Work" tab is reachable anywhere in the World, so it
  // drives the same monitoring loop the Office board uses.
  function setPersonalView(open) {
    personalView = open === true;
    if (!personalView) {
      if (!keepMonitoring()) stopMonitoring();
      return false;
    }
    monitoring = true;
    if (!tickTimer) {
      tickTimer = window.setInterval(updateElapsedLabels, OFFICE_TASKS_TICK_MS);
    }
    void refresh({ quiet: tasks.length > 0 });
    schedulePoll();
    return true;
  }

  // Populate the private avatar card once at World startup. An idle task list
  // stops immediately after this request; only an actually running timer keeps
  // the existing low-frequency reconciliation loop alive.
  function prime() {
    if (!getSession()?.sessionToken) {
      selfWorkState("locked", "Sign in to load assigned work");
      return false;
    }
    monitoring = true;
    if (!tickTimer) {
      tickTimer = window.setInterval(updateElapsedLabels, OFFICE_TASKS_TICK_MS);
    }
    void refresh({ quiet: false });
    schedulePoll();
    return true;
  }

  function refreshNow() {
    if (!getSession()?.sessionToken) return Promise.resolve(false);
    monitoring = true;
    if (!tickTimer) {
      tickTimer = window.setInterval(updateElapsedLabels, OFFICE_TASKS_TICK_MS);
    }
    // Callers use this immediately after creating or changing a task, so it
    // must read fresh state rather than join a poll that started earlier.
    const result = refresh({ quiet: tasks.length > 0, force: true });
    schedulePoll();
    return result;
  }

  function setRecentIssues(items = []) {
    const seen = new Set();
    recentIssues = (Array.isArray(items) ? items : [])
      .map(normalizedRecentIssue)
      .filter((issue) => {
        if (!issue || seen.has(issue.id)) return false;
        seen.add(issue.id);
        return true;
      })
      .slice(0, 6);
    renderWorkPane();
    selfWorkState(
      authorized || recentIssues.length ? "ready" : "locked",
      authorized ? "" : "Sign in to load assigned work",
    );
    return recentIssues.map((issue) => ({ ...issue }));
  }

  function setActive(next) {
    officeActive = next === true;
    if (!officeActive) {
      close();
      // An assignee's active timer survives leaving the Office. Keep only its
      // low-frequency HTTPS poll and private check-in prompt alive; no task
      // data enters the Town presence socket.
      if (!keepMonitoring()) stopMonitoring();
      return;
    }
    monitoring = true;
    physicalTickBucket = -1;
    if (!tickTimer) {
      tickTimer = window.setInterval(updateElapsedLabels, OFFICE_TASKS_TICK_MS);
    }
    void refresh();
    schedulePoll();
  }

  function stopActiveForDeparture() {
    if (!ownActiveTasks().length) return false;
    const sessionToken = String(getSession()?.sessionToken || "");
    if (!sessionToken) return false;
    // Leaving and re-entering the Office fires this again each time, so a
    // failing stop is backed off rather than re-posted on every departure.
    const key = "POST:/api/tasks/stop-active";
    if (departureBackoff.waitMs(key) > 0) return false;
    try {
      // `keepalive` lets this bounded, same-origin server-time stop finish
      // while pagehide tears the World down. The token stays in the request
      // header and no task copy enters the URL, beacon body, or sockets.
      void fetch(`${OFFICE_TASKS_PATH}/stop-active`, {
        method: "POST",
        credentials: "same-origin",
        cache: "no-store",
        keepalive: true,
        headers: {
          accept: "application/json",
          authorization: `Bearer ${sessionToken}`,
          "content-type": "application/json",
        },
        body: "{}",
      })
        .then((response) => {
          if (response.ok) {
            departureBackoff.succeed(key);
            return;
          }
          departureBackoff.fail(
            key,
            markWorldHTTPFailure(
              new Error(`stop-active returned ${response.status}`),
              response,
            ),
          );
        })
        .catch(() => departureBackoff.fail(key, {}));
      return true;
    } catch (_) {
      return false;
    }
  }

  function destroy() {
    stopActiveForDeparture();
    officeActive = false;
    personalView = false;
    stopMonitoring();
    document.removeEventListener("visibilitychange", onVisibilityChange);
    root.removeEventListener("click", onClick);
    root.removeEventListener("submit", onSubmit);
    root.removeEventListener("input", onWorkControl);
    root.removeEventListener("change", onWorkControl);
    world.updateOfficeMarketingTasks?.({
      authorized: false,
      state: "locked",
      message: "Organization access required",
      tasks: [],
    });
    recentIssues = [];
    selfWorkState("locked", "");
  }

  document.addEventListener("visibilitychange", onVisibilityChange);
  root.addEventListener("click", onClick);
  root.addEventListener("submit", onSubmit);
  root.addEventListener("input", onWorkControl);
  root.addEventListener("change", onWorkControl);
  physicalState("locked", "Enter Office to sync tasks");

  return {
    close,
    destroy,
    open,
    physicalAction,
    prime,
    refresh,
    refreshNow,
    setActive,
    setPersonalView,
    setRecentIssues,
    showCheckin,
    stopActiveForDeparture,
  };
}
