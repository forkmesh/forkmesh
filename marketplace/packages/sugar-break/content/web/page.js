window.ForkMeshDashboard.registerPage("sugar-break", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const LENGTH_NAMES = { 7: "1 week", 14: "2 weeks", 21: "3 weeks", 30: "1 month", 60: "2 months", 90: "3 months" };
  const STATUS_CLASS = { "sugar-free": "sugar-day-free", slip: "sugar-day-slip", open: "sugar-day-open", upcoming: "sugar-day-upcoming" };
  const STATUS_LABEL = { "sugar-free": "sugar-free", slip: "had sugar", open: "nothing logged", upcoming: "to come" };
  const state = { installed: false, overview: null, day: localDay(new Date()) };
  const currentSession = () => session();

  function localDay(date) {
    const d = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
    return d.toISOString().slice(0, 10);
  }
  function shortDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { month: "short", day: "numeric" }); }
  function longDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" }); }
  function dayOfMonth(day) { return Number(String(day).slice(8, 10)); }
  function plural(count, one, many) { return `${count} ${count === 1 ? one : many}`; }
  function lengthLabel(days) {
    const value = Number(days || 0);
    return LENGTH_NAMES[value] ? `${LENGTH_NAMES[value]} (${value} days)` : plural(value, "day", "days");
  }
  function message(kind, value = "") {
    const node = $(`[data-sugar-${kind}]`); if (!node) return;
    node.textContent = value; node.classList.toggle("hidden", !value);
  }
  function apiPath(suffix = "") {
    state.day = localDay(new Date());
    return `/api/sugar-break${suffix}${suffix.includes("?") ? "&" : "?"}day=${state.day}`;
  }
  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    if (currentSession()?.sessionToken) headers.authorization = `Bearer ${currentSession().sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }

  function renderInstall() {
    $(`[data-sugar-install-panel]`)?.classList.toggle("hidden", state.installed);
    $(`[data-sugar-app]`)?.classList.toggle("hidden", !state.installed);
    $$(`[data-sugar-requires-install]`).forEach((node) => node.classList.toggle("hidden", !state.installed));
  }
  function resultLine(item, summary) {
    const days = `${summary.sugarFreeDays || 0} of ${item.lengthDays || summary.lengthDays || 0} days sugar-free`;
    if (item.status === "completed") return `Completed: ${days}`;
    if (item.status === "ended") return `Ended early${item.endedDay ? ` on ${shortDay(item.endedDay)}` : ""}: ${days}`;
    return `In progress: ${days}`;
  }
  function progressSvg(percent) {
    const ratio = Math.max(0, Math.min(100, Number(percent || 0))) / 100;
    return `<svg viewBox="0 0 300 14" role="img" aria-label="${escapeHtml(`${Math.round(ratio * 100)}% of the break sugar-free`)}">
      <rect x="0" y="0" width="300" height="14" rx="7" fill="var(--sugar-track)"/>
      ${ratio > 0 ? `<rect x="0" y="0" width="${Math.max(14, 300 * ratio).toFixed(1)}" height="14" rx="7" fill="var(--sugar-bar)"/>` : ""}
    </svg>`;
  }
  function renderStart(overview) {
    const profile = overview.profile || {};
    const preferred = Number(profile.defaultLengthDays || 30);
    const lengths = [...new Set([...(overview.lengths || [7, 14, 21, 30, 60, 90]).map(Number), preferred])].sort((a, b) => a - b);
    const select = $(`[data-sugar-length]`);
    const chosen = Number(select.value || 0) || preferred;
    select.innerHTML = lengths.map((days) => `<option value="${days}">${escapeHtml(lengthLabel(days))}</option>`).join("");
    select.value = String(lengths.includes(chosen) ? chosen : preferred);
    const current = overview.current;
    const result = $(`[data-sugar-last-result]`);
    const line = current && current.status !== "active" ? resultLine(current, overview.summary || {}) : "";
    result.textContent = line; result.classList.toggle("hidden", !line);
  }
  function renderActive(overview) {
    const current = overview.current || {}, summary = overview.summary || {}, today = overview.today || {};
    const length = Number(current.lengthDays || summary.lengthDays || 0);
    $(`[data-sugar-day-label]`).textContent = `Day ${summary.dayNumber || 0} of ${length}`;
    $(`[data-sugar-range]`).textContent = `${longDay(current.startDay)} – ${longDay(current.endDay)}`;
    $(`[data-sugar-progress]`).innerHTML = progressSvg(summary.percent);
    $(`[data-sugar-progress-text]`).textContent = `${summary.sugarFreeDays || 0} of ${length} days sugar-free · ${summary.percent || 0}%`;
    const open = today.inBreak !== false;
    $(`[data-sugar-skip]`).disabled = !open;
    $(`[data-sugar-slip]`).disabled = !open;
    const skips = Number(today.skips || 0), slips = Number(today.slips || 0);
    const status = today.status === "slip" ? "had sugar" : today.status === "sugar-free" ? "sugar-free so far" : "nothing logged yet";
    $(`[data-sugar-today]`).textContent = `+${skips} · ${plural(slips, "slip", "slips")} · ${status}`;
    const events = [...(overview.events || [])].reverse();
    $(`[data-sugar-events]`).innerHTML = events.length ? events.map((event) => `<div class="flex items-center justify-between gap-2 rounded-md px-2 py-1.5 hover:bg-secondary">
        <span class="text-sm text-foreground">${escapeHtml(new Date(Number(event.at || event.loggedAt || 0)).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" }))} · ${event.kind === "slip" ? "Had sugar" : "+1 Skipped sugar"}</span>
        <button type="button" data-sugar-undo="${escapeHtml(event.id)}" class="text-xs text-muted-foreground hover:text-destructive">Undo</button>
      </div>`).join("") : `<p class="text-sm text-muted-foreground">Nothing logged today. Tap +1 each time you skip sugar.</p>`;
    const stats = [
      ["+1 total", String(summary.skips || 0), plural(Number(summary.slips || 0), "slip", "slips")],
      ["Sugar-free days", `${summary.sugarFreeDays || 0} of ${length}`, `${summary.percent || 0}%`],
      ["Streak", plural(Number(summary.streak || 0), "day", "days"), "in a row, up to today"],
      ["Best streak", plural(Number(summary.bestStreak || 0), "day", "days"), "this break"],
      ["Days left", String(summary.remainingDays || 0), `ends ${shortDay(current.endDay)}`],
    ];
    $(`[data-sugar-stats]`).innerHTML = stats.map(([label, value, note]) => `<div><div class="text-xs text-muted-foreground">${escapeHtml(label)}</div><div class="text-base font-semibold text-foreground">${escapeHtml(value)}</div><div class="text-xs text-muted-foreground">${escapeHtml(note)}</div></div>`).join("");
    $(`[data-sugar-days]`).innerHTML = (overview.days || []).map((day) => {
      const counts = [day.skips ? `+${day.skips}` : "", day.slips ? plural(day.slips, "slip", "slips") : ""].filter(Boolean).join(", ");
      const title = `${longDay(day.day)}: ${STATUS_LABEL[day.status] || day.status}${counts ? ` · ${counts}` : ""}`;
      const classes = ["sugar-day", STATUS_CLASS[day.status] || "sugar-day-open", day.day === state.day ? "sugar-day-today" : ""].filter(Boolean).join(" ");
      return `<div class="${classes}" title="${escapeHtml(title)}" aria-label="${escapeHtml(title)}">${dayOfMonth(day.day)}${day.skips ? `<span>+${Number(day.skips)}</span>` : ""}</div>`;
    }).join("");
  }
  function renderHistory(overview) {
    const history = overview.history || [];
    $(`[data-sugar-history-panel]`).classList.toggle("hidden", !history.length);
    $(`[data-sugar-history]`).innerHTML = history.map((item) => {
      const summary = item.summary || {};
      return `<div class="flex flex-wrap items-center justify-between gap-2 rounded-md px-2 py-1.5 hover:bg-secondary">
        <span class="text-sm text-foreground">${escapeHtml(`${shortDay(item.startDay)} – ${shortDay(item.endedDay || item.endDay)} · ${lengthLabel(item.lengthDays)}`)}</span>
        <span class="text-xs text-muted-foreground">${escapeHtml(`${resultLine(item, summary)} · +${summary.skips || 0}`)}</span>
      </div>`;
    }).join("");
  }
  function render() {
    const overview = state.overview || {};
    const active = overview.current?.status === "active";
    $(`[data-sugar-start-panel]`).classList.toggle("hidden", active);
    $(`[data-sugar-active-panel]`).classList.toggle("hidden", !active);
    renderStart(overview);
    if (active) renderActive(overview);
    renderHistory(overview);
  }
  async function load() {
    try {
      const payload = await request(apiPath()); state.installed = payload.installed !== false;
      state.overview = state.installed ? payload : null; message("error", ""); renderInstall(); if (state.installed) render();
    } catch (error) {
      if (error.status === 409) { state.installed = false; state.overview = null; renderInstall(); }
      else message("error", `Sugar Break is unavailable: ${error.message}`);
    }
  }
  async function install() {
    const button = $(`[data-sugar-install]`); if (button) button.disabled = true;
    try { await request(apiPath("/install"), "POST", {}); message("notice", "Sugar Break installed. Start a break whenever you are ready; it follows your account to the desktop and mobile apps."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function startBreak(event) {
    event.preventDefault();
    const lengthDays = Number($(`[data-sugar-length]`).value || 0);
    try {
      await request(apiPath("/breaks"), "POST", { lengthDays });
      message("notice", `Your ${lengthDays}-day sugar break has started. Press +1 every time you skip sugar.`); await load();
    } catch (error) { message("error", `Could not start the sugar break: ${error.message}`); }
  }
  async function logEvent(kind) {
    try {
      await request(apiPath("/events"), "POST", { kind, day: localDay(new Date()) });
      message("notice", kind === "slip" ? "Logged that you had sugar. Tomorrow is a fresh day." : "+1 logged. You skipped sugar."); await load();
    } catch (error) { message("error", `Could not log that: ${error.message}`); }
  }
  async function undoEvent(id) {
    try { await request(apiPath(`/events/${encodeURIComponent(id)}`), "DELETE"); message("notice", "Undone."); await load(); }
    catch (error) { message("error", `Could not undo: ${error.message}`); }
  }
  async function endBreak() {
    const current = state.overview?.current; if (!current?.id) return;
    if (!confirm("End this sugar break early? Your days so far are kept in your history.")) return;
    try {
      await request(apiPath(`/breaks/${encodeURIComponent(current.id)}`), "PATCH", { end: true });
      message("notice", "Sugar break ended."); await load();
    } catch (error) { message("error", `Could not end the sugar break: ${error.message}`); }
  }
  async function exportMine() {
    try { const snapshot = await request(apiPath("/export")); downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-sugar-break.json"); }
    catch (error) { message("error", `Could not export: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm("Uninstall Sugar Break? Your breaks download first, then they are deleted from web, desktop, and mobile.")) return;
    try {
      const snapshot = await request(apiPath("/export"));
      if ((snapshot.export?.breaks || []).length) downloadJson(snapshot.export, snapshot.filename || "forkmesh-sugar-break.json");
      await request(apiPath("/uninstall"), "POST", {}); state.installed = false; state.overview = null;
      $(`[data-sugar-manage-dialog]`).close(); message("notice", "Sugar Break uninstalled."); await load();
    } catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Sugar Break.");
    await load();
    $(`[data-sugar-install]`)?.addEventListener("click", () => void install());
    $(`[data-sugar-refresh]`)?.addEventListener("click", () => void load());
    $(`[data-sugar-manage]`)?.addEventListener("click", () => $(`[data-sugar-manage-dialog]`).showModal());
    $(`[data-sugar-export]`)?.addEventListener("click", () => void exportMine());
    $(`[data-sugar-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-sugar-start-panel]`)?.addEventListener("submit", startBreak);
    $(`[data-sugar-skip]`)?.addEventListener("click", () => void logEvent("skip"));
    $(`[data-sugar-slip]`)?.addEventListener("click", () => void logEvent("slip"));
    $(`[data-sugar-events]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-sugar-undo]"); if (button) void undoEvent(button.dataset.sugarUndo);
    });
    $(`[data-sugar-end]`)?.addEventListener("click", () => void endBreak());
    $$(`[data-sugar-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    window.lucide?.createIcons?.();
  }
  return init();
});
