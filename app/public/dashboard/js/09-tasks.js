  // ---------------------------------------------------------------------------
  // Organization tasks. The dashboard uses the same encrypted catalog and
  // lifecycle endpoints as desktop and World; authorization always remains on
  // the server. Filtering and paging stay local because the authorized list is
  // returned as one bounded catalog snapshot.
  const TASKS_PAGE_SIZE = 100;
  const TASK_IMAGE_TYPES = new Set([
    "image/gif", "image/jpeg", "image/png", "image/webp",
  ]);
  const taskAvatarProfiles = new Map();
  const taskAttachmentUrls = new Map();
  const taskDetailsLoaded = new Set();
  let taskDetailNotice = null;
  let taskListNotice = null;
  // Comments and check-in notes being written, keyed by task, so a repaint of
  // the detail pane (a catalog refresh, another member's change) cannot eat an
  // unposted draft.
  const taskCommentDrafts = new Map();
  const taskCheckinNotes = new Map();
  const taskReminderNotes = new Map();
  const taskProgressDrafts = new Map();
  // Tasks whose activity was re-read because a catalog refresh replaced the
  // detail copy; recorded so a relay that returns no history is asked once.
  const taskHistoryRequested = new Set();
  let taskCheckinOpenId = "";
  let taskCheckinBusy = false;
  let taskRemindOpenId = "";
  let taskRemindBusy = false;
  let taskPersistentReminderBusy = false;
  let taskActionMenuOpenId = "";
  const taskCollapsedGroups = new Set();
  // query null = the caret is not inside an @token, so no suggestions are open.
  const taskMentionState = { query: null, index: 0, matches: [] };

  function taskAssigneeKind(task) {
    const kind = String(task?.assigneeKind || "").trim().toLowerCase();
    if (["agent", "bot", "claude", "codex"].includes(kind)) return "agent";
    return kind === "unassigned" ? "unassigned" : "user";
  }

  function taskIsDone(task) {
    return task.completedAt > 0 || task.status === "done";
  }

  function taskStatusKey(task) {
    if (taskIsDone(task)) return "done";
    if (task?.status === "active") return "active";
    if (taskAssigneeKind(task) === "agent" && task?.agentSessionId) return "queued";
    return "ready";
  }

  function taskStatusLabel(task) {
    return { done: "Done", active: "In progress", queued: "Queued", ready: "Ready" }[taskStatusKey(task)];
  }

  function taskStatusIcon(task) {
    return { done: "check-circle-2", active: "loader-circle", queued: "bot", ready: "circle-dot" }[taskStatusKey(task)];
  }

  function taskProgress(task) {
    if (taskIsDone(task)) return 100;
    const value = Math.round(Number(task?.progress));
    return Number.isFinite(value) ? Math.max(0, Math.min(100, value)) : 0;
  }

  function taskCanEditProgress(task) {
    const actor = state.tasksView.actor;
    if (!task || taskIsDone(task)) return false;
    if (state.tasksView.canManage) return true;
    if (!actor) return false;
    return task.createdBy === actor || (taskAssigneeKind(task) === "user" && task.assignee === actor);
  }

  function taskProgressBarHtml(task, label) {
    const value = taskProgress(task);
    return `<span class="task-progress" data-done="${taskIsDone(task)}"><span class="task-progress-track" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${value}" aria-valuetext="${value}% complete" aria-label="${escapeHtml(label)}"><span class="task-progress-fill" style="width:${value}%"></span></span><span class="task-progress-value" aria-hidden="true">${value}%</span></span>`;
  }

  function taskProgressCardHtml(task) {
    const bar = taskProgressBarHtml(task, "Task progress");
    if (!taskCanEditProgress(task)) return taskPropsGroupHtml("Progress", bar);
    const value = taskProgressDrafts.has(task.id) ? taskProgressDrafts.get(task.id) : taskProgress(task);
    const editor = `<form class="task-progress-editor" data-task-progress-form="${escapeHtml(task.id)}"><input type="range" min="0" max="100" step="1" value="${value}" data-task-progress-range aria-label="Set progress"><input type="number" min="0" max="100" step="1" inputmode="numeric" value="${value}" class="task-input" data-task-progress-number aria-label="Progress percent"><button type="submit" class="task-action-button" data-tone="primary">Save</button></form>`;
    return taskPropsGroupHtml("Progress", bar + editor + taskNoticeHtml(task, "progress"));
  }

  const TASK_PERSISTENT_REMINDER_CHOICES = [[15, "15 minutes"], [30, "30 minutes"], [60, "1 hour"], [240, "4 hours"], [1440, "1 day"], [10080, "1 week"]];

  function taskPersistentReminderHtml(task) {
    if (task.deletedAt) return "";
    const reminder = task.persistentReminder || {};
    const enabled = Boolean(reminder.enabled);
    const every = Number(reminder.everyMinutes) || 60;
    const who = taskAssigneeKind(task) === "user" && task.assignee ? `@${task.assignee}` : (task.createdBy ? `@${task.createdBy}` : "the creator");
    const status = enabled
      ? (taskIsDone(task) ? "Paused while the task is done." : `Pings ${escapeHtml(who)} until the task is done${reminder.nextAt ? ` · next ${escapeHtml(taskDate(reminder.nextAt))}` : ""}.`)
      : "Off. Turn it on to keep pinging until the task is done.";
    if (!taskCanEditProgress(task)) return taskPropsGroupHtml("Persistent reminder", `<p class="task-switch-row">${enabled ? "On" : "Off"}</p><p class="task-props-note">${status}</p>`);
    const options = TASK_PERSISTENT_REMINDER_CHOICES.map(([minutes, label]) => `<option value="${minutes}"${minutes === every ? " selected" : ""}>Every ${label}</option>`).join("");
    return taskPropsGroupHtml("Persistent reminder", `<label class="task-switch-row"><span>Keep reminding</span><input type="checkbox" class="task-switch" data-task-persistent-reminder${enabled ? " checked" : ""}${taskPersistentReminderBusy ? " disabled" : ""}></label><select class="task-input" data-task-persistent-reminder-every aria-label="Reminder interval"${taskPersistentReminderBusy || !enabled ? " disabled" : ""}>${options}</select><p class="task-props-note">${status}</p>${taskNoticeHtml(task, "persistent")}`);
  }

  function taskAssigneeLabel(task) {
    const kind = taskAssigneeKind(task);
    if (kind === "agent") return "Bot";
    if (kind === "unassigned") return "Unassigned";
    return String(task?.assignee || "Unassigned");
  }

  function validTaskAvatarPng(value) {
    const png = String(value || "").trim();
    return png.length <= 90_000 && /^[A-Za-z0-9+/]+={0,2}$/.test(png) ? png : "";
  }

  function taskAvatarHtml(task, size = "task-avatar") {
    const kind = taskAssigneeKind(task);
    if (kind === "agent") {
      return `<span class="${size}" data-kind="agent" title="Bot assignee" aria-label="Bot assignee avatar"><i data-lucide="bot" class="h-3 w-3"></i></span>`;
    }
    if (kind === "unassigned") {
      return `<span class="${size}" data-kind="unassigned" title="Unassigned" aria-label="Unassigned">?</span>`;
    }
    const name = String(task?.assignee || "").trim().toLowerCase();
    const initial = Array.from(name)[0]?.toUpperCase() || "?";
    return `<span class="${size}" data-task-assignee-avatar="${escapeHtml(name)}" title="@${escapeHtml(name || "member")}" aria-label="@${escapeHtml(name || "member")} avatar">${escapeHtml(initial)}</span>`;
  }

  function taskAvatarForName(name) {
    const account = String(name || "").trim().toLowerCase();
    if (!account) return Promise.resolve("");
    const sessionName = String(state.session?.nodeName || state.session?.email || "").trim().toLowerCase();
    const ownAvatar = validTaskAvatarPng(state.session?.avatarPng);
    if (account === sessionName && ownAvatar) return Promise.resolve(ownAvatar);
    let pending = taskAvatarProfiles.get(account);
    if (!pending) {
      pending = fetchJson(`/api/accounts/${encodeURIComponent(account)}`)
        .then((profile) => {
          if (profile?.exists !== true || profile?.profilePrivate === true) return "";
          return validTaskAvatarPng(profile.avatarPng);
        })
        .catch(() => "");
      taskAvatarProfiles.set(account, pending);
    }
    return pending;
  }

  function hydrateTaskAvatars() {
    document.querySelectorAll("[data-task-assignee-avatar]").forEach((avatar) => {
      const name = String(avatar.dataset.taskAssigneeAvatar || "").trim().toLowerCase();
      if (!name || avatar.dataset.taskAvatarLoading || avatar.dataset.taskAvatarHydrated) return;
      avatar.dataset.taskAvatarLoading = "true";
      void taskAvatarForName(name).then((png) => {
        delete avatar.dataset.taskAvatarLoading;
        if (!avatar.isConnected || !png) return;
        const image = document.createElement("img");
        image.src = `data:image/png;base64,${png}`;
        image.alt = "";
        image.className = "h-full w-full object-cover";
        image.decoding = "async";
        image.addEventListener("error", () => image.remove(), { once: true });
        avatar.replaceChildren(image);
        avatar.dataset.taskAvatarHydrated = "true";
      });
    });
  }

  function taskDate(value, fallback = "—") {
    const milliseconds = Number(value || 0);
    if (!milliseconds) return fallback;
    const date = new Date(milliseconds);
    return Number.isNaN(date.getTime()) ? fallback : date.toLocaleString([], {
      year: "numeric", month: "short", day: "numeric", hour: "numeric", minute: "2-digit",
    });
  }

  function taskElapsed(milliseconds) {
    let seconds = Math.max(0, Math.floor(Number(milliseconds || 0) / 1000));
    const days = Math.floor(seconds / 86400); seconds %= 86400;
    const hours = Math.floor(seconds / 3600); seconds %= 3600;
    const minutes = Math.floor(seconds / 60);
    if (days) return `${days}d ${hours}h`;
    if (hours) return `${hours}h ${minutes}m`;
    return `${minutes}m`;
  }

  function taskFileSize(bytes) {
    const size = Math.max(0, Number(bytes || 0));
    return size >= 1024 * 1024 ? `${(size / 1024 / 1024).toFixed(1)} MiB`
      : size >= 1024 ? `${Math.round(size / 1024)} KiB` : `${size} B`;
  }

  function taskFlash(node, text) {
    if (!node) return;
    node.replaceChildren();
    if (text) {
      const icon = document.createElement("i");
      icon.setAttribute("data-lucide", node.dataset.flashIcon || "circle-alert");
      icon.setAttribute("aria-hidden", "true");
      const body = document.createElement("span");
      body.className = "fm-flash-body";
      body.textContent = text;
      node.append(icon, body);
      window.lucide?.createIcons();
    }
    node.hidden = !text;
  }

  function taskFriendlyError(error) {
    const raw = String(error?.message || error || "Task request failed");
    const messages = {
      active_task_exists: "You already have another task in progress. Stop it before starting this one.",
      active_task_cannot_be_updated: "Stop this task before editing it.",
      assignee_not_team_member: "That assignee is not a member of the selected team.",
      assignee_or_manager_only: "Only the assignee or an organization manager can complete this task.",
      completed_task_cannot_be_updated: "Reopen this task before editing it.",
      cannot_remind_yourself: "This task is assigned to you. Check in on it instead.",
      reminder_too_soon: "The assignee was reminded less than 10 minutes ago. Try again later.",
      task_has_no_assignee: "Only a task assigned to a person can be sent a reminder.",
      invalid_repository: "Use a repository in owner/name format.",
      repository_required: "A repository is required for bot work.",
      task_image_too_large: "Each task image must be 256 KiB or smaller.",
      task_images_too_large: "Task images must total 1 MiB or less.",
      too_many_task_images: "A task can include up to four images.",
      task_delete_blocked: "This task is busy. Only an organization owner can move it to trash anyway.",
      force_requires_owner: "Only an organization owner can move a busy task to trash.",
      forbidden: "Only the task's creator, its assignee or an organization manager can do that.",
      task_trash_expired: `This task was deleted more than ${TASK_TRASH_DAYS} days ago and is being purged.`,
      task_not_deleted: "This task is no longer in the trash.",
      task_delete_batch_refused: "Nothing was moved to trash: some tasks are busy or not yours to delete.",
    };
    return messages[raw] || raw.replaceAll("_", " ").replace(/^./, (letter) => letter.toUpperCase());
  }

  async function taskRequest(method, path, body = null) {
    const token = state.session?.sessionToken || "";
    const headers = { accept: "application/json" };
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, {
      method,
      cache: "no-store",
      credentials: "same-origin",
      headers,
      body: body === null ? undefined : JSON.stringify(body),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.payload = payload;
      throw error;
    }
    return payload;
  }

  function taskNoticeHtml(task, place) {
    const notice = taskDetailNotice && taskDetailNotice.taskId === task.id && taskDetailNotice.place === place ? taskDetailNotice : null;
    return `<p data-task-notice="${place}" class="task-notice" role="status"${notice ? ` data-tone="${notice.tone}"` : " hidden"}>${notice ? escapeHtml(notice.message) : ""}</p>`;
  }

  function setTaskNotice(taskId, place, message, tone = "good") {
    taskDetailNotice = { taskId, place, message, tone };
    const detail = $("[data-tasks-detail]");
    if (!detail) return;
    const current = detail.querySelector("[data-task-detail-id]")?.dataset.taskDetailId || "";
    for (const node of detail.querySelectorAll("[data-task-notice]")) {
      const shown = current === taskId && node.dataset.taskNotice === place;
      node.hidden = !shown;
      node.textContent = shown ? message : "";
      if (shown) node.dataset.tone = tone;
    }
  }

  function renderTaskListNotice() {
    const node = $("[data-tasks-notice]");
    if (!node) return;
    const notice = taskListNotice;
    node.hidden = !notice;
    if (!notice) { node.innerHTML = ""; return; }
    node.dataset.tone = notice.tone;
    node.innerHTML = `<span>${escapeHtml(notice.message)}</span>${notice.undoIds?.length ? ` <button type="button" data-tasks-notice-undo="${escapeHtml(notice.undoIds.join(","))}" class="task-notice-undo">Undo</button>` : ""}`;
  }

  function setTaskListNotice(message, tone = "good", undoIds = []) {
    taskListNotice = message ? { message, tone, undoIds } : null;
    renderTaskListNotice();
  }

  const TASK_TRASH_DAYS = 30;
  const TASK_DELETE_BATCH = 50;
  const TASK_BLOCKER_COPY = {
    agent_running: "an agent job is running on it",
    qa_in_progress: "it is waiting for QA review",
  };

  function taskBlockedReason(error) {
    const payload = error?.payload || {};
    const failures = Array.isArray(payload.failures) ? payload.failures : [];
    const reasons = [
      ...(Array.isArray(payload.blockedBy) ? payload.blockedBy : []),
      ...failures.flatMap((item) => (Array.isArray(item?.blockedBy) ? item.blockedBy : [])),
    ];
    return [...new Set(reasons)].map((key) => TASK_BLOCKER_COPY[key] || String(key).replaceAll("_", " ")).join(" and ");
  }

  function taskClearable(task) {
    return taskIsDone(task) && !(Number(task.qa?.requestedAt || 0) > 0 && !Number(task.qa?.reviewedAt || 0));
  }

  async function restoreTasks(taskIds) {
    let restored = 0;
    for (const taskId of taskIds) {
      const payload = await taskRequest("POST", `/api/tasks/${taskId}/restore`, {});
      state.tasksView.trash = state.tasksView.trash.filter((task) => task.id !== taskId);
      if (payload.task) replaceTask(payload.task, false);
      restored += 1;
    }
    return restored;
  }

  async function undoTaskDelete(taskIds) {
    setTaskListNotice("Restoring…");
    try {
      const restored = await restoreTasks(taskIds);
      setTaskListNotice(restored === 1 ? "Task restored." : `Restored ${restored} tasks.`);
      renderTasksPage();
    } catch (error) {
      setTaskListNotice(taskFriendlyError(error), "bad");
      void loadOrganizationTasks({ quiet: true });
    }
  }

  async function restoreTrashedTask(task) {
    if (!task || state.tasksView.busyId) return;
    state.tasksView.busyId = task.id;
    renderTaskDetail();
    try {
      await restoreTasks([task.id]);
      if (state.tasksView.selectedId === task.id) state.tasksView.selectedId = "";
      setTaskListNotice("Task restored to the board.");
    } catch (error) {
      setTaskNotice(task.id, "actions", taskFriendlyError(error), "bad");
    } finally {
      state.tasksView.busyId = "";
      renderTasksPage();
    }
  }

  async function loadTaskTrash() {
    if (state.tasksView.trashLoading) return;
    state.tasksView.trashLoading = true;
    try {
      const tasks = [];
      let offset = 0;
      for (;;) {
        const page = await taskRequest("GET", `/api/tasks?status=deleted${offset ? `&offset=${offset}` : ""}`);
        tasks.push(...(Array.isArray(page?.tasks) ? page.tasks : []));
        const next = Number(page?.nextOffset);
        if (page?.hasMore !== true || !Number.isInteger(next) || next <= offset) break;
        offset = next;
      }
      state.tasksView.trash = tasks.filter((task) => task && typeof task === "object");
    } catch (error) {
      state.tasksView.trash = [];
      setTaskListNotice(`Could not load the trash: ${taskFriendlyError(error)}`, "bad");
    } finally {
      state.tasksView.trashLoaded = true;
      state.tasksView.trashLoading = false;
      renderTasksPage();
    }
  }

  function taskMatchesFilter(task, filter) {
    const done = taskIsDone(task);
    if (filter === "open") return !done;
    if (filter === "done") return done;
    if (filter === "active") return task.status === "active";
    if (filter === "qa") return Number(task.qa?.requestedAt || 0) > 0 && task.qa?.status === "unknown";
    if (filter === "mine") return !done && taskAssigneeKind(task) === "user" && task.assignee === state.tasksView.actor;
    return true;
  }

  function taskMatchesAssignee(task, filter) {
    if (!filter || filter === "all") return true;
    const kind = taskAssigneeKind(task);
    if (filter === "agent" || filter === "unassigned") return kind === filter;
    if (!filter.startsWith("user:")) return true;
    return kind === "user" && task.assignee === filter.slice(5);
  }

  function taskStateCounts() {
    const result = { open: 0, closed: 0, active: 0, queued: 0, qa: 0 };
    for (const task of state.tasksView.items) {
      if (taskIsDone(task)) result.closed += 1; else result.open += 1;
      if (task.status === "active") result.active += 1;
      if (taskStatusKey(task) === "queued") result.queued += 1;
      if (Number(task.qa?.requestedAt || 0) > 0 && task.qa?.status === "unknown") result.qa += 1;
    }
    return result;
  }

  function taskSearchQuery() {
    return String(state.tasksView.query || "").trim().toLowerCase();
  }

  function taskMatchesQuery(task, query) {
    if (!query) return true;
    const qa = task.qa || {};
    const agent = task.agent || {};
    return [
      task.title, task.details, task.repository, task.department, task.team,
      task.destination, taskAssigneeLabel(task), task.createdBy, task.id,
      task.completionNote, qa.status, qa.reviewer, qa.howToTest,
      agent.provider, agent.model, agent.mode, agent.strength,
      ...taskWorkItems(task).flatMap((item) => [item.branch, item.repository, item.node]),
    ].some((value) => String(value || "").toLowerCase().includes(query));
  }

  function filteredTasks() {
    const query = taskSearchQuery();
    const trash = state.tasksView.filter === "trash";
    return (trash ? state.tasksView.trash : state.tasksView.items).filter((task) => {
      if (!trash && !taskMatchesFilter(task, state.tasksView.filter)) return false;
      if (!taskMatchesAssignee(task, state.tasksView.assignee)) return false;
      return taskMatchesQuery(task, query);
    });
  }

  const TASK_FILTER_KEYS = ["open", "active", "mine", "qa", "done", "all"];

  function taskFilterCounts() {
    const query = taskSearchQuery();
    const assignee = state.tasksView.assignee;
    const result = Object.fromEntries(TASK_FILTER_KEYS.map((key) => [key, 0]));
    for (const task of state.tasksView.items) {
      if (!taskMatchesQuery(task, query)) continue;
      if (taskMatchesFilter(task, "mine")) result.mine += 1;
      if (!taskMatchesAssignee(task, assignee)) continue;
      for (const key of TASK_FILTER_KEYS) {
        if (key !== "mine" && taskMatchesFilter(task, key)) result[key] += 1;
      }
    }
    result.trash = state.tasksView.trashLoaded
      ? state.tasksView.trash.filter((task) => taskMatchesAssignee(task, assignee) && taskMatchesQuery(task, query)).length
      : null;
    return result;
  }

  function taskPeopleChoices() {
    const actor = state.tasksView.actor;
    const members = [...new Set(state.tasksView.members.map((name) => String(name || "").trim()).filter(Boolean))];
    members.sort((a, b) => (b === actor) - (a === actor) || a.localeCompare(b));
    return [
      { value: "all", label: "Everyone" },
      ...members.map((name) => ({ value: `user:${name}`, label: name === actor ? `@${name} (you)` : `@${name}`, name })),
      { value: "agent", label: "Bot" },
      { value: "unassigned", label: "Unassigned" },
    ];
  }

  function taskPeopleOpenCounts() {
    const counts = new Map();
    for (const task of state.tasksView.items) {
      if (taskIsDone(task)) continue;
      const kind = taskAssigneeKind(task);
      const key = kind === "user" ? `user:${task.assignee}` : kind;
      counts.set(key, (counts.get(key) || 0) + 1);
      counts.set("all", (counts.get("all") || 0) + 1);
    }
    return counts;
  }

  function taskPersonAvatarHtml(choice) {
    if (choice.value === "all") return '<span class="task-person-avatar"><i data-lucide="users" class="h-3.5 w-3.5"></i></span>';
    if (choice.value === "agent") return taskAvatarHtml({ assigneeKind: "agent" }, "task-person-avatar");
    if (choice.value === "unassigned") return taskAvatarHtml({ assigneeKind: "unassigned" }, "task-person-avatar");
    return taskAvatarHtml({ assigneeKind: "user", assignee: choice.name }, "task-person-avatar");
  }

  function renderTaskPeople() {
    const strip = $("[data-tasks-people]");
    if (!strip) return;
    const choices = taskPeopleChoices();
    if (!choices.some(({ value }) => value === state.tasksView.assignee)) state.tasksView.assignee = "all";
    const counts = taskPeopleOpenCounts();
    strip.innerHTML = choices.map((choice) => {
      const open = counts.get(choice.value) || 0;
      const active = choice.value === state.tasksView.assignee;
      const tip = `${choice.label}: ${formatCount(open)} open`;
      return `<button type="button" class="task-person" data-tasks-person="${escapeHtml(choice.value)}" aria-pressed="${active}" title="${escapeHtml(tip)}" aria-label="${escapeHtml(tip)}">${taskPersonAvatarHtml(choice)}<span class="task-person-count">${escapeHtml(formatCount(open))}</span></button>`;
    }).join("");
  }

  const TASK_GROUPS = [["active", "In progress"], ["queued", "Bot queue"], ["ready", "Ready"], ["done", "Done"]];
  const TASK_PRIORITY_LABELS = { urgent: "Urgent", high: "High", medium: "Medium", low: "Low" };

  function taskRingSvg(key, value) {
    if (key === "done") return '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="7" fill="currentColor"/><path d="M5 8.2l2 2 4-4.2" fill="none" stroke="rgb(var(--dashboard-card-rgb))" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>';
    if (key === "trash") return '<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="6.5" fill="none" stroke="currentColor" stroke-width="1.5" stroke-dasharray="2 2"/><path d="M5.5 5.5l5 5M10.5 5.5l-5 5" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/></svg>';
    const share = Math.max(0, Math.min(100, Number(value) || 0)) / 100;
    const pie = share > 0 ? `<circle cx="8" cy="8" r="3" fill="none" stroke="currentColor" stroke-width="6" stroke-dasharray="${(share * 18.85).toFixed(2)} 18.85" transform="rotate(-90 8 8)"/>` : "";
    return `<svg viewBox="0 0 16 16" aria-hidden="true"><circle cx="8" cy="8" r="6.5" fill="none" stroke="currentColor" stroke-width="1.5"${key === "queued" ? ' stroke-dasharray="2.2 2"' : ""}/>${pie}</svg>`;
  }

  function taskRingHtml(task, bar = false) {
    const key = task.deletedAt ? "trash" : taskStatusKey(task);
    const value = taskProgress(task);
    const label = task.deletedAt ? "In trash" : taskStatusLabel(task);
    const role = bar
      ? ` role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${value}" aria-valuetext="${value}% complete" aria-label="${escapeHtml(`Progress: ${task.title || "task"}`)}"`
      : ' aria-hidden="true"';
    return `<span class="task-ring" data-state="${key}" title="${escapeHtml(`${label} · ${value}%`)}"${role}>${taskRingSvg(key, value)}</span>`;
  }

  function taskPriorityLevel(task) {
    const value = Number(task?.priority ?? 50);
    return value <= 5 ? "urgent" : value <= 15 ? "high" : value <= 40 ? "medium" : "low";
  }

  function taskPriorityHtml(task) {
    const level = taskPriorityLevel(task);
    const title = `Priority ${task?.priority ?? 50} · ${TASK_PRIORITY_LABELS[level]}`;
    if (level === "urgent") {
      return `<span class="task-prio" data-level="urgent" title="${escapeHtml(title)}"><svg viewBox="0 0 16 16" aria-hidden="true"><rect x="1.5" y="1.5" width="13" height="13" rx="3" fill="currentColor"/><path d="M8 4.5v4.2" stroke="rgb(var(--dashboard-card-rgb))" stroke-width="1.8" stroke-linecap="round"/><circle cx="8" cy="11.3" r="1" fill="rgb(var(--dashboard-card-rgb))"/></svg></span>`;
    }
    const filled = { high: 3, medium: 2, low: 1 }[level];
    const bars = [[2, 10, 3], [6.5, 7, 6], [11, 4, 9]].map(([x, y, height], index) => `<rect x="${x}" y="${y}" width="3" height="${height}" rx="1" fill="currentColor" opacity="${index < filled ? 1 : 0.3}"/>`).join("");
    return `<span class="task-prio" data-level="${level}" title="${escapeHtml(title)}"><svg viewBox="0 0 16 16" aria-hidden="true">${bars}</svg></span>`;
  }

  function taskShortAge(value) {
    const elapsed = Math.max(0, Date.now() - Number(value || 0));
    if (!Number(value)) return "";
    const minutes = Math.floor(elapsed / 60000);
    if (minutes < 60) return `${Math.max(1, minutes)}m`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return `${hours}h`;
    const days = Math.floor(hours / 24);
    if (days < 60) return `${days}d`;
    return `${Math.floor(days / 30)}mo`;
  }

  function taskReplyCount(task) {
    return Number(task.replyCount || (Array.isArray(task.replies) ? task.replies.length : 0));
  }

  function taskRowHtml(task) {
    const selected = task.id === state.tasksView.selectedId;
    const changed = task.updatedAt || task.createdAt;
    const updated = relativeTimeLabel(changed);
    const branches = taskWorkItems(task).length;
    const replies = taskReplyCount(task);
    return `<button type="button" class="task-row" data-task-select="${escapeHtml(task.id)}" aria-current="${selected}" title="${escapeHtml(task.title)}">
      ${taskPriorityHtml(task)}
      ${taskRingHtml(task, true)}
      <span class="task-row-title">${escapeHtml(task.title)}</span>
      <span class="task-row-tags">${branches ? `<span class="task-row-count" title="${branches} work item${branches === 1 ? "" : "s"}"><i data-lucide="git-branch" class="h-3 w-3"></i>${branches}</span>` : ""}${replies ? `<span class="task-row-count" title="${replies} comment${replies === 1 ? "" : "s"}"><i data-lucide="message-square" class="h-3 w-3"></i>${replies}</span>` : ""}${task.department ? `<span class="task-label"><span>#${escapeHtml(task.department)}</span></span>` : ""}</span>
      ${taskRowThumbnailsHtml(task)}
      ${taskAvatarHtml(task)}
      <span class="task-row-age"${updated ? ` title="Updated ${escapeHtml(updated)}"` : ""}>${escapeHtml(taskShortAge(changed))}</span>
    </button>`;
  }

  function taskListHtml(tasks) {
    if (state.tasksView.filter === "trash") return tasks.map(taskRowHtml).join("");
    return TASK_GROUPS.map(([key, label]) => {
      const items = tasks.filter((task) => taskStatusKey(task) === key);
      if (!items.length) return "";
      const open = !taskCollapsedGroups.has(key);
      return `<section class="task-group" data-task-group="${key}"><button type="button" class="task-group-head" data-task-group-toggle="${key}" aria-expanded="${open}"><span class="task-ring" data-state="${key}" aria-hidden="true">${taskRingSvg(key, key === "active" ? 50 : 0)}</span>${escapeHtml(label)}<span class="task-group-count">${formatCount(items.length)}</span><i data-lucide="chevron-down" class="task-group-chevron"></i></button>${open ? items.map(taskRowHtml).join("") : ""}</section>`;
    }).join("");
  }

  function taskRowThumbnailsHtml(task) {
    const attachments = Array.isArray(task.attachments) ? task.attachments : [];
    if (!attachments.length) return "";
    const shown = attachments.slice(0, 3);
    const hidden = attachments.length - shown.length;
    const tiles = shown.map((attachment, index) => {
      const thumbnail = String(attachment.thumbnail || "");
      const preview = /^data:image\/(?:png|jpeg|webp);base64,/.test(thumbnail)
        ? `<img src="${escapeHtml(thumbnail)}" alt="" loading="lazy" />`
        : '<i data-lucide="image" class="h-4 w-4"></i>';
      const more = hidden && index === shown.length - 1 ? `<span class="task-row-thumb-more">+${hidden}</span>` : "";
      return `<span class="task-row-thumb" title="${escapeHtml(attachment.name || "Task image")}">${preview}${more}</span>`;
    }).join("");
    const label = `${attachments.length} image${attachments.length === 1 ? "" : "s"} attached`;
    return `<span class="task-row-thumbs" role="img" aria-label="${escapeHtml(label)}">${tiles}</span>`;
  }

  function taskDetailCard(icon, label, copy, wide = false, panel = false) {
    return `<section class="task-detail-card"${wide ? ' data-wide="true"' : ""}${panel ? ' data-panel="true"' : ""}><h3 class="task-detail-label"><i data-lucide="${icon}" class="h-4 w-4"></i>${escapeHtml(label)}</h3><div class="task-detail-copy">${copy || '<span class="text-muted-foreground">—</span>'}</div></section>`;
  }

  function taskPropsGroupHtml(title, body) {
    return `<section class="task-props-group">${title ? `<h3 class="task-props-title">${escapeHtml(title)}</h3>` : ""}${body}</section>`;
  }

  function taskPropsHtml(rows) {
    const items = rows.filter(Boolean).map(([label, value]) => `<div class="task-prop"><dt>${escapeHtml(label)}</dt><dd>${value}</dd></div>`).join("");
    return items ? `<dl class="task-props">${items}</dl>` : "";
  }

  function taskRepositoryHtml(repository) {
    const value = String(repository || "").trim();
    const parts = value.split("/");
    return parts.length === 2 && parts.every((part) => /^[A-Za-z0-9._-]{1,100}$/.test(part))
      ? `<a class="text-accent hover:underline" href="${escapeHtml(repoPathUrl({ owner: parts[0], name: parts[1] }))}">${escapeHtml(value)}</a>`
      : escapeHtml(value || "—");
  }

  function taskWorkItems(task) {
    return (Array.isArray(task?.workItems) ? task.workItems : [])
      .filter((item) => item && String(item.branch || "").trim());
  }

  function taskWorkItemsHtml(task) {
    const items = taskWorkItems(task);
    if (!items.length) return "";
    const rows = items.map((item) => {
      const place = [item.node && `on ${item.node}`, item.worktree].filter(Boolean).join(" · ");
      return `<li class="task-work-item">
        <span class="flex flex-wrap items-center gap-x-2 gap-y-1"><i data-lucide="git-branch" class="h-3.5 w-3.5 shrink-0 text-muted-foreground"></i><code class="font-mono text-xs text-foreground">${escapeHtml(item.branch)}</code>${item.repository ? `<span class="text-xs text-muted-foreground">in ${taskRepositoryHtml(item.repository)}</span>` : ""}</span>
        ${place ? `<span class="mt-0.5 block truncate font-mono text-[10px] text-muted-foreground" title="${escapeHtml(place)}">${escapeHtml(place)}</span>` : ""}
      </li>`;
    }).join("");
    return taskDetailCard("git-branch", `Work items (${items.length})`, `<ul class="task-work-items">${rows}</ul>`, true);
  }

  function taskAttachmentsHtml(task) {
    const attachments = Array.isArray(task.attachments) ? task.attachments : [];
    if (!attachments.length) return "";
    const cards = attachments.map((attachment) => {
      const url = String(attachment.url || "");
      const preview = attachment.thumbnail
        ? `<img src="${escapeHtml(attachment.thumbnail)}" alt="" loading="lazy" />`
        : url ? `<span data-task-attachment-preview="${escapeHtml(url)}"><i data-lucide="image" class="h-5 w-5"></i></span>`
          : '<i data-lucide="file-image" class="h-5 w-5"></i>';
      return `<a class="task-attachment" data-task-attachment-link="${escapeHtml(url)}" data-task-attachment-name="${escapeHtml(attachment.name || "Task image")}" ${url ? `href="${escapeHtml(url)}"` : ""} target="_blank" rel="noopener">
        <span class="task-attachment-preview">${preview}</span>
        <span class="block p-2"><strong class="block truncate text-xs text-foreground">${escapeHtml(attachment.name || "Task image")}</strong><small class="mt-0.5 block text-[10px] text-muted-foreground">${escapeHtml(taskFileSize(attachment.size))} · ${escapeHtml(attachment.mime || "image")}</small></span>
      </a>`;
    }).join("");
    return taskDetailCard("paperclip", `Attachments (${attachments.length})`, `<div class="task-attachment-grid">${cards}</div>`, true);
  }

  async function taskAttachmentObjectUrl(path) {
    if (taskAttachmentUrls.has(path)) return taskAttachmentUrls.get(path);
    const token = state.session?.sessionToken || "";
    const response = await fetch(path, {
      credentials: "same-origin",
      cache: "no-store",
      headers: token ? { authorization: `Bearer ${token}` } : {},
    });
    if (!response.ok) throw new Error("Attachment unavailable");
    const objectUrl = URL.createObjectURL(await response.blob());
    taskAttachmentUrls.set(path, objectUrl);
    while (taskAttachmentUrls.size > 32) {
      const oldest = taskAttachmentUrls.keys().next().value;
      URL.revokeObjectURL(taskAttachmentUrls.get(oldest));
      taskAttachmentUrls.delete(oldest);
    }
    return objectUrl;
  }

  function hydrateTaskAttachments(root) {
    root.querySelectorAll?.("[data-task-attachment-preview]").forEach((preview) => {
      const path = preview.dataset.taskAttachmentPreview;
      if (!path || preview.dataset.loading) return;
      preview.dataset.loading = "true";
      void taskAttachmentObjectUrl(path).then((objectUrl) => {
        if (!preview.isConnected) return;
        const image = document.createElement("img");
        image.src = objectUrl;
        image.alt = "";
        image.loading = "lazy";
        preview.replaceWith(image);
        const link = image.closest("[data-task-attachment-link]");
        if (link) link.href = objectUrl;
      }).catch(() => { preview.dataset.loading = "failed"; });
    });
  }

  function openTaskImagePreview(src, name) {
    let dialog = document.querySelector("[data-task-image-preview]");
    if (!dialog) {
      dialog = document.createElement("dialog");
      dialog.className = "task-image-modal p-3 sm:p-5";
      dialog.dataset.taskImagePreview = "true";
      dialog.innerHTML = '<header class="mb-3 flex items-center justify-between gap-3"><strong class="min-w-0 truncate text-sm text-foreground" data-task-image-preview-name></strong><button type="button" class="task-btn" data-size="small" data-task-image-preview-close>Close</button></header><img data-task-image-preview-image alt="" /><p class="mt-3 text-center text-xs text-muted-foreground">Click outside the image or press Escape to close.</p>';
      dialog.querySelector("[data-task-image-preview-close]").addEventListener("click", () => dialog.close());
      dialog.addEventListener("click", (event) => {
        if (event.target === dialog) dialog.close();
      });
      document.body.append(dialog);
    }
    dialog.querySelector("[data-task-image-preview-name]").textContent = name || "Task image";
    const image = dialog.querySelector("[data-task-image-preview-image]");
    image.src = src;
    image.alt = name || "Task image";
    if (!dialog.open) dialog.showModal();
  }

  async function expandTaskAttachment(link) {
    const path = String(link?.dataset.taskAttachmentLink || "");
    let src = link?.querySelector("img")?.src || "";
    if (path) {
      try {
        src = await taskAttachmentObjectUrl(path);
      } catch (_) {}
    }
    if (src) openTaskImagePreview(src, link?.dataset.taskAttachmentName || "Task image");
  }

  function taskHistoryHtml(task) {
    const history = Array.isArray(task.completionHistory) ? task.completionHistory : [];
    if (!history.length) return "";
    const items = [...history].reverse().map((entry) => `<li class="border-t border-border pt-3 first:border-t-0 first:pt-0">
      <div class="flex flex-wrap items-center gap-2 text-xs"><strong class="text-foreground">${escapeHtml(entry.completedBy ? `@${entry.completedBy}` : "Completed")}</strong><time class="text-muted-foreground">${escapeHtml(taskDate(entry.completedAt))}</time><span class="task-label">QA: ${escapeHtml(entry.qaStatus || "unknown")}</span>${entry.qaReviewer ? `<span class="text-muted-foreground">reviewed by @${escapeHtml(entry.qaReviewer)}${entry.qaReviewedAt ? ` · ${escapeHtml(taskDate(entry.qaReviewedAt))}` : ""}</span>` : ""}${entry.undoneAt ? `<span class="task-label">Reopened by ${escapeHtml(entry.undoneBy ? `@${entry.undoneBy}` : "a member")} · ${escapeHtml(taskDate(entry.undoneAt))}</span>` : ""}</div>
      ${entry.note ? `<div class="mt-2 text-xs leading-5 text-muted-foreground">${taskMarkdownHtml(entry.note)}</div>` : '<p class="mt-2 text-xs leading-5 text-muted-foreground">No completion note</p>'}
    </li>`).join("");
    return taskDetailCard("history", `Completion history (${history.length})`, `<ol class="grid gap-3">${items}</ol>`, true);
  }

  // ---- Comments, @mentions, check-ins and the activity timeline -------------
  // Every recorded thing that has happened to a task, on one rail. The relay
  // returns lifecycle events in `history` and comment bodies in `replies`; a
  // "replied" event and its reply row share a timestamp, so the row wins and
  // the full comment is shown in place of the event's excerpt.
  const TASK_EVENT_STYLES = {
    created: { icon: "sparkles", label: "created this task", tone: "" },
    updated: { icon: "pencil", label: "edited this task", tone: "" },
    started: { icon: "play", label: "started work", tone: "primary" },
    stopped: { icon: "pause", label: "paused work", tone: "" },
    checkin: { icon: "radio", label: "checked in", tone: "primary" },
    completed: { icon: "check-circle-2", label: "completed this task", tone: "primary" },
    reopened: { icon: "history", label: "reopened this task", tone: "warning" },
    returned: { icon: "undo-2", label: "returned this task to the list", tone: "" },
    qa_requested: { icon: "shield-question", label: "sent this to QA", tone: "" },
    qa_reviewed: { icon: "shield-check", label: "reviewed this in QA", tone: "primary" },
    deleted: { icon: "trash-2", label: "deleted this task", tone: "danger" },
    reminded: { icon: "bell-ring", label: "sent a reminder", tone: "warning" },
    reminder_seen: { icon: "eye", label: "saw the reminder", tone: "primary" },
    emailed: { icon: "mail", label: "set off an email", tone: "" },
    email_opened: { icon: "mail-open", label: "opened the email", tone: "primary" },
    email_clicked: { icon: "mouse-pointer-click", label: "clicked a link in the email", tone: "primary" },
    email_viewed: { icon: "eye", label: "opened the task from the email", tone: "primary" },
  };
  const TASK_CHECKIN_STATES = [
    ["going_well", "thumbs-up", "Going well", "primary"],
    ["blocked", "circle-alert", "Blocked", "danger"],
    ["needs_help", "life-buoy", "Need help", ""],
  ];
  const TASK_CHECKIN_NOTE_MAX = 500;
  const TASK_REMINDER_NOTE_MAX = 500;
  const TASK_REMINDER_COOLDOWN_MS = 10 * 60 * 1000;
  // The same handle shape the relay pings on (its MENTION_RE): a handle, never
  // the local part of an email address.
  const TASK_MENTION_SOURCE = "(^|[^A-Za-z0-9._%+-])@([a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)";

  function taskMentionNames(value) {
    const names = [];
    for (const match of String(value || "").matchAll(new RegExp(TASK_MENTION_SOURCE, "gi"))) {
      const name = match[2].toLowerCase();
      if (!names.includes(name)) names.push(name);
    }
    return names;
  }

  function taskMentionHtml(value) {
    const text = String(value || "");
    let out = "";
    let cursor = 0;
    for (const match of text.matchAll(new RegExp(TASK_MENTION_SOURCE, "gi"))) {
      const start = match.index + match[1].length;
      const name = match[2].toLowerCase();
      out += escapeHtml(text.slice(cursor, start));
      out += `<span class="task-mention"${name === state.tasksView.actor ? ' data-me="true"' : ""}>@${escapeHtml(name)}</span>`;
      cursor = start + 1 + match[2].length;
    }
    return out + escapeHtml(text.slice(cursor));
  }

  function taskMarkdownHtml(value) {
    const html = renderMarkdown(String(value || ""), { repo: null });
    let literal = 0;
    return `<div class="task-markdown ${MD_BODY_CLASS}">${html.split(/(<[^>]*>)/).map((part, index) => {
      if (index % 2) {
        if (/^<(?:pre|code|a)\b/i.test(part)) literal += 1;
        else if (/^<\/(?:pre|code|a)\b/i.test(part)) literal = Math.max(0, literal - 1);
        return part;
      }
      if (literal) return part;
      return part.replace(new RegExp(TASK_MENTION_SOURCE, "gi"), (whole, prefix, name) => {
        const lower = name.toLowerCase();
        return `${prefix}<span class="task-mention"${lower === state.tasksView.actor ? ' data-me="true"' : ""}>@${lower}</span>`;
      });
    }).join("")}</div>`;
  }

  function taskTimelineEntries(task) {
    const replies = Array.isArray(task.replies) ? task.replies : [];
    const replyTimes = new Set(replies.map((reply) => Number(reply.createdAt) || 0));
    const events = (Array.isArray(task.history) ? task.history : [])
      .filter((entry) => entry && !(entry.kind === "replied" && replyTimes.has(Number(entry.createdAt) || 0)))
      .map((entry) => ({
        id: String(entry.id || ""),
        // A reply that has scrolled out of the replies window still has its
        // event, so it is drawn as the comment it was rather than dropped.
        kind: entry.kind === "replied" ? "comment" : String(entry.kind || ""),
        actor: String(entry.actor || "").toLowerCase(),
        body: String(entry.summary || ""),
        at: Number(entry.createdAt) || 0,
      }));
    const comments = replies.map((reply) => ({
      id: String(reply.id || ""),
      kind: "comment",
      actor: String(reply.author || "").toLowerCase(),
      body: String(reply.body || ""),
      at: Number(reply.createdAt) || 0,
      readBy: Array.isArray(reply.readBy) ? reply.readBy.map((reader) => String(reader?.name || "")).filter(Boolean) : null,
    }));
    return [...events, ...comments].sort((left, right) => left.at - right.at || left.id.localeCompare(right.id));
  }

  function taskTimeHtml(at) {
    return `<time title="${escapeHtml(taskDate(at))}">${escapeHtml(relativeTimeLabel(at) || taskDate(at))}</time>`;
  }

  function taskCheckinChipHtml(value) {
    const text = String(value || "").trim();
    const key = text.toLowerCase().replace(/[\s-]+/g, "_");
    const known = TASK_CHECKIN_STATES.find(([stateKey]) => key === stateKey || key.startsWith(`${stateKey}_`));
    if (!known) return text ? ` — <span class="task-timeline-copy">${taskMentionHtml(text)}</span>` : "";
    const note = text.slice(known[0].length).replace(/^[\s:·—-]+/, "");
    return ` <span class="task-checkin-chip" data-state="${known[0]}">${escapeHtml(known[2])}</span>${note ? ` <span class="task-timeline-copy">${taskMentionHtml(note)}</span>` : ""}`;
  }

  function taskTimelineEntryHtml(entry, task) {
    const who = entry.actor ? `@${entry.actor}` : "Someone";
    const initial = escapeHtml(Array.from(entry.actor || "?")[0].toUpperCase());
    if (entry.kind === "comment") {
      return `<li class="task-timeline-item" data-kind="comment">
        <span class="task-timeline-marker" data-task-assignee-avatar="${escapeHtml(entry.actor)}" title="${escapeHtml(who)}">${initial}</span>
        <div class="task-comment"><div class="task-comment-head"><strong>${escapeHtml(who)}</strong><span>commented</span>${taskTimeHtml(entry.at)}</div><div class="task-comment-body">${taskMarkdownHtml(entry.body)}</div>${entry.readBy ? `<p class="task-comment-hint">${entry.readBy.length ? `✓ Read by ${escapeHtml(entry.readBy.map((name) => `@${name}`).join(", "))}` : "Not read yet"}</p>` : ""}</div>
      </li>`;
    }
    const style = TASK_EVENT_STYLES[entry.kind] || { icon: "dot", label: entry.kind.replaceAll("_", " ") || "updated this task", tone: "" };
    let detail = "";
    if (entry.kind === "checkin") detail = taskCheckinChipHtml(entry.body);
    else if (entry.body && entry.body !== task.title) detail = ` — <span class="task-timeline-copy">${taskMentionHtml(entry.body)}</span>`;
    return `<li class="task-timeline-item" data-kind="${escapeHtml(entry.kind)}">
      <span class="task-timeline-marker"${style.tone ? ` data-tone="${style.tone}"` : ""}><i data-lucide="${escapeHtml(style.icon)}" class="h-3 w-3"></i></span>
      <p class="task-timeline-line"><strong>${escapeHtml(who)}</strong> ${escapeHtml(style.label)}${detail} · ${taskTimeHtml(entry.at)}</p>
    </li>`;
  }

  function taskActivityHtml(task) {
    const entries = taskTimelineEntries(task);
    const pending = !Array.isArray(task.history) && !taskDetailsLoaded.has(task.id);
    const timeline = entries.length
      ? `<ol class="task-timeline">${entries.map((entry) => taskTimelineEntryHtml(entry, task)).join("")}</ol>`
      : `<p class="task-comment-hint mb-3">${pending ? "Loading activity…" : "Nothing has happened to this task yet."}</p>`;
    const count = Number(task.replyCount || (Array.isArray(task.replies) ? task.replies.length : 0));
    return taskDetailCard("messages-square", `Activity · ${count} comment${count === 1 ? "" : "s"}`, timeline, true);
  }

  function taskQuickCommentHtml(task) {
    if (task.deletedAt) return "";
    return `<form data-task-reply-form="${escapeHtml(task.id)}" class="task-comment-form task-quick-comment">
      <div class="task-mention-field"><textarea data-task-reply-input data-task-mention-input="${escapeHtml(task.id)}" maxlength="2000" rows="2" required autocomplete="off" placeholder="Leave a comment — type @ to ping a teammate" aria-label="Comment on this task" class="task-input">${escapeHtml(taskCommentDrafts.get(task.id) || "")}</textarea><div data-task-mention-list class="task-mention-list fm-popover" role="listbox" aria-label="Mention a teammate" hidden></div></div>
      <div class="task-comment-footer"><span class="task-comment-hint">Everyone on the task is pinged; Ctrl+Enter posts.</span><button type="submit" class="task-action-button" data-tone="primary"><i data-lucide="send" class="h-3.5 w-3.5"></i>Comment</button></div>${taskNoticeHtml(task, "activity")}
    </form>`;
  }

  function taskStatusChoice(task) {
    const key = taskStatusKey(task);
    return key === "done" || key === "active" ? key : "ready";
  }

  function taskPropertyAccess(task) {
    const actor = state.tasksView.actor;
    const manager = state.tasksView.canManage;
    const done = taskIsDone(task);
    const active = task.status === "active";
    const mine = taskAssigneeKind(task) === "user" && task.assignee === actor;
    const creator = Boolean(actor) && task.createdBy === actor;
    const reviewer = Boolean(actor) && task.qa?.reviewer === actor;
    const busy = Boolean(state.tasksView.busyId) || Boolean(task.deletedAt);
    return {
      busy,
      edit: !busy && manager && !active && !done,
      priority: !busy && manager,
      start: !busy && !done && mine,
      complete: !busy && !done && (mine || manager),
      undo: !busy && done && (manager || mine || creator || reviewer),
      locked: task.deletedAt ? "Restore the task to change this" : !manager ? "Only task managers can change this" : active ? "Stop the task to change this" : done ? "Mark the task undone to change this" : "",
    };
  }

  function taskPropSelectHtml(name, label, choices, current, enabled, title = "") {
    if (!enabled) {
      const shown = choices.find(([value]) => value === current)?.[1] || "—";
      return `<span${title ? ` title="${escapeHtml(title)}"` : ""}>${escapeHtml(shown)}</span>`;
    }
    const options = choices.map(([value, text, allowed = true]) => `<option value="${escapeHtml(value)}"${value === current ? " selected" : ""}${allowed || value === current ? "" : " disabled"}>${escapeHtml(text)}</option>`).join("");
    return `<select class="task-prop-input" data-task-prop="${name}" aria-label="${escapeHtml(label)}"${title ? ` title="${escapeHtml(title)}"` : ""}${enabled ? "" : " disabled"}>${options}</select>`;
  }

  function taskPropTextHtml(name, label, value, enabled, placeholder, title = "", extra = "") {
    if (!enabled) return `<span${title ? ` title="${escapeHtml(title)}"` : ""}>${value ? escapeHtml(value) : '<span class="text-muted-foreground">None</span>'}</span>`;
    return `<input class="task-prop-input" data-task-prop="${name}" aria-label="${escapeHtml(label)}" value="${escapeHtml(value || "")}" placeholder="${escapeHtml(placeholder)}" autocomplete="off" spellcheck="false"${title ? ` title="${escapeHtml(title)}"` : ""}${extra}${enabled ? "" : " disabled"} />`;
  }

  function taskEditablePropsHtml(task) {
    const access = taskPropertyAccess(task);
    const status = taskStatusChoice(task);
    const statusChoices = [
      ["ready", "Ready", access.undo || (status === "active" && access.start)],
      ["active", "In progress", access.start],
      ["done", "Done", access.complete],
    ];
    const statusEnabled = access.start || access.complete || access.undo;
    const kind = taskAssigneeKind(task);
    const assignment = kind === "user" && task.assignee ? `user:${task.assignee}` : kind === "agent" ? "agent" : "unassigned";
    const members = [...new Set([...(state.tasksView.members || []).map((name) => String(name || "").toLowerCase()).filter(Boolean), ...(kind === "user" && task.assignee ? [task.assignee] : [])])];
    const assigneeChoices = [["unassigned", "Unassigned"], ["agent", "Bot", Boolean(task.repository)], ...members.map((name) => [`user:${name}`, `@${name}`])];
    const departments = [...new Set([...(state.tasksView.departments.length ? state.tasksView.departments : ["general", "engineering"]), ...(task.department ? [task.department] : [])])];
    const teams = [...new Set([...(state.tasksView.teams || []), ...(task.team ? [task.team] : [])])];
    const priority = Number(task.priority ?? 50);
    const creator = task.createdBy ? `${taskAvatarHtml({ assigneeKind: "user", assignee: task.createdBy })}@${escapeHtml(task.createdBy)}` : "—";
    return taskPropsHtml([
      ["Status", `${taskRingHtml(task)}${task.deletedAt ? '<span>In trash</span>' : statusEnabled ? taskPropSelectHtml("status", "Status", statusChoices, status, true) : `<span>${escapeHtml(taskStatusLabel(task))}</span>`}`],
      ["Priority", `${taskPriorityHtml(task)}${access.priority ? `<input type="number" min="1" max="99" step="1" inputmode="numeric" class="task-prop-input task-prop-number" data-task-prop="priority" aria-label="Priority" value="${escapeHtml(String(priority))}" />` : `<span class="tabular-nums">P${escapeHtml(String(priority))}</span>`}<span class="text-muted-foreground">${TASK_PRIORITY_LABELS[taskPriorityLevel(task)]}</span>`],
      ["Assignee", `${taskAvatarHtml(task)}${access.edit ? taskPropSelectHtml("assignee", "Assignee", assigneeChoices, assignment, true) : `<span title="${escapeHtml(access.locked)}">${escapeHtml(taskAssigneeLabel(task))}</span>`}`],
      ["Created by", creator],
      ["Repository", access.edit ? taskPropTextHtml("repository", "Repository", task.repository, true, "owner/repository") : task.repository ? taskRepositoryHtml(task.repository) : '<span class="text-muted-foreground">None</span>'],
      ["Department", taskPropSelectHtml("department", "Department", departments.map((value) => [value, `#${value.replaceAll("-", " ")}`]), task.department || "general", access.edit, access.locked)],
      ["Team", taskPropSelectHtml("team", "Team", [["", "No specific team"], ...teams.map((value) => [value, value])], task.team || "", access.edit, access.locked)],
      ["Follows", taskPropTextHtml("parentTaskId", "Follows task", task.parentTaskId, access.edit, "Parent task ID", access.locked, ' maxlength="32"')],
      task.destination && ["Routed to", `<span class="task-label"><span>${escapeHtml(task.destination)}</span></span>`],
    ]) + taskNoticeHtml(task, "props");
  }

  async function submitTaskProperty(control) {
    const task = selectedTask();
    const name = control?.dataset.taskProp;
    if (!task || !name || control.disabled || state.tasksView.busyId) return;
    if (name === "status") {
      const current = taskStatusChoice(task);
      const target = control.value;
      if (target === current) return;
      const action = target === "done" ? "complete" : current === "done" ? "undo" : target === "active" ? "start" : "stop";
      void mutateTask(action, task);
      return;
    }
    let body = null;
    if (name === "priority") {
      const raw = Math.round(Number(control.value));
      if (!Number.isFinite(raw)) { renderTaskDetail(); return; }
      const priority = Math.max(1, Math.min(99, raw));
      if (priority !== Number(task.priority ?? 50)) body = { priority };
    } else if (name === "assignee") {
      const value = String(control.value || "unassigned");
      const kind = value === "agent" ? "agent" : value.startsWith("user:") ? "user" : "unassigned";
      const current = taskAssigneeKind(task) === "user" && task.assignee ? `user:${task.assignee}` : taskAssigneeKind(task) === "agent" ? "agent" : "unassigned";
      if (value !== current) body = { assigneeKind: kind, ...(kind === "user" ? { assignee: value.slice(5) } : {}) };
    } else {
      const value = String(control.value || "").trim().toLowerCase();
      const previous = String(task[name] || "").trim();
      const next = name === "repository" ? String(control.value || "").trim() : value;
      if (next !== previous) body = { [name]: next };
    }
    if (!body) return;
    state.tasksView.busyId = task.id;
    renderTaskDetail();
    try {
      const payload = await taskRequest("PATCH", `/api/tasks/${task.id}`, body);
      replaceTask(payload.task);
      taskDetailsLoaded.add(payload.task.id);
      taskDetailNotice = { taskId: task.id, place: "props", message: "Saved.", tone: "good" };
      renderTasksPage();
    } catch (error) {
      taskDetailNotice = { taskId: task.id, place: "props", message: taskFriendlyError(error), tone: "bad" };
    } finally {
      state.tasksView.busyId = "";
      renderTaskDetail();
    }
  }

  function taskCanCheckIn(task) {
    return Boolean(task) && task.status === "active" && !taskIsDone(task)
      && taskAssigneeKind(task) === "user" && task.assignee === state.tasksView.actor;
  }

  function taskCheckinHtml(task) {
    if (taskCheckinOpenId !== task.id || !taskCanCheckIn(task)) return "";
    const buttons = TASK_CHECKIN_STATES.map(([key, icon, label, tone]) => `<button type="button" class="task-action-button" data-task-checkin-state="${key}"${tone ? ` data-tone="${tone}"` : ""}${taskCheckinBusy ? " disabled" : ""}><i data-lucide="${icon}" class="h-3.5 w-3.5"></i>${escapeHtml(label)}</button>`).join("");
    return taskDetailCard("radio", "Check in", `<p class="task-comment-hint">Tell your team how this is going. Everyone on the task is pinged, and so is anyone you @mention.</p>
      <div class="task-mention-field mt-3"><input data-task-checkin-note data-task-mention-input="${escapeHtml(task.id)}" maxlength="${TASK_CHECKIN_NOTE_MAX}" autocomplete="off" value="${escapeHtml(taskCheckinNotes.get(task.id) || "")}" placeholder="Optional note — @mention whoever should know" class="task-input" /><div data-task-mention-list class="task-mention-list fm-popover" role="listbox" aria-label="Mention a teammate" hidden></div></div>
      <div class="task-checkin-states mt-3">${buttons}<button type="button" class="task-action-button" data-task-checkin-cancel>Cancel</button></div>${taskNoticeHtml(task, "checkin")}`, true, true);
  }

  function taskCanRemind(task) {
    return Boolean(task) && !taskIsDone(task) && taskAssigneeKind(task) === "user"
      && Boolean(task.assignee) && task.assignee !== state.tasksView.actor;
  }

  function taskReminderWaitMs(task) {
    const at = Number(task?.lastReminder?.at) || 0;
    return at ? Math.max(0, at + TASK_REMINDER_COOLDOWN_MS - Date.now()) : 0;
  }

  function taskReminderStatus(reminder) {
    if (!reminder) return "";
    const who = reminder.to ? `@${reminder.to}` : "The assignee";
    if (reminder.seenAt) {
      const how = reminder.seenVia === "ping" ? "opened the ping" : "opened the task";
      return `Seen: ${who} ${how} ${relativeTimeLabel(reminder.seenAt) || "just now"}`;
    }
    if (reminder.delivered === false) return "Not delivered: the ping could not be filed";
    const phones = Number(reminder.phones) || 0;
    const reach = phones ? `pinged and pushed to ${phones} phone${phones === 1 ? "" : "s"}` : "pinged";
    return `Not seen yet: ${who} was ${reach}`;
  }

  function taskRemindHtml(task) {
    if (taskRemindOpenId !== task.id || !taskCanRemind(task)) return "";
    const wait = taskReminderWaitMs(task);
    const hint = wait
      ? `@${escapeHtml(task.lastReminder.by || "someone")} reminded @${escapeHtml(task.assignee)} ${escapeHtml(relativeTimeLabel(task.lastReminder.at) || "just now")}. You can send another in ${Math.ceil(wait / 60000)} min.`
      : `Ping @${escapeHtml(task.assignee)} to check in and share how this task is going.`;
    return taskDetailCard("bell-ring", "Ask for a check-in", `<p class="task-comment-hint">${hint}</p>
      <input data-task-remind-note maxlength="${TASK_REMINDER_NOTE_MAX}" autocomplete="off" value="${escapeHtml(taskReminderNotes.get(task.id) || "")}" placeholder="Optional note, like “Can you share an update before standup?”" class="task-input mt-3" />
      <div class="task-checkin-states mt-3"><button type="button" class="task-action-button" data-tone="primary" data-task-remind-send${taskRemindBusy || wait ? " disabled" : ""}><i data-lucide="send" class="h-3.5 w-3.5"></i>Send reminder</button><button type="button" class="task-action-button" data-task-remind-cancel>Cancel</button></div>${taskNoticeHtml(task, "remind")}`, true, true);
  }

  // Candidates come from what this viewer can already read — the roster the
  // editor loads, assignees, authors and timeline actors — so the composer
  // needs no organization lookup of its own.
  function taskMentionCandidates() {
    const names = new Set();
    const add = (value) => {
      const raw = value && typeof value === "object" ? (value.name || value.username || value.account) : value;
      const name = String(raw || "").trim().toLowerCase();
      if (name !== state.tasksView.actor && /^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(name)) names.add(name);
    };
    (Array.isArray(state.tasksView.members) ? state.tasksView.members : []).forEach(add);
    for (const task of state.tasksView.items) {
      if (taskAssigneeKind(task) === "user") add(task.assignee);
      add(task.createdBy);
      (Array.isArray(task.replies) ? task.replies : []).forEach((reply) => add(reply.author));
      (Array.isArray(task.history) ? task.history : []).forEach((entry) => add(entry.actor));
    }
    return [...names].sort();
  }

  function taskMentionQueryAt(input) {
    const caret = input.selectionStart ?? input.value.length;
    const match = /(^|[^A-Za-z0-9._%+-])@([a-z0-9-]*)$/i.exec(input.value.slice(0, caret));
    return match ? match[2].toLowerCase() : null;
  }

  function renderTaskMentionList(input) {
    const list = input.closest(".task-mention-field")?.querySelector("[data-task-mention-list]");
    if (!list) return;
    const query = taskMentionState.query;
    const matches = query === null ? [] : taskMentionCandidates().filter((name) => name.startsWith(query)).slice(0, 6);
    taskMentionState.matches = matches;
    if (!matches.length) { list.hidden = true; list.innerHTML = ""; return; }
    taskMentionState.index = Math.min(taskMentionState.index, matches.length - 1);
    list.innerHTML = matches.map((name, index) => `<button type="button" role="option" tabindex="-1" class="task-mention-option" data-task-mention-pick="${escapeHtml(name)}" aria-selected="${index === taskMentionState.index}">@${escapeHtml(name)}</button>`).join("");
    list.hidden = false;
  }

  function rememberTaskDraft(input) {
    const store = input.matches("[data-task-checkin-note]") ? taskCheckinNotes : taskCommentDrafts;
    const taskId = input.dataset.taskMentionInput || "";
    if (input.value) store.set(taskId, input.value);
    else store.delete(taskId);
  }

  function insertTaskMention(input, name) {
    const caret = input.selectionStart ?? input.value.length;
    const before = input.value.slice(0, caret).replace(/@([a-z0-9-]*)$/i, `@${name} `);
    input.value = before + input.value.slice(caret);
    input.focus();
    input.setSelectionRange(before.length, before.length);
    rememberTaskDraft(input);
    taskMentionState.query = null;
    renderTaskMentionList(input);
  }

  function taskPingedCopy(lead, body) {
    const named = taskMentionNames(body).filter((name) => name !== state.tasksView.actor);
    return named.length
      ? `${lead}. The task's team was pinged, and so was ${named.map((name) => `@${name}`).join(", ")}.`
      : `${lead}. The task's team was pinged.`;
  }

  function taskActionHtml(action, icon, label, enabled = true, tone = "") {
    return { action, icon, label, enabled, tone };
  }

  function taskActionButtonHtml(item, tone = item.tone) {
    return `<button type="button" class="task-action-button" data-task-action="${item.action}"${tone ? ` data-tone="${tone}"` : ""}${item.enabled ? "" : " disabled"}><i data-lucide="${item.icon}" class="h-3.5 w-3.5"></i>${escapeHtml(item.label)}</button>`;
  }

  function taskActionMenuItemHtml(item) {
    return `<button type="button" role="menuitem" class="task-menu-item" data-task-action="${item.action}"${item.tone === "danger" ? ' data-tone="danger"' : ""}${item.enabled ? "" : " disabled"}><i data-lucide="${item.icon}"></i>${escapeHtml(item.label)}</button>`;
  }

  const TASK_QUICK_ACTIONS = ["stop", "start", "checkin", "remind", "complete", "qa", "edit", "assign", "followup"];

  function taskActionLayoutHtml(task, items) {
    const find = (action) => items.find((item) => item.action === action && item.enabled);
    const order = task.deletedAt ? ["restore", "copy"]
      : taskIsDone(task) ? ["undo", "followup", "copy"]
        : [task.status === "active" ? "complete" : "start", "remind", "complete", "qa", "edit", "copy"];
    const primary = order.map(find).find(Boolean);
    const quick = TASK_QUICK_ACTIONS.map(find).filter((item) => item && item !== primary).slice(0, 3);
    const rest = items.filter((item) => item !== primary && !quick.includes(item));
    const safe = rest.filter((item) => item.tone !== "danger");
    const danger = rest.filter((item) => item.tone === "danger");
    const open = taskActionMenuOpenId === task.id;
    const menu = rest.length ? `<span class="task-action-spacer"></span><div class="task-menu-wrap"><button type="button" class="task-action-button" data-task-action-menu aria-haspopup="menu" aria-expanded="${open}"><i data-lucide="ellipsis" class="h-3.5 w-3.5"></i>More</button>${open ? `<div class="task-menu" role="menu">${safe.map(taskActionMenuItemHtml).join("")}${safe.length && danger.length ? '<hr class="task-menu-divider">' : ""}${danger.map(taskActionMenuItemHtml).join("")}</div>` : ""}</div>` : "";
    return `${primary ? taskActionButtonHtml(primary, "solid") : ""}${quick.map((item) => taskActionButtonHtml(item, item.tone === "primary" ? "" : item.tone)).join("")}${menu}`;
  }

  function taskActionsHtml(task) {
    const actor = state.tasksView.actor;
    const manager = state.tasksView.canManage;
    const done = taskIsDone(task);
    const active = task.status === "active";
    const mine = taskAssigneeKind(task) === "user" && task.assignee === actor;
    const creator = task.createdBy === actor;
    const reviewer = task.qa?.reviewer === actor;
    const agent = taskAssigneeKind(task) === "agent";
    if (task.deletedAt) {
      return taskActionLayoutHtml(task, [
        taskActionHtml("restore", "archive-restore", "Restore", task.canRestore !== false && !state.tasksView.busyId, "primary"),
        taskActionHtml("copy", "copy", "Copy prompt", true),
      ]);
    }
    return taskActionLayoutHtml(task, [
      taskActionHtml("edit", "pencil", "Edit", manager && !active && !done),
      taskActionHtml("assign", "bot", agent ? "Bot assigned" : "Assign bot", manager && !active && !done && Boolean(task.repository) && !agent),
      taskActionHtml(active ? "stop" : "start", active ? "square" : "play", active ? "Stop" : "Start", mine && !done, active ? "" : "primary"),
      taskActionHtml("checkin", "radio", "Check in", taskCanCheckIn(task)),
      taskActionHtml("remind", "bell-ring", "Remind", taskCanRemind(task)),
      taskActionHtml("complete", "check-circle-2", "Complete", !done && (mine || manager), "primary"),
      taskActionHtml("qa", "shield-check", "Send to QA", !done && manager),
      taskActionHtml("return", "undo-2", "Return to list", !done && agent && (manager || creator)),
      taskActionHtml("undo", "history", "Mark undone", done && (manager || mine || creator || reviewer)),
      taskActionHtml("followup", "git-branch", "Follow up", manager || mine || creator),
      taskActionHtml("copy", "copy", "Copy prompt", true),
      taskActionHtml("delete", "trash-2", "Delete", manager || mine || creator, "danger"),
    ]);
  }

  function renderTaskDetail() {
    const detail = $("[data-tasks-detail]");
    if (!detail) return;
    const task = selectedTask();
    if (!task) {
      detail.innerHTML = '<div class="task-blankslate" data-framed="true"><i data-lucide="list-checks"></i><strong>No task selected</strong><p>Pick a task from the list to see its details and activity.</p></div>';
      window.lucide?.createIcons();
      return;
    }
    const qa = task.qa || {};
    const agent = task.agent || {};
    const updated = relativeTimeLabel(task.updatedAt || task.createdAt);
    const checkin = task.lastCheckin;
    const agentLines = [
      agent.provider && `Provider: ${agent.provider}`, agent.model && `Model: ${agent.model}`,
      agent.mode && `Mode: ${agent.mode}`, agent.strength && `Reasoning: ${agent.strength}`,
      agent.status && `Run status: ${agent.status}`, agent.startedBy && `Started by: ${agent.startedBy}`,
      agent.finishedBy && `Finished by: ${agent.finishedBy}`,
      (task.agentSessionId || agent.sessionId) && `Session: ${task.agentSessionId || agent.sessionId}`,
    ].filter(Boolean);
    const qaTone = { passed: "", failed: "bad", requested: "attention" }[qa.status];
    const qaLabel = { passed: "Passed", failed: "Failed", requested: "Awaiting review" }[qa.status] || (qa.status && qa.status !== "unknown" ? qa.status : "Not requested");
    detail.innerHTML = `<article class="task-detail" data-task-detail-id="${escapeHtml(task.id)}">
      <div class="task-detail-main">
        <header class="task-detail-head">
          <div class="task-detail-crumbs">${task.repository ? `${taskRepositoryHtml(task.repository)}<span aria-hidden="true">/</span>` : ""}${task.department ? `<span>#${escapeHtml(task.department)}</span><span aria-hidden="true">/</span>` : ""}<button type="button" class="task-detail-id" data-task-action="copy-id" title="Copy task ID ${escapeHtml(task.id)}">${escapeHtml(task.id.slice(0, 8))}<i data-lucide="copy" class="h-3 w-3"></i></button>${task.kind === "bid" ? '<span class="task-label">Bounty bid</span>' : ""}</div>
          <h2 class="task-detail-title">${escapeHtml(task.title || "Organization task")}</h2>
          <div class="task-action-bar">${taskActionsHtml(task)}</div>
          ${taskNoticeHtml(task, "actions")}
          ${taskQuickCommentHtml(task)}
        </header>
        ${task.deletedAt ? taskDetailCard("trash-2", "In trash", `<p>Deleted ${escapeHtml(taskDate(task.deletedAt))}${task.deletedBy ? ` by @${escapeHtml(task.deletedBy)}` : ""}</p>${task.deleteReason ? `<p class="mt-1 text-xs text-muted-foreground">${escapeHtml(task.deleteReason)}</p>` : ""}<p class="mt-2 text-xs text-muted-foreground">Restorable until ${escapeHtml(taskDate(task.purgeAt))}; after that it is purged with its replies, check-ins, QA reviews and attachments.</p>`, true, true) : ""}
        ${taskCheckinHtml(task)}
        ${taskRemindHtml(task)}
        ${taskDetailCard("align-left", "Description", task.details ? taskMarkdownHtml(task.details) : '<p class="text-muted-foreground">No details provided.</p>', true)}
        ${task.completionNote ? taskDetailCard("badge-check", "Completion note", taskMarkdownHtml(task.completionNote), true) : ""}
        ${taskWorkItemsHtml(task)}
        ${taskAttachmentsHtml(task)}
        ${taskHistoryHtml(task)}
        ${taskActivityHtml(task)}
      </div>
      <aside class="task-detail-aside" aria-label="Task properties">
        ${taskPropsGroupHtml("", taskEditablePropsHtml(task))}
        ${taskProgressCardHtml(task)}
        ${taskPersistentReminderHtml(task)}
        ${taskPropsGroupHtml("Timing", taskPropsHtml([
          ["Created", escapeHtml(taskDate(task.createdAt))],
          ["Updated", escapeHtml(updated || taskDate(task.updatedAt))],
          ["Tracked", `<span class="tabular-nums">${escapeHtml(taskElapsed(task.elapsedMs))}</span>`],
          task.startedAt && ["Started", escapeHtml(taskDate(task.startedAt))],
          task.nextCheckinAt && ["Next check-in", escapeHtml(taskDate(task.nextCheckinAt))],
          task.completedAt && ["Completed", escapeHtml(taskDate(task.completedAt))],
        ]))}
        ${taskPropsGroupHtml("Quality assurance", taskPropsHtml([
          ["Status", `<span class="task-state"${qaTone ? ` data-tone="${qaTone}"` : ""}${qa.status === "passed" ? ' data-state="done"' : ""}>${escapeHtml(qaLabel)}</span>`],
          qa.reviewer && ["Reviewer", `@${escapeHtml(qa.reviewer)}`],
          qa.requestedAt && ["Requested", escapeHtml(taskDate(qa.requestedAt))],
          qa.reviewedAt && ["Reviewed", escapeHtml(taskDate(qa.reviewedAt))],
        ]) + (qa.howToTest ? `<div class="task-props-note"><strong>How to test</strong>${taskMarkdownHtml(qa.howToTest)}</div>` : ""))}
        ${agentLines.length ? taskPropsGroupHtml("Agent run", taskPropsHtml(agentLines.map((line) => { const at = line.indexOf(": "); return [line.slice(0, at), `<span class="break-all">${escapeHtml(line.slice(at + 2))}</span>`]; }))) : ""}
        ${task.bountyRequest ? taskPropsGroupHtml("Bounty request", taskPropsHtml([
          ["Amount", `${escapeHtml(task.bountyRequest.amountSol || "—")} ${escapeHtml(task.bountyRequest.currency || "SOL")}`],
          ["Status", `<span class="capitalize">${escapeHtml(task.bountyRequest.status || "requested")}</span>`],
        ])) : ""}
        ${checkin ? taskPropsGroupHtml("Latest check-in", `<div class="flex flex-wrap items-center gap-2">${taskCheckinChipHtml(checkin.state).trim() || `<span class="capitalize">${escapeHtml(String(checkin.state || "").replaceAll("_", " "))}</span>`}<span class="text-xs text-muted-foreground">${escapeHtml(taskDate(checkin.at))}</span></div><p class="task-props-note">${escapeHtml(checkin.note || "No note")}</p>`) : ""}
        ${task.lastReminder ? taskPropsGroupHtml("Last reminder", `<p class="task-props-note"><strong>@${escapeHtml(task.lastReminder.by || "someone")}</strong> asked for a check-in · ${escapeHtml(taskDate(task.lastReminder.at))}</p><p class="task-props-note">${escapeHtml(task.lastReminder.note || "No note")}</p><p class="task-props-note text-muted-foreground">${escapeHtml(taskReminderStatus(task.lastReminder))}</p>`) : ""}
      </aside>
    </article>`;
    window.lucide?.createIcons();
    hydrateTaskAvatars();
    hydrateTaskAttachments(detail);
    // A catalog refresh swaps in the list copy, which carries no timeline.
    if (!Array.isArray(task.history) && taskDetailsLoaded.has(task.id) && !taskHistoryRequested.has(task.id)) {
      taskHistoryRequested.add(task.id);
      taskDetailsLoaded.delete(task.id);
      void loadSelectedTaskDetail();
    }
  }

  function renderTasksPage() {
    const list = $("[data-tasks-list]");
    if (!list) return;
    const summary = $("[data-tasks-summary]");
    const count = $("[data-tasks-count]");
    const prev = $("[data-tasks-prev]");
    const next = $("[data-tasks-next]");
    const pageList = $("[data-tasks-pages]");
    renderTaskPeople();
    renderTaskListNotice();
    const ready = !state.tasksView.loading && !state.tasksView.error;
    const filterCounts = ready ? taskFilterCounts() : {};
    for (const button of $$("[data-tasks-filter]")) {
      const key = button.dataset.tasksFilter;
      const active = key === state.tasksView.filter;
      button.setAttribute("aria-pressed", String(active));
      const name = button.dataset.tasksFilterLabel || key;
      const value = filterCounts[key];
      const counted = Number.isFinite(value);
      const badge = button.querySelector("[data-tasks-filter-count]");
      if (badge) {
        badge.textContent = counted ? formatCount(value) : "";
        badge.hidden = !counted;
      }
      const label = counted ? `${name}: ${formatCount(value)}` : name;
      button.title = label;
      button.setAttribute("aria-label", label);
    }
    const manager = $("[data-tasks-manager]");
    const create = $("[data-tasks-new]");
    const clear = $("[data-tasks-delete-closed]");
    if (manager) manager.hidden = !state.tasksView.canManage;
    if (create) create.hidden = !state.tasksView.canManage;

    if (state.tasksView.loading || state.tasksView.error) {
      list.innerHTML = state.tasksView.loading
        ? `<div class="task-blankslate"><span>${loadingHtml("Loading organization tasks...")}</span></div>`
        : `<div class="task-blankslate"><i data-lucide="lock-keyhole"></i><strong>Tasks are unavailable</strong><p>${escapeHtml(state.tasksView.error)}</p></div>`;
      if (summary) summary.textContent = "";
      if (count) count.textContent = state.tasksView.loading ? "Loading" : "Unavailable";
      if (pageList) pageList.innerHTML = "";
      [prev, next].forEach((button) => { if (button) button.disabled = true; });
      if (clear) clear.hidden = true;
      $$("[data-tasks-stat]").forEach((node) => { node.textContent = "—"; });
      renderTaskDetail();
      window.lucide?.createIcons();
      return;
    }

    const trashView = state.tasksView.filter === "trash";
    if (trashView && !state.tasksView.trashLoaded) {
      if (!state.tasksView.trashLoading) void loadTaskTrash();
      list.innerHTML = `<div class="task-blankslate"><span>${loadingHtml("Loading the trash...")}</span></div>`;
      if (summary) summary.textContent = "";
      if (pageList) pageList.innerHTML = "";
      [prev, next].forEach((button) => { if (button) button.disabled = true; });
      renderTaskDetail();
      window.lucide?.createIcons();
      return;
    }

    const counts = taskStateCounts();
    if (clear) clear.hidden = !state.tasksView.canManage || !state.tasksView.items.some(taskClearable);
    if (count) count.textContent = `${formatCount(counts.open)} open · ${formatCount(counts.closed)} closed`;
    for (const key of ["open", "active", "queued", "qa"]) {
      const node = $(`[data-tasks-stat="${key}"]`);
      if (node) node.textContent = formatCount(counts[key]);
    }

    const matches = filteredTasks();
    const pages = Math.max(1, Math.ceil(matches.length / TASKS_PAGE_SIZE));
    state.tasksView.page = Math.min(Math.max(1, state.tasksView.page), pages);
    const start = (state.tasksView.page - 1) * TASKS_PAGE_SIZE;
    const end = Math.min(start + TASKS_PAGE_SIZE, matches.length);
    const visibleMatches = matches.slice(start, end);
    if (visibleMatches.length && !visibleMatches.some((task) => task.id === state.tasksView.selectedId)) {
      state.tasksView.selectedId = visibleMatches[0].id;
    }
    list.innerHTML = matches.length ? taskListHtml(visibleMatches)
      : trashView
        ? `<div class="task-blankslate"><i data-lucide="trash-2"></i><strong>The trash is empty</strong><p>Deleted tasks stay here for ${TASK_TRASH_DAYS} days.</p></div>`
        : '<div class="task-blankslate"><i data-lucide="search-x"></i><strong>No tasks match this view</strong><p>Try another status, person, or search.</p></div>';
    if (summary) summary.textContent = !matches.length
      ? (trashView ? "Trash is empty" : "No matching tasks")
      : trashView
        ? `${formatCount(matches.length)} in trash · restorable for ${TASK_TRASH_DAYS} days · showing ${start + 1}–${end} · page ${state.tasksView.page} of ${pages}`
        : `${formatCount(counts.open)} open · ${formatCount(counts.closed)} closed · showing ${start + 1}–${end} of ${formatCount(matches.length)} · page ${state.tasksView.page} of ${pages}`;
    if (prev) prev.disabled = state.tasksView.page <= 1;
    if (next) next.disabled = state.tasksView.page >= pages;
    if (pageList) {
      const visiblePages = [...new Set([1, state.tasksView.page - 1, state.tasksView.page, state.tasksView.page + 1, pages])]
        .filter((page) => page >= 1 && page <= pages).sort((a, b) => a - b);
      pageList.innerHTML = visiblePages.map((page, index) => {
        const gap = index && page - visiblePages[index - 1] > 1 ? '<span class="px-1 text-sm text-muted-foreground">…</span>' : "";
        const active = page === state.tasksView.page;
        return `${gap}<button type="button" data-tasks-page="${page}" class="task-page-button"${active ? ' aria-current="page"' : ""}>${page}</button>`;
      }).join("");
    }
    renderTaskDetail();
    window.lucide?.createIcons();
    hydrateTaskAvatars();
    void loadSelectedTaskDetail();
  }

  function replaceTask(task, select = true) {
    if (!task?.id) return;
    const index = state.tasksView.items.findIndex((item) => item.id === task.id);
    if (index >= 0) state.tasksView.items[index] = task;
    else state.tasksView.items.unshift(task);
    if (select) state.tasksView.selectedId = task.id;
  }

  function takeTaskEmailToken(taskId) {
    try {
      const url = new URL(window.location.href);
      const token = String(url.searchParams.get("mail") || "").trim().toLowerCase();
      if (!token || String(url.searchParams.get("task") || "").trim().toLowerCase() !== taskId) return "";
      url.searchParams.delete("mail");
      window.history.replaceState(window.history.state, "", url.pathname + url.search + url.hash);
      return /^[a-f0-9]{32}\.[a-f0-9]{24}$/.test(token) ? token : "";
    } catch {
      return "";
    }
  }

  async function loadSelectedTaskDetail() {
    const taskId = state.tasksView.selectedId;
    if (!taskId || taskDetailsLoaded.has(taskId) || state.tasksView.filter === "trash") return;
    taskDetailsLoaded.add(taskId);
    const mail = takeTaskEmailToken(taskId);
    try {
      const payload = await taskRequest("GET", `/api/tasks/${taskId}${mail ? `?mail=${encodeURIComponent(mail)}` : ""}`);
      replaceTask(payload.task, false);
      renderTasksPage();
    } catch (error) {
      taskDetailsLoaded.delete(taskId);
      setTaskNotice(taskId, "activity", `Could not load task activity: ${taskFriendlyError(error)}`, "bad");
    }
  }

  async function fetchOrganizationTaskCatalog() {
    let payload = null;
    const tasks = [];
    let offset = 0;
    do {
      const separator = offset ? `?offset=${offset}` : "";
      const page = await fetchJson(`/api/tasks${separator}`, { fresh: true });
      if (!payload) payload = page;
      tasks.push(...(Array.isArray(page?.tasks) ? page.tasks : []));
      const next = Number(page?.nextOffset);
      if (page?.hasMore !== true || !Number.isInteger(next) || next <= offset) break;
      offset = next;
    } while (tasks.length < Number(payload?.taskLimit || 2000));
    return { ...(payload || {}), tasks };
  }

  function applyLinkedTask() {
    if (state.tasksView.linkedTaskApplied) return;
    let linked = "";
    try {
      linked = String(new URLSearchParams(window.location.search).get("task") || "").trim().toLowerCase();
    } catch {
      linked = "";
    }
    if (!/^[a-f0-9]{32}$/.test(linked)) return;
    const task = state.tasksView.items.find((item) => item.id === linked);
    if (!task) return;
    state.tasksView.linkedTaskApplied = true;
    state.tasksView.filter = taskIsDone(task) ? "done" : "open";
    state.tasksView.assignee = "all";
    state.tasksView.query = "";
    const index = filteredTasks().findIndex((item) => item.id === linked);
    state.tasksView.page = index >= 0 ? Math.floor(index / TASKS_PAGE_SIZE) + 1 : 1;
    state.tasksView.selectedId = linked;
  }

  async function loadOrganizationTasks({ quiet = false } = {}) {
    if (!state.session) {
      state.tasksView.loading = false;
      state.tasksView.error = "Sign in with an organization member account to manage the private task catalog.";
      renderTasksPage();
      return;
    }
    if (!quiet) state.tasksView.loading = true;
    state.tasksView.error = "";
    renderTasksPage();
    state.tasksView.trashLoaded = false;
    try {
      const payload = await fetchOrganizationTaskCatalog();
      state.tasksView.items = (Array.isArray(payload?.tasks) ? payload.tasks : []).filter((task) => task && typeof task === "object");
      taskDetailsLoaded.clear();
      state.tasksView.actor = String(payload?.actor || "").toLowerCase();
      state.tasksView.canManage = payload?.canManage === true;
      state.tasksView.members = Array.isArray(payload?.members) ? payload.members : [];
      state.tasksView.departments = Array.isArray(payload?.departments) ? payload.departments : [];
      state.tasksView.teams = Array.isArray(payload?.teams) ? payload.teams : [];
      state.tasksView.serverNow = Number(payload?.serverNow || 0);
      if (!state.tasksView.items.some((task) => task.id === state.tasksView.selectedId)) state.tasksView.selectedId = "";
      applyLinkedTask();
    } catch (error) {
      if (!quiet) state.tasksView.items = [];
      state.tasksView.error = error?.status === 401 || error?.status === 403
        ? "This task catalog is private to current organization members."
        : `Tasks are unavailable: ${taskFriendlyError(error)}`;
    } finally {
      state.tasksView.loading = false;
      renderTasksPage();
      if (!state.tasksView.error) void loadSelectedTaskDetail();
    }
  }

  function fillTaskEditorOptions() {
    const department = $('[data-task-field="department"]');
    const team = $('[data-task-field="team"]');
    const assignee = $('[data-task-field="assignee"]');
    if (department) department.innerHTML = (state.tasksView.departments.length ? state.tasksView.departments : ["general", "engineering"])
      .map((value) => `<option value="${escapeHtml(value)}">${escapeHtml(value.replaceAll("-", " "))}</option>`).join("");
    if (team) team.innerHTML = '<option value="">No specific team</option>' + state.tasksView.teams
      .map((value) => `<option value="${escapeHtml(value)}">${escapeHtml(value)}</option>`).join("");
    if (assignee) assignee.innerHTML = '<option value="unassigned">Unassigned</option><option value="agent">Bot</option>' + state.tasksView.members
      .map((value) => `<option value="user:${escapeHtml(value)}">@${escapeHtml(value)}</option>`).join("");
  }

  function taskField(name) { return $(`[data-task-field="${name}"]`); }

  function openTaskEditor(mode, task = null) {
    const dialog = $("[data-task-editor]");
    const form = $("[data-task-editor-form]");
    if (!dialog || !form) return;
    fillTaskEditorOptions();
    form.reset();
    dialog.dataset.mode = mode;
    dialog.dataset.taskId = task?.id || "";
    const followup = mode === "followup";
    $("[data-task-editor-title]").textContent = mode === "edit" ? "Edit organization task" : followup ? "Create follow-up task" : "New organization task";
    $("[data-task-editor-subtitle]").textContent = followup
      ? "Routing and assignment are inherited securely from the parent task."
      : mode === "edit" ? "Update task copy, assignment, repository, and priority." : "Route work to a member, the shared bot queue, or leave it unassigned.";
    $("[data-task-editor-save]").textContent = mode === "edit" ? "Save changes" : followup ? "Create follow-up" : "Create task";
    for (const item of $$('[data-task-create-only]')) item.hidden = mode === "edit" || followup;
    for (const name of ["assignee", "priority", "repository"]) {
      const field = taskField(name)?.closest("label");
      if (field) field.hidden = followup;
    }
    taskField("title").value = mode === "edit" ? task?.title || "" : "";
    taskField("details").value = mode === "edit" ? task?.details || "" : "";
    taskField("department").value = task?.department || state.tasksView.departments[0] || "general";
    taskField("team").value = task?.team || "";
    taskField("repository").value = task?.repository || (mode === "create" ? "forkmesh/forkmesh" : "");
    taskField("priority").value = String(task?.priority ?? 50);
    taskField("howToTest").value = task?.qa?.howToTest || "";
    const assignment = taskAssigneeKind(task) === "user" ? `user:${task?.assignee || state.tasksView.actor}` : taskAssigneeKind(task);
    taskField("assignee").value = mode === "create" ? "unassigned" : assignment;
    const status = $("[data-task-editor-status]");
    taskFlash(status, "");
    dialog.showModal();
    taskField("title")?.focus();
    window.lucide?.createIcons();
  }

  function fileAsBase64(file) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.addEventListener("load", () => resolve(String(reader.result || "").split(",", 2)[1] || ""), { once: true });
      reader.addEventListener("error", () => reject(new Error("Could not read task image.")), { once: true });
      reader.readAsDataURL(file);
    });
  }

  async function taskEditorAttachments() {
    const files = [...(taskField("attachments")?.files || [])];
    if (files.length > 4) throw new Error("A task can include up to four images.");
    let total = 0;
    return Promise.all(files.map(async (file) => {
      if (!TASK_IMAGE_TYPES.has(file.type)) throw new Error("Tasks accept PNG, JPEG, GIF, or WebP images.");
      if (file.size > 256 * 1024) throw new Error("Each task image must be 256 KiB or smaller.");
      total += file.size;
      if (total > 1024 * 1024) throw new Error("Task images must total 1 MiB or less.");
      const attachment = { name: file.name, mime: file.type, size: file.size, file: await fileAsBase64(file) };
      const thumbnail = await taskImageThumbnail(file);
      if (thumbnail) attachment.thumbnail = thumbnail;
      return attachment;
    }));
  }

  async function taskImageThumbnail(file) {
    if (file.type === "image/gif" || typeof createImageBitmap !== "function") return "";
    try {
      const bitmap = await createImageBitmap(file);
      const side = 48;
      const canvas = document.createElement("canvas");
      canvas.width = side;
      canvas.height = side;
      const scale = Math.max(side / bitmap.width, side / bitmap.height);
      const width = bitmap.width * scale;
      const height = bitmap.height * scale;
      canvas.getContext("2d").drawImage(bitmap, (side - width) / 2, (side - height) / 2, width, height);
      bitmap.close?.();
      const thumbnail = canvas.toDataURL("image/webp", 0.72);
      return thumbnail.length <= 10_000 ? thumbnail : "";
    } catch (_) {
      return "";
    }
  }

  async function saveTaskEditor() {
    const dialog = $("[data-task-editor]");
    const save = $("[data-task-editor-save]");
    const status = $("[data-task-editor-status]");
    const mode = dialog?.dataset.mode || "create";
    const taskId = dialog?.dataset.taskId || "";
    const title = String(taskField("title")?.value || "").trim();
    if (!title) return;
    save.disabled = true; save.textContent = mode === "edit" ? "Saving…" : "Creating…";
    status.hidden = true;
    try {
      let body;
      if (mode === "followup") {
        body = { title, details: String(taskField("details")?.value || "").trim(), parentTaskId: taskId };
      } else {
        const assignment = String(taskField("assignee")?.value || "unassigned");
        const kind = assignment === "agent" ? "agent" : assignment.startsWith("user:") ? "user" : "unassigned";
        body = {
          title,
          details: String(taskField("details")?.value || "").trim(),
          repository: String(taskField("repository")?.value || "").trim(),
          priority: Math.max(1, Math.min(99, Number(taskField("priority")?.value || 50))),
          assigneeKind: kind,
          ...(kind === "user" ? { assignee: assignment.slice(5) } : {}),
        };
        if (mode === "create") Object.assign(body, {
          department: String(taskField("department")?.value || "general"),
          team: String(taskField("team")?.value || ""),
          destination: kind === "agent" ? "agent" : "department",
          howToTest: String(taskField("howToTest")?.value || "").trim(),
          attachments: await taskEditorAttachments(),
        });
      }
      const payload = await taskRequest(mode === "edit" ? "PATCH" : "POST", mode === "edit" ? `/api/tasks/${taskId}` : "/api/tasks", body);
      replaceTask(payload.task);
      taskDetailsLoaded.add(payload.task.id);
      dialog.close();
      state.tasksView.filter = mode === "edit" ? state.tasksView.filter : "all";
      state.tasksView.page = 1;
      taskDetailNotice = { taskId: payload.task.id, place: "actions", message: mode === "edit" ? "Task updated." : mode === "followup" ? "Follow-up task created." : "Task created.", tone: "good" };
      renderTasksPage();
    } catch (error) {
      taskFlash(status, taskFriendlyError(error));
    } finally {
      save.disabled = false;
      save.textContent = mode === "edit" ? "Save changes" : mode === "followup" ? "Create follow-up" : "Create task";
    }
  }

  function selectedTask() {
    const source = state.tasksView.filter === "trash" ? state.tasksView.trash : state.tasksView.items;
    return source.find((task) => task.id === state.tasksView.selectedId) || null;
  }

  function openTaskActionDialog(action, task = selectedTask()) {
    const dialog = $("[data-task-action-dialog]");
    if (!dialog) return;
    const config = {
      complete: ["Complete task", task?.title || "", "Completion note and evidence", task?.completionNote || "", "Complete task", ""],
      qa: ["Send task to QA", task?.title || "", "Exact QA steps", task?.qa?.howToTest || "", "Send to QA", ""],
      delete: ["Move task to trash", task?.title || "", "Reason (optional)", "", "Move to trash", `The task leaves the board now. Restore it from Trash within ${TASK_TRASH_DAYS} days; after that it is purged with its replies, check-ins, QA reviews and attachments.`],
      clear: ["Clear closed tasks", "Move reviewed, completed tasks to trash", "", "", "Move to trash", taskClearCopy()],
    }[action];
    if (!config) return;
    dialog.dataset.action = action;
    dialog.dataset.force = "";
    dialog.dataset.taskId = task?.id || "";
    $("[data-task-action-title]").textContent = config[0];
    $("[data-task-action-subtitle]").textContent = config[1];
    $("[data-task-action-label]").textContent = config[2];
    $("[data-task-action-copy]").value = config[3];
    $("[data-task-action-copy]").closest("label").hidden = !config[2];
    $("[data-task-action-submit]").textContent = config[4];
    $("[data-task-action-submit]").dataset.tone = action === "delete" || action === "clear" ? "danger" : "primary";
    taskFlash($("[data-task-action-warning]"), config[5]);
    taskFlash($("[data-task-action-status]"), "");
    dialog.showModal();
    if (config[2]) $("[data-task-action-copy]").focus();
    window.lucide?.createIcons();
  }

  function taskClearCopy() {
    const clearable = state.tasksView.items.filter(taskClearable).length;
    const waiting = state.tasksView.items.filter((task) => taskIsDone(task) && !taskClearable(task)).length;
    return `${clearable} closed task${clearable === 1 ? "" : "s"} move${clearable === 1 ? "s" : ""} to trash and can be restored for ${TASK_TRASH_DAYS} days.${waiting ? ` ${waiting} awaiting QA stay${waiting === 1 ? "s" : ""} on the board.` : ""}`;
  }

  async function mutateTask(action, task = selectedTask()) {
    if (!task || state.tasksView.busyId) return;
    state.tasksView.busyId = task.id;
    renderTaskDetail();
    try {
      let method = "POST";
      let path = `/api/tasks/${task.id}/${action}`;
      let body = {};
      if (action === "assign") {
        method = "PATCH"; path = `/api/tasks/${task.id}`;
        body = { assigneeKind: "agent", repository: task.repository };
      }
      const payload = await taskRequest(method, path, body);
      replaceTask(payload.task);
      taskDetailsLoaded.add(payload.task.id);
      taskDetailNotice = { taskId: task.id, place: "actions", message: { start: "Task started.", stop: "Task stopped.", complete: "Task completed.", assign: "Task added to the bot queue.", return: "Task returned to the list.", undo: "Task reopened." }[action] || "Task updated.", tone: "good" };
      renderTasksPage();
    } catch (error) {
      taskDetailNotice = { taskId: task.id, place: "actions", message: taskFriendlyError(error), tone: "bad" };
    } finally {
      state.tasksView.busyId = "";
      renderTaskDetail();
    }
  }

  async function submitTaskActionDialog() {
    const dialog = $("[data-task-action-dialog]");
    const action = dialog?.dataset.action;
    const taskId = dialog?.dataset.taskId;
    const submit = $("[data-task-action-submit]");
    const status = $("[data-task-action-status]");
    submit.disabled = true; status.hidden = true;
    try {
      if (action === "clear") {
        const clearable = state.tasksView.items.filter(taskClearable);
        const trashed = [];
        try {
          for (let index = 0; index < clearable.length; index += TASK_DELETE_BATCH) {
            const ids = clearable.slice(index, index + TASK_DELETE_BATCH).map((task) => task.id);
            submit.textContent = `Moving ${Math.min(index + TASK_DELETE_BATCH, clearable.length)} of ${clearable.length}…`;
            const payload = await taskRequest("POST", "/api/tasks/delete", { ids });
            const gone = new Set(Array.isArray(payload.deleted) ? payload.deleted : ids);
            trashed.push(...gone);
            state.tasksView.items = state.tasksView.items.filter((task) => !gone.has(task.id));
          }
        } finally {
          state.tasksView.trashLoaded = false;
          if (!state.tasksView.items.some((task) => task.id === state.tasksView.selectedId)) state.tasksView.selectedId = "";
          if (trashed.length) setTaskListNotice(`Moved ${trashed.length} closed task${trashed.length === 1 ? "" : "s"} to trash.`, "good", trashed);
        }
      } else if (action === "delete") {
        const reason = String($("[data-task-action-copy]").value || "").trim();
        const body = { ...(reason ? { reason } : {}), ...(dialog.dataset.force === "1" ? { force: true } : {}) };
        await taskRequest("DELETE", `/api/tasks/${taskId}`, Object.keys(body).length ? body : null);
        state.tasksView.items = state.tasksView.items.filter((task) => task.id !== taskId);
        state.tasksView.trashLoaded = false;
        if (state.tasksView.selectedId === taskId) state.tasksView.selectedId = "";
        setTaskListNotice("Task moved to trash.", "good", [taskId]);
      } else {
        const field = action === "complete" ? "completionNote" : "howToTest";
        const payload = await taskRequest("POST", `/api/tasks/${taskId}/${action}`, { [field]: String($("[data-task-action-copy]").value || "").trim() });
        replaceTask(payload.task);
        taskDetailsLoaded.add(payload.task.id);
        taskDetailNotice = { taskId, place: "actions", message: action === "complete" ? "Task completed and sent to QA." : "Task sent to QA.", tone: "good" };
      }
      dialog.close();
      renderTasksPage();
    } catch (error) {
      const blocked = taskBlockedReason(error);
      if (action === "delete" && error?.message === "task_delete_blocked") {
        dialog.dataset.force = "1";
        taskFlash(status, `This task is busy: ${blocked}. An organization owner can move it to trash anyway.`);
      } else {
        taskFlash(status, `${taskFriendlyError(error)}${blocked ? ` (${blocked})` : ""}`);
      }
      if (action === "clear") renderTasksPage();
    } finally {
      submit.disabled = false;
      submit.textContent = dialog.dataset.force === "1" ? "Move to trash anyway" : { clear: "Move to trash", delete: "Move to trash", complete: "Complete task", qa: "Send to QA" }[action] || "Continue";
    }
  }

  async function submitTaskReply(form) {
    const input = form.querySelector("[data-task-reply-input]");
    const button = form.querySelector('button[type="submit"]');
    const taskId = form.dataset.taskReplyForm;
    const body = String(input?.value || "").trim();
    if (!body || button.disabled) return;
    button.disabled = true;
    try {
      const payload = await taskRequest("POST", `/api/tasks/${taskId}/replies`, { body });
      taskCommentDrafts.delete(taskId);
      replaceTask(payload.task);
      taskDetailsLoaded.add(payload.task.id);
      taskDetailNotice = { taskId, place: "activity", message: taskPingedCopy("Comment posted", body), tone: "good" };
      renderTasksPage();
    } catch (error) {
      setTaskNotice(taskId, "activity", taskFriendlyError(error), "bad");
    } finally {
      button.disabled = false;
    }
  }

  async function submitTaskCheckin(task, checkinState) {
    if (!task || taskCheckinBusy) return;
    const note = String(taskCheckinNotes.get(task.id) || "").trim();
    taskCheckinBusy = true;
    renderTaskDetail();
    try {
      const payload = await taskRequest("POST", `/api/tasks/${task.id}/checkin`, note ? { state: checkinState, note } : { state: checkinState });
      taskCheckinNotes.delete(task.id);
      taskCheckinOpenId = "";
      if (payload.task) {
        replaceTask(payload.task, false);
        taskDetailsLoaded.add(payload.task.id);
      } else {
        taskDetailsLoaded.delete(task.id);
        void loadSelectedTaskDetail();
      }
      taskDetailNotice = { taskId: task.id, place: "actions", message: taskPingedCopy("Check-in sent", note), tone: "good" };
      renderTasksPage();
    } catch (error) {
      taskDetailNotice = { taskId: task.id, place: "checkin", message: taskFriendlyError(error), tone: "bad" };
    } finally {
      taskCheckinBusy = false;
      renderTaskDetail();
    }
  }

  async function submitTaskPersistentReminder(task, enabled, everyMinutes) {
    if (!task || taskPersistentReminderBusy) return;
    taskPersistentReminderBusy = true;
    renderTaskDetail();
    try {
      const payload = await taskRequest("PATCH", `/api/tasks/${task.id}`, { persistentReminder: enabled ? { enabled: true, everyMinutes } : false });
      if (payload.task) replaceTask(payload.task, false);
      taskDetailNotice = { taskId: task.id, place: "persistent", message: enabled ? "Persistent reminder on." : "Persistent reminder off.", tone: "good" };
      renderTasksPage();
    } catch (error) {
      taskDetailNotice = { taskId: task.id, place: "persistent", message: taskFriendlyError(error), tone: "bad" };
    } finally {
      taskPersistentReminderBusy = false;
      renderTaskDetail();
    }
  }

  async function submitTaskReminder(task) {
    if (!task || taskRemindBusy) return;
    const note = String(taskReminderNotes.get(task.id) || "").trim();
    taskRemindBusy = true;
    renderTaskDetail();
    try {
      const payload = await taskRequest("POST", `/api/tasks/${task.id}/remind`, note ? { note } : {});
      taskReminderNotes.delete(task.id);
      taskRemindOpenId = "";
      if (payload.task) {
        replaceTask(payload.task, false);
        taskDetailsLoaded.add(payload.task.id);
      }
      taskDetailNotice = payload.reminderError
        ? { taskId: task.id, place: "actions", message: `The reminder was saved, but @${task.assignee} could not be pinged.`, tone: "bad" }
        : { taskId: task.id, place: "actions", message: `Reminder sent. @${task.assignee} was pinged to check in.`, tone: "good" };
      renderTasksPage();
    } catch (error) {
      if (error?.payload?.lastReminder) {
        replaceTask({ ...task, lastReminder: error.payload.lastReminder }, false);
      }
      taskDetailNotice = { taskId: task.id, place: "remind", message: taskFriendlyError(error), tone: "bad" };
    } finally {
      taskRemindBusy = false;
      renderTaskDetail();
    }
  }

  async function submitTaskProgress(form) {
    const taskId = form.dataset.taskProgressForm;
    const input = form.querySelector("[data-task-progress-number]");
    const button = form.querySelector('button[type="submit"]');
    const raw = Math.round(Number(input?.value));
    if (!taskId || !Number.isFinite(raw) || button?.disabled) return;
    const progress = Math.max(0, Math.min(100, raw));
    if (button) button.disabled = true;
    try {
      const payload = await taskRequest("PATCH", `/api/tasks/${taskId}`, { progress });
      taskProgressDrafts.delete(taskId);
      replaceTask(payload.task, false);
      taskDetailsLoaded.add(payload.task.id);
      taskDetailNotice = { taskId, place: "progress", message: `Progress set to ${taskProgress(payload.task)}%.`, tone: "good" };
      renderTasksPage();
    } catch (error) {
      setTaskNotice(taskId, "progress", taskFriendlyError(error), "bad");
    } finally {
      if (button) button.disabled = false;
    }
  }

  function taskPromptText(task) {
    const lines = [`[task:${task.id}] ${task.title}`];
    if (task.repository) lines.push(`Repository: ${task.repository}`);
    const branches = taskWorkItems(task).map((item) => item.branch);
    if (branches.length) lines.push(`Branches: ${[...new Set(branches)].join(", ")}`);
    if (task.details) lines.push("", task.details);
    if (task.qa?.howToTest) lines.push("", `How to test: ${task.qa.howToTest}`);
    return lines.join("\n");
  }

  function initTasksPage() {
    const search = $("[data-tasks-search]");
    search?.addEventListener("input", () => {
      state.tasksView.query = search.value;
      state.tasksView.page = 1;
      renderTasksPage();
    });
    const panel = $("[data-tasks-panel]");
    panel?.addEventListener("click", (event) => {
      const person = event.target.closest("[data-tasks-person]");
      if (person) {
        const choice = person.dataset.tasksPerson || "all";
        state.tasksView.assignee = choice === state.tasksView.assignee ? "all" : choice;
        if (state.tasksView.assignee !== "all" && state.tasksView.filter === "mine") {
          state.tasksView.filter = "open";
        }
        state.tasksView.page = 1;
        renderTasksPage();
        return;
      }
      const undo = event.target.closest("[data-tasks-notice-undo]");
      if (undo) {
        void undoTaskDelete(String(undo.dataset.tasksNoticeUndo || "").split(",").filter(Boolean));
        return;
      }
      const filter = event.target.closest("[data-tasks-filter]");
      if (filter) {
        if (filter.dataset.tasksFilter !== state.tasksView.filter) taskListNotice = null;
        state.tasksView.filter = filter.dataset.tasksFilter;
        if (state.tasksView.filter === "trash") state.tasksView.trashLoaded = false;
        if (state.tasksView.filter === "mine") state.tasksView.assignee = "all";
        state.tasksView.page = 1;
        renderTasksPage();
        return;
      }
      const group = event.target.closest("[data-task-group-toggle]");
      if (group) {
        const key = group.dataset.taskGroupToggle;
        if (taskCollapsedGroups.has(key)) taskCollapsedGroups.delete(key);
        else taskCollapsedGroups.add(key);
        renderTasksPage();
        return;
      }
      const selected = event.target.closest("[data-task-select]");
      if (selected) { if (selected.dataset.taskSelect !== state.tasksView.selectedId) taskDetailNotice = null; state.tasksView.selectedId = selected.dataset.taskSelect; renderTasksPage(); void loadSelectedTaskDetail(); return; }
      const page = event.target.closest("[data-tasks-page]");
      if (page) { state.tasksView.page = Number(page.dataset.tasksPage) || 1; renderTasksPage(); return; }
      if (event.target.closest("[data-tasks-prev]")) { state.tasksView.page = Math.max(1, state.tasksView.page - 1); renderTasksPage(); return; }
      if (event.target.closest("[data-tasks-next]")) { state.tasksView.page += 1; renderTasksPage(); return; }
      if (event.target.closest("[data-tasks-refresh]")) { void loadOrganizationTasks(); return; }
      if (event.target.closest("[data-tasks-new]")) { openTaskEditor("create"); return; }
      if (event.target.closest("[data-tasks-delete-closed]")) openTaskActionDialog("clear");
    });

    const detailPane = $("[data-tasks-detail]");
    detailPane?.addEventListener("click", async (event) => {
      const attachment = event.target.closest("[data-task-attachment-link]");
      if (attachment) {
        event.preventDefault();
        void expandTaskAttachment(attachment);
        return;
      }
      const checkinState = event.target.closest("[data-task-checkin-state]");
      if (checkinState) {
        if (!checkinState.disabled) void submitTaskCheckin(selectedTask(), checkinState.dataset.taskCheckinState);
        return;
      }
      if (event.target.closest("[data-task-checkin-cancel]")) {
        taskCheckinOpenId = "";
        renderTaskDetail();
        return;
      }
      const remindSend = event.target.closest("[data-task-remind-send]");
      if (remindSend) {
        if (!remindSend.disabled) void submitTaskReminder(selectedTask());
        return;
      }
      if (event.target.closest("[data-task-remind-cancel]")) {
        taskRemindOpenId = "";
        renderTaskDetail();
        return;
      }
      if (event.target.closest("[data-task-action-menu]")) {
        const id = selectedTask()?.id || "";
        taskActionMenuOpenId = taskActionMenuOpenId === id ? "" : id;
        renderTaskDetail();
        detailPane.querySelector(".task-menu-item:not(:disabled)")?.focus();
        return;
      }
      const button = event.target.closest("[data-task-action]");
      if (!button || button.disabled) return;
      const task = selectedTask();
      const action = button.dataset.taskAction;
      if (taskActionMenuOpenId) {
        taskActionMenuOpenId = "";
        renderTaskDetail();
      }
      if (action === "copy-id") {
        const copied = await copyTextToClipboard(task.id);
        setTaskNotice(task.id, "actions", copied ? "Task ID copied." : "Could not copy the task ID.", copied ? "good" : "bad");
      } else if (action === "checkin") {
        taskCheckinOpenId = taskCheckinOpenId === task?.id ? "" : task?.id || "";
        renderTaskDetail();
        detailPane.querySelector("[data-task-checkin-note]")?.focus();
      } else if (action === "remind") {
        taskRemindOpenId = taskRemindOpenId === task?.id ? "" : task?.id || "";
        renderTaskDetail();
        detailPane.querySelector("[data-task-remind-note]")?.focus();
      } else if (action === "edit") openTaskEditor("edit", task);
      else if (action === "followup") openTaskEditor("followup", task);
      else if (["complete", "qa", "delete"].includes(action)) openTaskActionDialog(action, task);
      else if (action === "restore") void restoreTrashedTask(task);
      else if (action === "copy") {
        const copied = await copyTextToClipboard(taskPromptText(task));
        setTaskNotice(task.id, "actions", copied ? "Task prompt copied." : "Could not copy the task prompt.", copied ? "good" : "bad");
      } else void mutateTask(action, task);
    });
    detailPane?.addEventListener("change", (event) => {
      const control = event.target.closest?.("[data-task-prop]");
      if (control) void submitTaskProperty(control);
    });
    detailPane?.addEventListener("keydown", (event) => {
      const control = event.target.closest?.("input[data-task-prop]");
      if (!control) return;
      if (event.key === "Enter") { event.preventDefault(); control.blur(); }
      else if (event.key === "Escape") { event.preventDefault(); renderTaskDetail(); }
    });
    $("[data-tasks-detail]")?.addEventListener("submit", (event) => {
      const progressForm = event.target.closest("[data-task-progress-form]");
      if (progressForm) {
        event.preventDefault();
        void submitTaskProgress(progressForm);
        return;
      }
      const form = event.target.closest("[data-task-reply-form]");
      if (!form) return;
      event.preventDefault();
      void submitTaskReply(form);
    });
    // @mention suggestions. Picking one happens on mousedown so the field never
    // loses focus (and its caret) to the suggestion button.
    detailPane?.addEventListener("mousedown", (event) => {
      const pick = event.target.closest("[data-task-mention-pick]");
      const input = pick?.closest(".task-mention-field")?.querySelector("[data-task-mention-input]");
      if (!input) return;
      event.preventDefault();
      insertTaskMention(input, pick.dataset.taskMentionPick);
    });
    detailPane?.addEventListener("change", (event) => {
      if (!event.target.closest?.("[data-task-persistent-reminder], [data-task-persistent-reminder-every]")) return;
      const enabled = Boolean(detailPane.querySelector("[data-task-persistent-reminder]")?.checked);
      if (!enabled && event.target.matches("[data-task-persistent-reminder-every]")) return;
      const everyMinutes = Number(detailPane.querySelector("[data-task-persistent-reminder-every]")?.value) || 60;
      void submitTaskPersistentReminder(selectedTask(), enabled, everyMinutes);
    });
    detailPane?.addEventListener("input", (event) => {
      const progressForm = event.target.closest?.("[data-task-progress-form]");
      if (progressForm) {
        if (String(event.target.value ?? "").trim() === "") return;
        const raw = Math.round(Number(event.target.value));
        if (!Number.isFinite(raw)) return;
        const value = Math.max(0, Math.min(100, raw));
        taskProgressDrafts.set(progressForm.dataset.taskProgressForm, value);
        const other = progressForm.querySelector(event.target.matches("[data-task-progress-range]") ? "[data-task-progress-number]" : "[data-task-progress-range]");
        if (other) other.value = String(value);
        return;
      }
      const remindNote = event.target.closest?.("[data-task-remind-note]");
      if (remindNote) {
        const taskId = state.tasksView.selectedId;
        if (remindNote.value) taskReminderNotes.set(taskId, remindNote.value);
        else taskReminderNotes.delete(taskId);
        return;
      }
      const input = event.target.closest?.("[data-task-mention-input]");
      if (!input) return;
      rememberTaskDraft(input);
      const query = taskMentionQueryAt(input);
      if (query !== taskMentionState.query) taskMentionState.index = 0;
      taskMentionState.query = query;
      renderTaskMentionList(input);
    });
    detailPane?.addEventListener("keydown", (event) => {
      const input = event.target.closest?.("[data-task-mention-input]");
      if (!input) return;
      const matches = taskMentionState.query === null ? [] : taskMentionState.matches;
      if (matches.length && (event.key === "ArrowDown" || event.key === "ArrowUp")) {
        event.preventDefault();
        const step = event.key === "ArrowDown" ? 1 : -1;
        taskMentionState.index = (taskMentionState.index + step + matches.length) % matches.length;
        renderTaskMentionList(input);
      } else if (matches.length && (event.key === "Enter" || event.key === "Tab")) {
        event.preventDefault();
        insertTaskMention(input, matches[taskMentionState.index]);
      } else if (matches.length && event.key === "Escape") {
        event.preventDefault();
        taskMentionState.query = null;
        renderTaskMentionList(input);
      } else if (event.key === "Enter" && (event.ctrlKey || event.metaKey) && input.form) {
        event.preventDefault();
        input.form.requestSubmit();
      }
    });
    detailPane?.addEventListener("focusout", (event) => {
      const input = event.target.closest?.("[data-task-mention-input]");
      if (!input) return;
      taskMentionState.query = null;
      renderTaskMentionList(input);
    });
    document.addEventListener("click", (event) => {
      if (!taskActionMenuOpenId || event.target.closest?.(".task-menu-wrap")) return;
      taskActionMenuOpenId = "";
      renderTaskDetail();
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && taskActionMenuOpenId) {
        taskActionMenuOpenId = "";
        renderTaskDetail();
        detailPane?.querySelector("[data-task-action-menu]")?.focus();
        return;
      }
      if (event.key !== "j" && event.key !== "k") return;
      if (event.metaKey || event.ctrlKey || event.altKey || event.target.closest?.("input, textarea, select, [contenteditable], dialog")) return;
      const rows = $$("[data-tasks-list] [data-task-select]");
      if (!rows.length) return;
      const index = rows.findIndex((row) => row.dataset.taskSelect === state.tasksView.selectedId);
      const next = rows[Math.max(0, Math.min(rows.length - 1, index + (event.key === "j" ? 1 : -1)))];
      if (!next || next.dataset.taskSelect === state.tasksView.selectedId) return;
      event.preventDefault();
      next.click();
      $(`[data-tasks-list] [data-task-select="${next.dataset.taskSelect}"]`)?.scrollIntoView({ block: "nearest" });
    });
    $$('[data-task-dialog-close]').forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    $("[data-task-editor-form]")?.addEventListener("submit", (event) => { event.preventDefault(); void saveTaskEditor(); });
    $("[data-task-action-form]")?.addEventListener("submit", (event) => { event.preventDefault(); void submitTaskActionDialog(); });
    void loadOrganizationTasks();
  }
