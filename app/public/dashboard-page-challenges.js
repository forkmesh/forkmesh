// 09-challenges.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["challenges"] = (core) => {
  const { $, copyTextToClipboard, escapeHtml, fetchJson, formatCount, issuePersonNameHtml, loadingHtml, publicProfileUrl, state } = core;
  const CHALLENGE_ERROR_TEXT = {
    unauthorized: "Log in with a user account to take part in challenges.",
    not_found: "That challenge no longer exists.",
    challenge_full: "That challenge is full.",
    challenge_finished: "That challenge has already finished.",
    too_many_owned_challenges: "You already run the maximum number of open challenges. Delete one or wait for it to finish.",
    too_many_joined_challenges: "You are already in the maximum number of open challenges.",
    owner_cannot_leave: "You started this challenge, so delete it instead of leaving.",
    forbidden: "Only the person who started a challenge can delete it.",
    invalid_name: "Give the challenge a name of up to 60 characters.",
    invalid_metric: "Pick what the challenge counts.",
    invalid_weights: "Custom points need at least one kind of work worth 1 to 100 points.",
    invalid_days: "A challenge lasts between 1 and 31 days.",
    invalid_start: "Start today, tomorrow, or on a date within the next 30 days.",
    invalid_daily_cap: "The daily cap must be 0 (no cap) or a positive number.",
    invalid_max_members: "A challenge has room for 2 to 50 people.",
  };

  function challengeAccountName() {
    const session = state.session;
    if (!session?.nodeName || session.kind === "node") return "";
    if (session.kind !== "user" && !session.email) return "";
    return String(session.nodeName).toLowerCase();
  }

  function challengeErrorText(error, fallback) {
    return CHALLENGE_ERROR_TEXT[String(error?.message || "")] || fallback;
  }

  async function challengePost(path, body) {
    const headers = { accept: "application/json", "content-type": "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const response = await fetch(path, {
      method: "POST",
      headers,
      cache: "no-store",
      credentials: "same-origin",
      body: JSON.stringify(body || {}),
    });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      throw error;
    }
    return payload;
  }

  function challengeInviteUrl(item) {
    return `${location.origin}${item.invitePath || `/dashboard/challenges?join=${item.id}`}`;
  }

  function challengeDayLabel(value) {
    const parsed = new Date(`${value}T00:00:00Z`);
    if (Number.isNaN(parsed.getTime())) return String(value || "");
    return parsed.toLocaleDateString(undefined, { month: "short", day: "numeric", timeZone: "UTC" });
  }

  function challengePlural(count, word) {
    const value = Number(count) || 0;
    return `${formatCount(value)} ${word}${value === 1 ? "" : "s"}`;
  }

  function challengeWhenText(item) {
    const range = `${challengeDayLabel(item.startsOn)} - ${challengeDayLabel(item.endsOn)}`;
    if (item.phase === "upcoming") {
      return `${range} · starts in ${challengePlural(item.daysLeft, "day")}`;
    }
    if (item.phase === "active") {
      return `${range} · ${item.daysLeft === 1 ? "last day" : `${formatCount(item.daysLeft)} days left`}`;
    }
    return `${range} · finished`;
  }

  function challengePhaseBadge(phase) {
    const labels = { upcoming: "Starts soon", active: "Live", finished: "Finished" };
    const tone = phase === "active" ? " fm-label-success" : "";
    return `<span class="fm-label${tone} shrink-0">${escapeHtml(labels[phase] || phase)}</span>`;
  }

  function challengeFlashHtml(tone, icon, html, actions = "") {
    const variant = tone ? ` fm-flash-${tone}` : "";
    return `<div class="fm-flash${variant}"><i data-lucide="${icon}" aria-hidden="true"></i><div class="fm-flash-body">${html}</div>${actions ? `<div class="fm-flash-actions">${actions}</div>` : ""}</div>`;
  }

  function challengeBlankslateHtml(icon, title, html) {
    return `<div class="fm-blankslate"><i data-lucide="${icon}" aria-hidden="true"></i><p class="fm-blankslate-title">${title}</p>${html ? `<p class="fm-blankslate-desc">${html}</p>` : ""}</div>`;
  }

  function challengeRulesText(item) {
    const parts = [item.metricLabel || "Code points"];
    if (item.metric === "custom" || item.metric === "points") {
      const weights = item.weights || {};
      const kinds = [["commits", "commit"], ["pulls", "pull request"], ["reviews", "review"], ["issues", "issue"]]
        .filter(([key]) => Number(weights[key]) > 0)
        .map(([key, word]) => `${word} ${formatCount(weights[key])}`);
      if (kinds.length) parts.push(kinds.join(", "));
    }
    parts.push(item.dailyCap ? `${formatCount(item.dailyCap)} point daily cap` : "no daily cap");
    parts.push(`${formatCount(item.memberCount)} of ${formatCount(item.maxMembers)} people`);
    return parts.join(" · ");
  }

  function challengeBarsHtml(entry, days, peak) {
    if (!days.length) return "";
    const bars = entry.days.map((score, index) => {
      const height = peak > 0 ? Math.max(score > 0 ? 12 : 4, Math.round((score / peak) * 100)) : 4;
      const tone = score > 0 ? (entry.you ? "bg-accent" : "bg-muted-foreground") : "bg-secondary";
      return `<span title="${escapeHtml(`${challengeDayLabel(days[index])}: ${formatCount(score)}`)}" class="block flex-1 min-w-0 rounded-sm ${tone}" style="height:${height}%"></span>`;
    }).join("");
    return `<div class="mt-2 flex h-8 items-end gap-0.5" aria-hidden="true">${bars}</div>`;
  }

  function challengeEntryHtml(entry, item, days, peak) {
    const rankTone = entry.rank <= 3 && entry.total > 0 ? "text-foreground" : "text-muted-foreground";
    const counts = entry.counts || {};
    const detail = [
      challengePlural(counts.commits, "commit"),
      challengePlural(counts.pulls, "PR"),
      challengePlural(counts.reviews, "review"),
      challengePlural(counts.issues, "issue"),
    ].join(" · ");
    const today = item.phase === "active"
      ? `<span class="block text-[11px] text-muted-foreground tabular-nums">+${escapeHtml(formatCount(entry.today))} today</span>`
      : "";
    return `<div class="py-3 px-2 flex items-start gap-3 rounded-md${entry.you ? " bg-secondary/60" : ""}">
      <span class="w-6 shrink-0 pt-0.5 font-mono text-sm font-semibold tabular-nums ${rankTone}">${escapeHtml(String(entry.rank))}</span>
      <div class="min-w-0 flex-1">
        <div class="flex items-center gap-2">
          <a class="truncate text-sm font-semibold text-foreground hover:underline" href="${escapeHtml(publicProfileUrl(entry.name))}">${escapeHtml(entry.name)}</a>
          ${entry.you ? '<span class="fm-label shrink-0">you</span>' : ""}
          ${entry.owner ? '<span class="shrink-0 text-[10px] text-muted-foreground">host</span>' : ""}
        </div>
        <p class="mt-0.5 truncate text-[11px] text-muted-foreground">${escapeHtml(detail)}</p>
        ${challengeBarsHtml(entry, days, peak)}
      </div>
      <div class="shrink-0 text-right">
        <span class="block font-mono text-lg font-semibold text-foreground tabular-nums">${escapeHtml(formatCount(entry.total))}</span>
        ${today}
      </div>
    </div>`;
  }

  function challengeCardHtml(item) {
    const board = item.board || { days: [], entries: [], winners: [] };
    const days = Array.isArray(board.days) ? board.days : [];
    const entries = Array.isArray(board.entries) ? board.entries : [];
    const peak = Math.max(0, ...entries.flatMap((entry) => entry.days || []));
    const winners = Array.isArray(board.winners) ? board.winners : [];
    const banner = item.phase === "finished"
      ? `<div class="mt-3">${challengeFlashHtml("neutral", "trophy",
          winners.length ? `${escapeHtml(winners.join(" and "))} ${winners.length === 1 ? "won" : "tied for the win"}.` : "Nobody scored, so nobody won this one.")}</div>`
      : item.phase === "upcoming"
        ? `<div class="mt-3">${challengeFlashHtml("neutral", "clock", "Scoring starts on the first day. Send the invite link so people can join before then.")}</div>`
        : "";
    const action = item.isOwner
      ? `<button data-challenges-delete="${escapeHtml(item.id)}" type="button" class="fm-btn fm-btn-sm fm-btn-danger"><i data-lucide="trash-2" class="w-3.5 h-3.5"></i>Delete</button>`
      : `<button data-challenges-leave="${escapeHtml(item.id)}" type="button" class="fm-btn fm-btn-sm"><i data-lucide="log-out" class="w-3.5 h-3.5"></i>Leave</button>`;
    const invite = item.phase === "finished" ? "" : `<div class="mt-3 flex flex-wrap items-center gap-2">
        <input type="text" readonly aria-label="Invite link for ${escapeHtml(item.name)}" value="${escapeHtml(challengeInviteUrl(item))}" class="h-control-sm min-w-0 flex-1 rounded-md border border-border bg-background px-3 font-mono text-xs text-foreground focus:outline-none focus:ring-1 focus:ring-foreground/20" />
        <button data-challenges-copy="${escapeHtml(item.id)}" type="button" class="fm-btn fm-btn-sm shrink-0"><i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy invite link</button>
      </div>`;
    const rows = entries.length
      ? `<div class="mt-2 divide-y divide-border">${entries.map((entry) => challengeEntryHtml(entry, item, days, peak)).join("")}</div>`
      : "";
    return `<article class="px-4 sm:px-5 py-4">
      <div class="flex flex-wrap items-start justify-between gap-3">
        <div class="min-w-0">
          <div class="flex items-center gap-2">
            <h3 class="truncate text-sm font-semibold text-foreground">${escapeHtml(item.name)}</h3>
            ${challengePhaseBadge(item.phase)}
          </div>
          <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(challengeWhenText(item))}</p>
          <p class="mt-0.5 text-xs text-muted-foreground">${escapeHtml(challengeRulesText(item))}</p>
        </div>
        ${action}
      </div>
      ${invite}
      ${banner}
      ${rows}
    </article>`;
  }

  function renderChallengeNotice() {
    const root = $("[data-challenges-notice]");
    if (!root) return;
    const view = state.challengesView;
    if (!view.notice) {
      root.innerHTML = "";
      return;
    }
    const bad = view.noticeTone === "bad";
    root.innerHTML = `<div class="px-4 sm:px-5 py-3 border-b border-border">${challengeFlashHtml(bad ? "danger" : "success", bad ? "circle-alert" : "circle-check", escapeHtml(view.notice))}</div>`;
    window.lucide?.createIcons();
  }

  function renderChallengeInvite() {
    const root = $("[data-challenges-invite]");
    if (!root) return;
    const view = state.challengesView;
    if (!view.inviteId) {
      root.innerHTML = "";
      return;
    }
    if (view.inviteError || !view.invite) {
      root.innerHTML = `<div class="px-4 sm:px-5 py-4 border-b border-border">${
        view.inviteError
          ? challengeFlashHtml("danger", "circle-alert", escapeHtml(view.inviteError))
          : `<p class="text-sm text-muted-foreground">${loadingHtml("Loading the challenge you were invited to...")}</p>`
      }</div>`;
      window.lucide?.createIcons();
      return;
    }
    const item = view.invite;
    if (item.isMember) {
      root.innerHTML = "";
      return;
    }
    const next = encodeURIComponent(`${location.pathname}${location.search}`);
    const people = Array.isArray(item.memberNames) && item.memberNames.length
      ? `<p class="mt-1 text-xs text-muted-foreground">In it so far: ${item.memberNames.map((name) => issuePersonNameHtml(name, "")).join(", ")}</p>`
      : "";
    const button = item.phase === "finished"
      ? '<span class="text-xs text-muted-foreground">This challenge has finished.</span>'
      : challengeAccountName()
        ? `<button data-challenges-join="${escapeHtml(item.id)}" type="button" class="fm-btn fm-btn-sm fm-btn-cta"${view.busy ? " disabled" : ""}><i data-lucide="swords" class="w-3.5 h-3.5"></i>Accept challenge</button>`
        : `<a class="fm-btn fm-btn-sm fm-btn-cta" href="/login?next=${next}">Log in to accept</a>`;
    root.innerHTML = `<div class="px-4 sm:px-5 py-3 border-b border-border">${challengeFlashHtml("", "swords", `
      <span class="fm-flash-title">${issuePersonNameHtml(item.ownerName, "")} challenged you to ${escapeHtml(item.name)}</span>
      <p class="text-xs">${escapeHtml(challengeWhenText(item))}</p>
      <p class="text-xs">${escapeHtml(challengeRulesText(item))}</p>
      ${people}`, button)}</div>`;
    window.lucide?.createIcons();
  }

  function renderChallengeStart() {
    const root = $("[data-challenges-start]");
    if (!root) return;
    const view = state.challengesView;
    if (!challengeAccountName()) {
      const next = encodeURIComponent(`${location.pathname}${location.search}`);
      root.innerHTML = challengeFlashHtml("", "info", `<a class="text-accent underline" href="/login?next=${next}">Log in</a>
        or <a class="text-accent underline" href="/signup">create an account</a>
        to start a challenge.`);
      window.lucide?.createIcons();
      return;
    }
    const options = view.options;
    if (!options) {
      root.innerHTML = `<p class="text-sm text-muted-foreground">${view.loading ? loadingHtml("Loading...") : ""}</p>`;
      return;
    }
    const presets = options.presets || [];
    const primary = presets.find((preset) => preset.id === options.defaultPreset) || presets[0];
    const others = presets.filter((preset) => preset !== primary);
    const disabled = view.busy ? " disabled" : "";
    root.innerHTML = `<div class="flex flex-col gap-3">
      ${primary ? `<div class="rounded-md border border-border bg-background p-3 flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3">
        <div class="min-w-0">
          <p class="text-sm font-semibold text-foreground">${escapeHtml(primary.label)}</p>
          <p class="mt-0.5 text-xs text-muted-foreground">${escapeHtml(primary.description)} Everything is set up; you only share the link.</p>
        </div>
        <button data-challenges-preset="${escapeHtml(primary.id)}" type="button" class="fm-btn fm-btn-cta shrink-0"${disabled}>
          <i data-lucide="zap" class="w-4 h-4"></i>
          Start in one click
        </button>
      </div>` : ""}
      <div class="flex flex-wrap items-center gap-2">
        ${others.map((preset) => `<button data-challenges-preset="${escapeHtml(preset.id)}" type="button" title="${escapeHtml(preset.description)}" class="fm-btn fm-btn-sm"${disabled}>${escapeHtml(preset.label)}</button>`).join("")}
        <button data-challenges-customize type="button" aria-expanded="${view.customizing ? "true" : "false"}" class="fm-btn fm-btn-sm">
          <i data-lucide="sliders-horizontal" class="w-3.5 h-3.5"></i>
          ${view.customizing ? "Hide custom options" : "Customize"}
        </button>
      </div>
    </div>`;
  }

  function challengeFieldClass() {
    return "mt-1 w-full rounded-md border border-border bg-background px-3 py-2 text-sm text-foreground focus:outline-none focus:ring-1 focus:ring-foreground/20";
  }

  function renderChallengeForm() {
    const form = $("[data-challenges-form]");
    if (!form) return;
    const view = state.challengesView;
    form.hidden = !(view.customizing && view.options && challengeAccountName());
    if (form.hidden || form.dataset.ready === "1") return;
    form.dataset.ready = "1";
    const options = view.options;
    const limits = options.limits || {};
    const field = challengeFieldClass();
    const metrics = (options.metrics || []).map((metric) =>
      `<option value="${escapeHtml(metric.id)}">${escapeHtml(metric.label)} - ${escapeHtml(metric.description)}</option>`).join("");
    const weights = (options.kinds || []).map((kind) => `<label class="text-xs text-muted-foreground">${escapeHtml(kind.label)}
        <input name="weight-${escapeHtml(kind.id)}" type="number" min="0" max="${Number(limits.maxWeight) || 100}" value="${kind.id === "commits" ? 1 : 0}" class="${field}" />
      </label>`).join("");
    form.innerHTML = `<label class="text-xs text-muted-foreground sm:col-span-2">Name
        <input name="name" type="text" required maxlength="${Number(limits.maxName) || 60}" value="${escapeHtml(`${challengeAccountName()}'s code challenge`)}" class="${field}" />
      </label>
      <label class="text-xs text-muted-foreground sm:col-span-2">What counts
        <select name="metric" class="${field}">${metrics}</select>
      </label>
      <fieldset data-challenges-weights class="sm:col-span-2 grid gap-3 grid-cols-2 sm:grid-cols-4" hidden>
        <legend class="mb-1 text-xs text-muted-foreground">Points for each</legend>
        ${weights}
      </fieldset>
      <label class="text-xs text-muted-foreground">Length in days
        <input name="days" type="number" min="${Number(limits.minDays) || 1}" max="${Number(limits.maxDays) || 31}" value="7" required class="${field}" />
      </label>
      <label class="text-xs text-muted-foreground">Starts
        <select name="start" class="${field}">
          <option value="today">Today</option>
          <option value="tomorrow">Tomorrow</option>
          <option value="date">On a date...</option>
        </select>
      </label>
      <label data-challenges-start-date class="text-xs text-muted-foreground" hidden>Start date (UTC)
        <input name="startDate" type="date" min="${escapeHtml(view.today)}" class="${field}" />
      </label>
      <label class="text-xs text-muted-foreground">Daily point cap (0 for none)
        <input name="dailyCap" type="number" min="0" max="${Number(limits.maxDailyCap) || 100000}" value="0" class="${field}" />
      </label>
      <label class="text-xs text-muted-foreground">Most people
        <input name="maxMembers" type="number" min="${Number(limits.minMembers) || 2}" max="${Number(limits.maxMembers) || 50}" value="10" class="${field}" />
      </label>
      <div class="sm:col-span-2 flex justify-end">
        <button type="submit" class="fm-btn">
          <i data-lucide="swords" class="w-4 h-4"></i>
          Start custom challenge
        </button>
      </div>`;
    syncChallengeForm(form);
  }

  function syncChallengeForm(form) {
    const metric = form.elements.metric?.value;
    const weights = form.querySelector("[data-challenges-weights]");
    if (weights) weights.hidden = metric !== "custom";
    const startDate = form.querySelector("[data-challenges-start-date]");
    if (startDate) startDate.hidden = form.elements.start?.value !== "date";
  }

  function readChallengeForm(form) {
    const number = (name) => Number.parseInt(form.elements[name]?.value || "0", 10) || 0;
    const settings = {
      name: String(form.elements.name?.value || "").trim(),
      metric: form.elements.metric?.value || "points",
      days: number("days"),
      start: form.elements.start?.value === "date"
        ? String(form.elements.startDate?.value || "")
        : form.elements.start?.value || "today",
      dailyCap: number("dailyCap"),
      maxMembers: number("maxMembers"),
    };
    if (settings.metric === "custom") {
      settings.weights = {};
      for (const kind of state.challengesView.options?.kinds || []) {
        settings.weights[kind.id] = number(`weight-${kind.id}`);
      }
    }
    return settings;
  }

  function renderChallengeList() {
    const root = $("[data-challenges-list]");
    const count = $("[data-challenges-count]");
    if (!root) return;
    const view = state.challengesView;
    if (!challengeAccountName()) {
      root.innerHTML = challengeBlankslateHtml("lock", "Signed out", "Log in to see the challenges you are in.");
      window.lucide?.createIcons();
      if (count) count.textContent = "Signed out";
      return;
    }
    if (view.loading || view.error) {
      root.innerHTML = view.loading
        ? `<div class="px-4 sm:px-5 py-8 text-sm text-muted-foreground">${loadingHtml("Loading challenges...")}</div>`
        : challengeBlankslateHtml("circle-alert", "Challenges did not load", escapeHtml(view.error));
      window.lucide?.createIcons();
      if (count) count.textContent = view.loading ? "Loading" : "Unavailable";
      return;
    }
    const live = view.items.filter((item) => item.phase !== "finished").length;
    if (count) count.textContent = `${formatCount(live)} open`;
    root.innerHTML = view.items.length
      ? view.items.map(challengeCardHtml).join("")
      : challengeBlankslateHtml("swords", "No challenges yet", "Start one above and send the invite link to the people you want to race.");
  }

  function renderChallengesPage() {
    renderChallengeNotice();
    renderChallengeInvite();
    renderChallengeStart();
    renderChallengeForm();
    renderChallengeList();
    window.lucide?.createIcons();
  }

  function setChallengeNotice(text, tone = "good") {
    state.challengesView.notice = text;
    state.challengesView.noticeTone = tone;
  }

  function upsertChallenge(item) {
    const view = state.challengesView;
    view.items = [item, ...view.items.filter((existing) => existing.id !== item.id)];
  }

  async function loadChallenges(options = {}) {
    const view = state.challengesView;
    if (!challengeAccountName()) {
      view.loading = false;
      renderChallengesPage();
      return;
    }
    view.loading = true;
    view.error = "";
    renderChallengesPage();
    try {
      const data = await fetchJson("/api/challenges", { fresh: options.fresh === true });
      view.items = Array.isArray(data.challenges) ? data.challenges : [];
      view.options = data.options || null;
      view.today = String(data.today || "");
    } catch (error) {
      view.items = [];
      view.error = error?.status === 401
        ? CHALLENGE_ERROR_TEXT.unauthorized
        : "Your challenges are unavailable right now.";
    } finally {
      view.loading = false;
      renderChallengesPage();
    }
  }

  async function loadChallengeInvite(options = {}) {
    const view = state.challengesView;
    if (!view.inviteId) return;
    view.inviteError = "";
    try {
      const data = await fetchJson(`/api/challenges/${view.inviteId}`, { fresh: options.fresh === true });
      view.invite = data.challenge || null;
    } catch (error) {
      view.invite = null;
      view.inviteError = challengeErrorText(error, "That challenge link could not be opened.");
    }
    renderChallengesPage();
  }

  async function createChallenge(body) {
    const view = state.challengesView;
    view.busy = true;
    renderChallengesPage();
    try {
      const data = await challengePost("/api/challenges", body);
      upsertChallenge(data.challenge);
      view.customizing = false;
      setChallengeNotice(`"${data.challenge.name}" is on. Copy its invite link below and send it to the people you want to race.`);
    } catch (error) {
      setChallengeNotice(challengeErrorText(error, "The challenge could not be started. Please try again."), "bad");
    } finally {
      view.busy = false;
      renderChallengesPage();
    }
  }

  async function challengeAction(id, action) {
    const view = state.challengesView;
    view.busy = true;
    try {
      const data = await challengePost(`/api/challenges/${id}/${action}`, {});
      if (action === "join") {
        upsertChallenge(data.challenge);
        view.invite = data.challenge;
        setChallengeNotice(`You are in "${data.challenge.name}". Good luck.`);
        if (new URLSearchParams(location.search).has("join")) {
          window.history.replaceState(null, "", location.pathname);
        }
        view.inviteId = "";
      } else {
        view.items = view.items.filter((item) => item.id !== id);
        setChallengeNotice(action === "delete" ? "Challenge deleted." : "You left the challenge.");
      }
    } catch (error) {
      setChallengeNotice(challengeErrorText(error, "That did not work. Please try again."), "bad");
    } finally {
      view.busy = false;
      renderChallengesPage();
    }
  }

  function initChallengesPage() {
    const view = state.challengesView;
    const joinId = new URLSearchParams(location.search).get("join") || "";
    view.inviteId = /^[0-9a-f]{32}$/.test(joinId) ? joinId : "";
    const panel = $("[data-challenges-panel]");
    panel?.addEventListener("click", async (event) => {
      if (event.target.closest("[data-challenges-refresh]")) {
        void loadChallenges({ fresh: true });
        void loadChallengeInvite({ fresh: true });
        return;
      }
      if (view.busy) return;
      const preset = event.target.closest("[data-challenges-preset]");
      if (preset) {
        void createChallenge({ preset: preset.dataset.challengesPreset });
        return;
      }
      if (event.target.closest("[data-challenges-customize]")) {
        view.customizing = !view.customizing;
        renderChallengesPage();
        return;
      }
      const join = event.target.closest("[data-challenges-join]");
      if (join) {
        void challengeAction(join.dataset.challengesJoin, "join");
        return;
      }
      const leave = event.target.closest("[data-challenges-leave]");
      if (leave) {
        if (window.confirm("Leave this challenge? Your score stops showing on its board.")) {
          void challengeAction(leave.dataset.challengesLeave, "leave");
        }
        return;
      }
      const remove = event.target.closest("[data-challenges-delete]");
      if (remove) {
        if (window.confirm("Delete this challenge for everyone in it?")) {
          void challengeAction(remove.dataset.challengesDelete, "delete");
        }
        return;
      }
      const copy = event.target.closest("[data-challenges-copy]");
      if (copy) {
        const item = view.items.find((entry) => entry.id === copy.dataset.challengesCopy);
        const copied = item ? await copyTextToClipboard(challengeInviteUrl(item)) : false;
        copy.innerHTML = copied
          ? '<i data-lucide="check" class="w-3.5 h-3.5"></i>Copied'
          : '<i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy failed';
        window.lucide?.createIcons();
        window.setTimeout(() => {
          if (!document.body.contains(copy)) return;
          copy.innerHTML = '<i data-lucide="copy" class="w-3.5 h-3.5"></i>Copy invite link';
          window.lucide?.createIcons();
        }, 1600);
      }
    });
    const form = $("[data-challenges-form]");
    form?.addEventListener("change", () => syncChallengeForm(form));
    form?.addEventListener("submit", (event) => {
      event.preventDefault();
      if (view.busy) return;
      void createChallenge(readChallengeForm(form));
    });
    void loadChallenges();
    void loadChallengeInvite();
  }
  return { initChallengesPage };
  };
})();
