window.ForkMeshDashboard.registerPage("water", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const ML_PER_FL_OZ = 29.5735295625;
  const ML_PER_CUP = 8 * ML_PER_FL_OZ;
  const DRINKS = ["water", "sparkling", "tea", "coffee", "juice", "milk", "other"];
  const SOURCES = { health: "Apple Health", quick: "Quick add", manual: "Added by hand" };
  const state = { installed: false, overview: null, day: localDay(new Date()) };
  const currentSession = () => session();

  function localDay(date) {
    const d = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
    return d.toISOString().slice(0, 10);
  }
  function shortDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { month: "numeric", day: "numeric" }); }
  function longDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" }); }
  function units() { return state.overview?.profile?.units || "metric"; }
  function imperial() { return units() === "imperial"; }
  function cups() { return units() === "cups"; }
  function unitLabel() { return imperial() ? "fl oz" : cups() ? "cups" : "ml"; }
  function toMl(shown) { return imperial() ? shown * ML_PER_FL_OZ : cups() ? shown * ML_PER_CUP : shown; }
  function amount(ml) {
    const value = Number(ml || 0);
    if (cups()) {
      const count = Math.round((value / ML_PER_CUP) * 10) / 10;
      return `${count.toLocaleString(undefined, { maximumFractionDigits: 1 })} ${count === 1 ? "cup" : "cups"}`;
    }
    if (imperial()) return `${(value / ML_PER_FL_OZ).toLocaleString(undefined, { maximumFractionDigits: value >= 10 * ML_PER_FL_OZ ? 0 : 1 })} fl oz`;
    if (value >= 1000) return `${(value / 1000).toLocaleString(undefined, { maximumFractionDigits: 2 })} L`;
    return `${Math.round(value)} ml`;
  }
  function drinkLabel(drink) { const value = String(drink || "water"); return value.charAt(0).toUpperCase() + value.slice(1); }
  function message(kind, value = "") {
    const node = $(`[data-water-${kind}]`); if (!node) return;
    node.textContent = value; node.classList.toggle("hidden", !value);
  }
  function apiPath(suffix = "") { return `/api/water${suffix}`; }
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
    $(`[data-water-install-panel]`)?.classList.toggle("hidden", state.installed);
    $(`[data-water-app]`)?.classList.toggle("hidden", !state.installed);
    $$(`[data-water-requires-install]`).forEach((node) => node.classList.toggle("hidden", !state.installed));
  }
  function glassSvg(total, goal) {
    const ratio = goal > 0 ? Math.min(1, total / goal) : 0;
    const fillTop = 20 + 140 * (1 - ratio);
    const color = goal > 0 && total >= goal ? "var(--water-goal-met)" : "var(--water-bar)";
    return `<svg viewBox="0 0 120 180" role="img" aria-label="${escapeHtml(`${amount(total)} of ${amount(goal)}`)}">
      <defs><clipPath id="water-glass-clip"><path d="M18 20 L102 20 L92 160 L28 160 Z"/></clipPath></defs>
      <rect x="0" y="${fillTop.toFixed(1)}" width="120" height="${(160 - fillTop).toFixed(1)}" fill="${color}" opacity=".85" clip-path="url(#water-glass-clip)"/>
      <path class="water-glass-outline" d="M18 20 L102 20 L92 160 L28 160 Z" fill="none" stroke-width="4" stroke-linejoin="round"/>
      <text x="60" y="176" text-anchor="middle" font-size="13" font-weight="600" fill="var(--water-ink)">${escapeHtml(amount(total))}</text>
    </svg>`;
  }
  function chartSvg(days, goal) {
    const width = 720, height = 200, left = 60, bottom = 24, top = 8;
    const plotW = width - left - 8, plotH = height - bottom - top;
    const peak = Math.max(1000, goal * 1.15, ...days.map((day) => Number(day.totalMl || 0) * 1.05));
    const slot = plotW / Math.max(1, days.length), bar = Math.min(18, Math.max(3, slot - 4));
    const grid = [0, 1, 2, 3, 4].map((step) => {
      const y = top + plotH - plotH * step / 4;
      return `<line x1="${left}" x2="${width - 8}" y1="${y}" y2="${y}" stroke="var(--water-grid)"/><text x="${left - 6}" y="${y + 4}" text-anchor="end" font-size="10" fill="var(--water-ink-muted)">${escapeHtml(amount(peak * step / 4))}</text>`;
    }).join("");
    const bars = days.map((day, index) => {
      const total = Number(day.totalMl || 0), x = left + index * slot + (slot - bar) / 2;
      const h = total > 0 ? Math.max(2, plotH * total / peak) : 0;
      const color = goal > 0 && total >= goal ? "var(--water-goal-met)" : "var(--water-bar)";
      const label = index % 5 === 0 || index === days.length - 1
        ? `<text x="${x + bar / 2}" y="${height - 6}" text-anchor="middle" font-size="10" fill="var(--water-ink-muted)">${escapeHtml(shortDay(day.day))}</text>` : "";
      return `<g><title>${escapeHtml(`${longDay(day.day)}: ${amount(total)} · ${day.entries || 0} drinks`)}</title><rect class="water-bar" x="${x.toFixed(1)}" y="${(top + plotH - h).toFixed(1)}" width="${bar.toFixed(1)}" height="${h.toFixed(1)}" rx="3" fill="${color}"/>${label}</g>`;
    }).join("");
    const goalY = top + plotH - plotH * goal / peak;
    const goalLine = goal > 0 ? `<line x1="${left}" x2="${width - 8}" y1="${goalY}" y2="${goalY}" stroke="var(--water-goal)" stroke-width="2" stroke-dasharray="6 4"/>` : "";
    return `<svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Water per day for the last ${days.length} days">${grid}${bars}${goalLine}</svg>`;
  }
  function render() {
    const overview = state.overview || {};
    const profile = overview.profile || {}, today = overview.today || {}, summary = overview.summary || {};
    const goal = Number(profile.goalMl || 0), total = Number(today.totalMl || 0);
    $(`[data-water-glass]`).innerHTML = glassSvg(total, goal);
    $(`[data-water-detail]`).textContent = goal <= 0 ? "No daily goal set"
      : total >= goal ? `Goal of ${amount(goal)} reached` : `${amount(goal - total)} to go of ${amount(goal)}`;
    $(`[data-water-presets]`).innerHTML = `<span class="text-xs text-muted-foreground">Quick add</span>` + (profile.presetsMl || [])
      .map((ml) => `<button type="button" data-water-preset="${Number(ml)}" class="rounded-md border border-border bg-secondary px-3 py-2 text-xs font-semibold text-foreground">+ ${escapeHtml(amount(ml))}</button>`).join("");
    $(`[data-water-unit]`).textContent = unitLabel();
    const entries = [...(overview.entries || [])].reverse();
    $(`[data-water-entries]`).innerHTML = entries.length ? entries.map((entry) => `<div class="flex items-center justify-between gap-2 rounded-md px-2 py-1.5 hover:bg-secondary">
        <span class="text-sm text-foreground">${escapeHtml(new Date(Number(entry.at || 0)).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" }))} · ${escapeHtml(drinkLabel(entry.drink))}</span>
        <span class="flex items-center gap-3"><span class="text-xs text-muted-foreground">${escapeHtml(SOURCES[entry.source] || "")}</span><span class="text-sm font-semibold text-foreground">${escapeHtml(amount(entry.amountMl))}</span><button type="button" data-water-delete="${escapeHtml(entry.id)}" class="text-xs text-muted-foreground hover:text-destructive" aria-label="Delete this drink">✕</button></span>
      </div>`).join("") : `<p class="text-sm text-muted-foreground">Nothing logged today. Tap a quick add button after each glass.</p>`;
    $(`[data-water-chart]`).innerHTML = chartSvg(overview.days || [], goal);
    $(`[data-water-summary]`).textContent = `Average ${amount(summary.averageMl)} · goal met on ${summary.goalDays || 0} days · streak ${summary.streak || 0}`;
    $(`[data-water-goal]`).value = goal;
    $(`[data-water-units]`).value = profile.unitsPreference || "auto";
  }
  async function load() {
    try {
      state.day = localDay(new Date());
      const payload = await request(apiPath(`?day=${state.day}`)); state.installed = payload.installed !== false;
      state.overview = state.installed ? payload : null; message("error", ""); renderInstall(); if (state.installed) render();
    } catch (error) {
      if (error.status === 409) { state.installed = false; state.overview = null; renderInstall(); }
      else message("error", `Water Intake is unavailable: ${error.message}`);
    }
  }
  async function install() {
    const button = $(`[data-water-install]`); if (button) button.disabled = true;
    try { await request(apiPath("/install"), "POST", { tzOffsetMinutes: new Date().getTimezoneOffset() }); message("notice", "Water Intake installed. Your log follows your account to the desktop and mobile apps."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function addDrink(ml, drink, source) {
    if (!(ml > 0)) return message("error", "Enter an amount first.");
    try {
      await request(apiPath("/entries"), "POST", { day: localDay(new Date()), amountMl: Math.round(ml), drink, source, at: Date.now() });
      message("notice", `Logged ${amount(ml)} of ${drink}.`); await load();
    } catch (error) { message("error", `Could not log the drink: ${error.message}`); }
  }
  async function removeEntry(id) {
    try { await request(apiPath(`/entries/${id}`), "DELETE"); await load(); }
    catch (error) { message("error", `Could not delete the drink: ${error.message}`); }
  }
  async function exportMine() {
    try { const snapshot = await request(apiPath("/export")); downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-water.json"); }
    catch (error) { message("error", `Could not export: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm("Uninstall Water Intake? Your drinks log downloads first, then it is deleted from web, desktop, and mobile.")) return;
    try {
      const snapshot = await request(apiPath("/export"));
      if ((snapshot.export?.entries || []).length) downloadJson(snapshot.export, snapshot.filename || "forkmesh-water.json");
      await request(apiPath("/uninstall"), "POST", {}); state.installed = false; state.overview = null;
      $(`[data-water-manage-dialog]`).close(); message("notice", "Water Intake uninstalled."); await load();
    } catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  async function saveGoal(event) {
    event.preventDefault();
    try {
      await request(apiPath("/profile"), "PUT", { goalMl: Number($(`[data-water-goal]`).value || 0), unitsPreference: $(`[data-water-units]`).value });
      message("notice", "Goal saved."); await load();
    } catch (error) { message("error", `Could not save the goal: ${error.message}`); }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Water Intake.");
    $(`[data-water-drink]`).innerHTML = DRINKS.map((drink) => `<option value="${drink}">${escapeHtml(drinkLabel(drink))}</option>`).join("");
    await load();
    $(`[data-water-install]`)?.addEventListener("click", () => void install());
    $(`[data-water-manage]`)?.addEventListener("click", () => $(`[data-water-manage-dialog]`).showModal());
    $(`[data-water-export]`)?.addEventListener("click", () => void exportMine());
    $(`[data-water-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-water-presets]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-water-preset]"); if (button) void addDrink(Number(button.dataset.waterPreset), "water", "quick");
    });
    $(`[data-water-add-form]`)?.addEventListener("submit", (event) => {
      event.preventDefault();
      const shown = Number($(`[data-water-amount]`).value || 0);
      void addDrink(toMl(shown), $(`[data-water-drink]`).value, "manual");
      $(`[data-water-amount]`).value = "";
    });
    $(`[data-water-entries]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-water-delete]"); if (button) void removeEntry(button.dataset.waterDelete);
    });
    $(`[data-water-goal-form]`)?.addEventListener("submit", saveGoal);
    $$(`[data-water-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    window.lucide?.createIcons?.();
  }
  return init();
});
