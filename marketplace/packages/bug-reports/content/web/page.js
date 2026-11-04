window.ForkMeshDashboard.registerPage("bug-reports", ({ select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel }) => {
  const STATUSES = ["new", "triaged", "closed"];
  const INBOX_LABELS = { new: "New", triaged: "Triaged", closed: "Closed" };
  const MINE_LABELS = { new: "Received", triaged: "In progress", closed: "Closed" };
  const state = { tab: "report", admin: false, filter: "new", mine: null, inbox: null, signedOut: false, busy: "" };

  function headers(json) {
    const result = { accept: "application/json" };
    if (json) result["content-type"] = "application/json";
    const token = session()?.sessionToken || "";
    if (token && token !== "cookie") result.authorization = `Bearer ${token}`;
    return result;
  }

  async function call(path, body) {
    const response = await fetch(path, {
      method: body ? "POST" : "GET",
      cache: "no-store",
      credentials: "same-origin",
      headers: headers(Boolean(body)),
      body: body ? JSON.stringify(body) : undefined,
    });
    const data = await response.json().catch(() => ({}));
    if (!response.ok || data.ok === false) {
      const error = new Error(data.error || `HTTP ${response.status}`);
      error.status = response.status;
      throw error;
    }
    return data;
  }

  function showError(text) {
    const box = $("[data-bugs-error]");
    if (!box) return;
    $("[data-bugs-error-text]").textContent = text || "";
    box.classList.toggle("hidden", !text);
  }

  function setStatus(text, tone = "") {
    const status = $("[data-bugs-status]");
    if (!status) return;
    status.textContent = text || "";
    status.className = `min-h-4 text-xs ${tone === "bad" ? "text-destructive" : tone === "good" ? "text-success" : "text-muted-foreground"}`;
  }

  function showTab(tab) {
    if (tab === "reported" && !state.admin) tab = "mine";
    state.tab = tab;
    $$("[data-bugs-tab]").forEach((button) => button.setAttribute("aria-pressed", button.dataset.bugsTab === tab ? "true" : "false"));
    $$("[data-bugs-pane]").forEach((pane) => pane.classList.toggle("hidden", pane.dataset.bugsPane !== tab));
    if (tab === "mine" && !state.mine) void loadMine();
    if (tab === "reported" && !state.inbox) void loadInbox();
  }

  function renderTabs() {
    const reported = $('[data-bugs-tab="reported"]');
    reported?.classList.toggle("hidden", !state.admin);
    const open = (state.mine?.counts?.new || 0) + (state.mine?.counts?.triaged || 0);
    const mine = $('[data-bugs-tab="mine"]');
    if (mine) mine.textContent = open ? `My reports (${open})` : "My reports";
    if (reported) reported.textContent = state.inbox?.counts?.new ? `Reported (${state.inbox.counts.new})` : "Reported";
    if (!state.admin && state.tab === "reported") showTab("mine");
  }

  function blank(title, text) {
    return `<div class="fm-blankslate"><i data-lucide="bug"></i><h2 class="fm-blankslate-title">${escapeHtml(title)}</h2><p class="fm-blankslate-desc">${escapeHtml(text)}</p></div>`;
  }

  function details(report, inbox) {
    const parts = [report.platform, report.appVersion, report.screen].filter(Boolean);
    const files = Array.isArray(report.attachments) ? report.attachments.length : 0;
    if (files) parts.push(files === 1 ? "1 attachment" : `${files} attachments`);
    if (inbox && report.package) parts.push(`plugin ${report.package}`);
    if (inbox && report.contact) parts.push(`contact ${report.contact}`);
    return parts.join(" · ");
  }

  function card(report, inbox, index) {
    const confirmed = Number(report.confirmedFixedAt) > 0;
    const label = inbox ? INBOX_LABELS[report.status] || report.status : confirmed ? "Fixed, confirmed by you" : MINE_LABELS[report.status] || report.status;
    const created = Number(report.createdAt) || 0;
    const busy = state.busy === report.id;
    const actions = inbox
      ? STATUSES.filter((status) => status !== report.status).map((status) => `<button type="button" class="fm-btn fm-btn-sm" data-bugs-set="${status}" data-bugs-report="${escapeHtml(report.id)}"${busy ? " disabled" : ""}>${status === "closed" ? "Close" : `Mark ${INBOX_LABELS[status].toLowerCase()}`}</button>`).join("")
      : confirmed ? "" : `<button type="button" class="fm-btn fm-btn-sm" data-bugs-fixed data-bugs-report="${escapeHtml(report.id)}"${busy ? " disabled" : ""}>Confirm it's fixed</button>`;
    const severity = report.severity === "crash" ? "Crash" : report.severity === "high" ? "Serious" : "";
    const info = details(report, inbox);
    return `<article class="px-4 py-3${index ? " border-t border-border" : ""}">
      <div class="flex flex-wrap items-baseline gap-x-2 gap-y-1">
        <span class="fm-label">${escapeHtml(label)}</span>
        ${severity ? `<span class="fm-label text-destructive">${severity}</span>` : ""}
        ${inbox && report.account ? `<strong class="text-sm text-foreground">${escapeHtml(report.account)}</strong>` : ""}
        <time class="ml-auto text-xs text-muted-foreground" title="${created ? escapeHtml(new Date(created).toLocaleString()) : ""}">${created ? escapeHtml(relativeTimeLabel(created)) : ""}</time>
      </div>
      <p class="mt-2 whitespace-pre-wrap text-sm leading-6 text-foreground">${escapeHtml(report.message || "")}</p>
      ${info ? `<p class="mt-1 text-xs text-muted-foreground">${escapeHtml(info)}</p>` : ""}
      ${actions ? `<footer class="mt-2 flex flex-wrap justify-end gap-2">${actions}</footer>` : ""}
    </article>`;
  }

  function renderMine() {
    const list = $("[data-bugs-mine]");
    if (!list) return;
    if (state.signedOut) list.innerHTML = blank("Sign in to follow your reports", "Reports you send while signed in show here with their status.");
    else if (!state.mine) list.innerHTML = '<div class="px-4 py-8 text-sm text-muted-foreground"><span class="fm-spinner"></span>Loading your reports…</div>';
    else if (!state.mine.reports.length) list.innerHTML = blank("No reports yet", "Bugs you report from any device show here, with what the team did about them.");
    else list.innerHTML = state.mine.reports.map((report, index) => card(report, false, index)).join("");
    window.lucide?.createIcons?.();
  }

  function renderInbox() {
    const list = $("[data-bugs-inbox]");
    if (!list) return;
    if (!state.inbox) list.innerHTML = '<div class="px-4 py-8 text-sm text-muted-foreground"><span class="fm-spinner"></span>Loading reported bugs…</div>';
    else if (!state.inbox.reports.length) list.innerHTML = blank("Nothing here", state.filter === "new" ? "Every reported bug has been looked at." : "No reports match this filter.");
    else list.innerHTML = state.inbox.reports.map((report, index) => card(report, true, index)).join("");
    window.lucide?.createIcons?.();
  }

  async function loadMine() {
    try {
      const data = await call("/api/bug-reports/mine");
      state.signedOut = false;
      state.mine = { reports: Array.isArray(data.reports) ? data.reports : [], counts: data.counts || {} };
      showError("");
    } catch (error) {
      state.signedOut = error.status === 401;
      if (!state.signedOut) showError("Your reports could not be loaded. Try again in a moment.");
      state.mine = state.signedOut ? null : state.mine;
    }
    renderMine();
    renderTabs();
  }

  async function loadInbox() {
    const query = state.filter ? `?status=${encodeURIComponent(state.filter)}` : "";
    try {
      const data = await call(`/api/bug-reports/inbox${query}`);
      state.admin = true;
      state.inbox = { reports: Array.isArray(data.reports) ? data.reports : [], counts: data.counts || {} };
    } catch (error) {
      if (error.status === 403 || error.status === 401) {
        state.admin = false;
        state.inbox = null;
      } else if (state.admin) {
        showError("Reported bugs could not be loaded. Try again in a moment.");
      }
    }
    renderInbox();
    renderTabs();
  }

  async function submit(event) {
    event.preventDefault();
    const field = $("[data-bugs-message]");
    const send = $("[data-bugs-send]");
    const message = String(field?.value || "").trim();
    if (message.length < 4) {
      setStatus("Write a few words about what went wrong.", "bad");
      field?.focus();
      return;
    }
    const contact = String($("[data-bugs-contact]")?.value || "").trim();
    const body = {
      message,
      severity: $("[data-bugs-severity]")?.value || "normal",
      source: "web",
      platform: "web",
      screen: "Bugs",
      device: String(navigator.userAgent || "").slice(0, 120),
    };
    if (contact) body.contact = contact;
    if (send) send.disabled = true;
    setStatus("Sending…");
    try {
      await call("/api/bug-reports", body);
      $("[data-bugs-form]")?.reset();
      setStatus("Thanks. Your report reached the ForkMesh team.", "good");
      void loadMine();
      if (state.admin) void loadInbox();
    } catch (error) {
      setStatus(error.status === 429 ? "Too many reports from here in the last hour. Try again later." : "The report could not be sent. Try again in a moment.", "bad");
    } finally {
      if (send) send.disabled = false;
    }
  }

  async function confirmFixed(id) {
    state.busy = id;
    renderMine();
    try {
      const data = await call("/api/bug-reports/mine", { report: id, fixed: true });
      const updated = data.report;
      state.mine.reports = state.mine.reports.map((report) => report.id === id ? (updated?.id === id ? updated : { ...report, status: "closed", confirmedFixedAt: Date.now() }) : report);
      if (data.counts) state.mine.counts = data.counts;
      showError("");
    } catch (_) {
      showError("The fix could not be confirmed. Try again in a moment.");
    }
    state.busy = "";
    renderMine();
    renderTabs();
  }

  async function setReportStatus(id, status) {
    state.busy = id;
    renderInbox();
    try {
      await call("/api/bug-reports/inbox", { report: id, status });
      showError("");
    } catch (_) {
      showError("The report could not be changed. Try again in a moment.");
    }
    state.busy = "";
    await loadInbox();
  }

  $$("[data-bugs-tab]").forEach((button) => button.addEventListener("click", () => showTab(button.dataset.bugsTab)));
  $("[data-bugs-form]")?.addEventListener("submit", (event) => { void submit(event); });
  $("[data-bugs-refresh]")?.addEventListener("click", () => { void loadMine(); void loadInbox(); });
  $("[data-bugs-filter]")?.addEventListener("change", (event) => {
    state.filter = String(event.target.value || "");
    state.inbox = null;
    renderInbox();
    void loadInbox();
  });
  $("[data-bugs-mine]")?.addEventListener("click", (event) => {
    const button = event.target.closest("[data-bugs-fixed]");
    if (button) void confirmFixed(button.dataset.bugsReport);
  });
  $("[data-bugs-inbox]")?.addEventListener("click", (event) => {
    const button = event.target.closest("[data-bugs-set]");
    if (button) void setReportStatus(button.dataset.bugsReport, button.dataset.bugsSet);
  });
  renderMine();
  void loadMine();
  void loadInbox();
});
