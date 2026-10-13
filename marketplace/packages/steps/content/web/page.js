window.ForkMeshDashboard.registerPage("steps", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const METERS_PER_MILE = 1609.344;
  const state = { installed: false, overview: null, day: localDay(new Date()) };
  const currentSession = () => session();

  function localDay(date) {
    const d = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
    return d.toISOString().slice(0, 10);
  }
  function dayLabel(day) {
    if (day === state.day) return "Today";
    return new Date(day + "T00:00:00").toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" });
  }
  function shortDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { month: "numeric", day: "numeric" }); }
  function n(value) { return Number(value || 0).toLocaleString(undefined, { maximumFractionDigits: 0 }); }
  function imperial() { return state.overview?.profile?.units === "imperial"; }
  function distance(meters) {
    const value = imperial() ? Number(meters || 0) / METERS_PER_MILE : Number(meters || 0) / 1000;
    return `${value.toLocaleString(undefined, { maximumFractionDigits: 2 })} ${imperial() ? "mi" : "km"}`;
  }
  function message(kind, value = "") {
    const node = $(`[data-steps-${kind}]`); if (!node) return;
    node.textContent = value; node.classList.toggle("hidden", !value);
  }
  function apiPath(suffix = "") { return `/api/steps${suffix}`; }
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
    $(`[data-steps-install-panel]`)?.classList.toggle("hidden", state.installed);
    $(`[data-steps-app]`)?.classList.toggle("hidden", !state.installed);
    $$(`[data-steps-requires-install]`).forEach((node) => node.classList.toggle("hidden", !state.installed));
  }
  function ringSvg(value, goal) {
    const radius = 54, circumference = 2 * Math.PI * radius;
    const ratio = goal > 0 ? Math.min(1, value / goal) : 0;
    const color = goal > 0 && value >= goal ? "var(--steps-goal-met)" : "var(--steps-bar)";
    return `<svg viewBox="0 0 140 140" role="img" aria-label="${escapeHtml(`${n(value)} of ${n(goal)} steps`)}">
      <circle class="steps-ring-track" cx="70" cy="70" r="${radius}" fill="none" stroke-width="12"/>
      <circle cx="70" cy="70" r="${radius}" fill="none" stroke="${color}" stroke-width="12" stroke-linecap="round" stroke-dasharray="${(circumference * ratio).toFixed(1)} ${circumference.toFixed(1)}" transform="rotate(-90 70 70)"/>
      <text x="70" y="66" text-anchor="middle" font-size="24" font-weight="600" fill="var(--steps-ink)">${n(value)}</text>
      <text x="70" y="86" text-anchor="middle" font-size="11" fill="var(--steps-ink-muted)">of ${n(goal)} steps</text>
    </svg>`;
  }
  function chartSvg(days, goal) {
    const width = 720, height = 220, left = 52, bottom = 24, top = 8;
    const plotW = width - left - 8, plotH = height - bottom - top;
    const peak = Math.max(2000, goal * 1.15, ...days.map((day) => Number(day.steps || 0) * 1.05));
    const slot = plotW / Math.max(1, days.length), bar = Math.min(18, Math.max(3, slot - 4));
    const grid = [0, 1, 2, 3, 4].map((step) => {
      const y = top + plotH - plotH * step / 4;
      return `<line x1="${left}" x2="${width - 8}" y1="${y}" y2="${y}" stroke="var(--steps-grid)"/><text x="${left - 6}" y="${y + 4}" text-anchor="end" font-size="10" fill="var(--steps-ink-muted)">${n(peak * step / 4)}</text>`;
    }).join("");
    const bars = days.map((day, index) => {
      const steps = Number(day.steps || 0), x = left + index * slot + (slot - bar) / 2;
      const h = steps > 0 ? Math.max(2, plotH * steps / peak) : 0;
      const color = goal > 0 && steps >= goal ? "var(--steps-goal-met)" : "var(--steps-bar)";
      const label = index % 5 === 0 || index === days.length - 1
        ? `<text x="${x + bar / 2}" y="${height - 6}" text-anchor="middle" font-size="10" fill="var(--steps-ink-muted)">${escapeHtml(shortDay(day.day))}</text>` : "";
      return `<g><title>${escapeHtml(`${dayLabel(day.day)}: ${n(steps)} steps · ${distance(day.distanceM)}`)}</title><rect class="steps-bar" x="${x.toFixed(1)}" y="${(top + plotH - h).toFixed(1)}" width="${bar.toFixed(1)}" height="${h.toFixed(1)}" rx="3" fill="${color}"/>${label}</g>`;
    }).join("");
    const goalY = top + plotH - plotH * goal / peak;
    const goalLine = goal > 0 ? `<line x1="${left}" x2="${width - 8}" y1="${goalY}" y2="${goalY}" stroke="var(--steps-goal)" stroke-width="2" stroke-dasharray="6 4"/>` : "";
    return `<svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Steps per day for the last ${days.length} days">${grid}${bars}${goalLine}</svg>`;
  }
  function render() {
    const overview = state.overview || {};
    const profile = overview.profile || {}, today = overview.today || {}, summary = overview.summary || {};
    const goal = Number(profile.goal || 0), steps = Number(today.steps || 0);
    $(`[data-steps-ring]`).innerHTML = ringSvg(steps, goal);
    const detail = [distance(today.distanceM)];
    if (today.floors) detail.push(`${n(today.floors)} floors`);
    if (today.activeKcal) detail.push(`${n(today.activeKcal)} active kcal`);
    if (goal > 0) detail.unshift(steps >= goal ? "Goal reached" : `${n(goal - steps)} to go`);
    $(`[data-steps-detail]`).textContent = detail.join(" · ");
    const health = today.sources?.health;
    $(`[data-steps-source]`).textContent = health
      ? `From Apple Health, synced ${new Date(Number(health.updatedAt || 0)).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" })}${today.sources?.manual ? ", plus steps you added by hand" : ""}`
      : "Nothing from your phone yet today. Open Steps in the ForkMesh iPhone app and turn on Apple Health.";
    const stats = [
      ["Last 7 days", `${n(summary.weekSteps)} steps`, distance(summary.weekDistanceM)],
      ["30-day average", `${n(summary.averageSteps)} steps`, ""],
      ["Goal streak", `${n(summary.streak)} ${summary.streak === 1 ? "day" : "days"}`, `${n(summary.goalDays)} of 30 days met`],
      ["Best day", summary.bestDay ? `${n(summary.bestSteps)} steps` : "—", summary.bestDay ? dayLabel(summary.bestDay) : ""],
    ];
    $(`[data-steps-stats]`).innerHTML = stats.map(([label, value, note]) => `<div><div class="text-xs text-muted-foreground">${escapeHtml(label)}</div><div class="text-base font-semibold text-foreground">${escapeHtml(value)}</div><div class="text-xs text-muted-foreground">${escapeHtml(note)}</div></div>`).join("");
    $(`[data-steps-chart]`).innerHTML = chartSvg(overview.days || [], goal);
    $(`[data-steps-goal]`).value = goal;
    $(`[data-steps-units]`).value = profile.unitsPreference || "auto";
    $(`[data-steps-distance-unit]`).textContent = imperial() ? "mi" : "km";
    const dayInput = $(`[data-steps-manual-day]`);
    dayInput.max = state.day; if (!dayInput.value) dayInput.value = state.day;
  }
  async function load() {
    try {
      state.day = localDay(new Date());
      const payload = await request(apiPath(`?day=${state.day}`)); state.installed = payload.installed !== false;
      state.overview = state.installed ? payload : null; message("error", ""); renderInstall(); if (state.installed) render();
    } catch (error) {
      if (error.status === 409) { state.installed = false; state.overview = null; renderInstall(); }
      else message("error", `Pedometer is unavailable: ${error.message}`);
    }
  }
  async function install() {
    const button = $(`[data-steps-install]`); if (button) button.disabled = true;
    try { await request(apiPath("/install"), "POST", { tzOffsetMinutes: new Date().getTimezoneOffset() }); message("notice", "Pedometer installed. Turn on step counting in the ForkMesh iPhone app to start filling it in."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function exportMine() {
    try { const snapshot = await request(apiPath("/export")); downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-steps.json"); }
    catch (error) { message("error", `Could not export: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm("Uninstall Pedometer? Your step history downloads first, then it is deleted from web, desktop, and mobile.")) return;
    try {
      const snapshot = await request(apiPath("/export"));
      if ((snapshot.export?.days || []).length) downloadJson(snapshot.export, snapshot.filename || "forkmesh-steps.json");
      await request(apiPath("/uninstall"), "POST", {}); state.installed = false; state.overview = null;
      $(`[data-steps-manage-dialog]`).close(); message("notice", "Pedometer uninstalled."); await load();
    } catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  async function saveManual(event) {
    event.preventDefault();
    const day = $(`[data-steps-manual-day]`).value, steps = Number($(`[data-steps-manual-steps]`).value || 0);
    const shown = Number($(`[data-steps-manual-distance]`).value || 0);
    const distanceM = Math.round((imperial() ? shown * METERS_PER_MILE : shown * 1000) * 10) / 10;
    try {
      await request(apiPath(`/days/${day}`), "PUT", { steps, distanceM });
      $(`[data-steps-manual-steps]`).value = ""; $(`[data-steps-manual-distance]`).value = "";
      message("notice", `Saved the steps you added for ${dayLabel(day)}.`); await load();
    } catch (error) { message("error", `Could not save steps: ${error.message}`); }
  }
  async function clearManual() {
    const day = $(`[data-steps-manual-day]`).value; if (!day) return;
    try { await request(apiPath(`/days/${day}?source=manual`), "DELETE"); message("notice", `Removed the steps you added for ${dayLabel(day)}.`); await load(); }
    catch (error) { message("error", `Could not remove steps: ${error.message}`); }
  }
  async function saveGoal(event) {
    event.preventDefault();
    try {
      await request(apiPath("/profile"), "PUT", { goal: Number($(`[data-steps-goal]`).value || 0), unitsPreference: $(`[data-steps-units]`).value });
      message("notice", "Goal saved."); await load();
    } catch (error) { message("error", `Could not save the goal: ${error.message}`); }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Pedometer.");
    await load();
    $(`[data-steps-install]`)?.addEventListener("click", () => void install());
    $(`[data-steps-refresh]`)?.addEventListener("click", () => void load());
    $(`[data-steps-manage]`)?.addEventListener("click", () => $(`[data-steps-manage-dialog]`).showModal());
    $(`[data-steps-export]`)?.addEventListener("click", () => void exportMine());
    $(`[data-steps-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-steps-manual-form]`)?.addEventListener("submit", saveManual);
    $(`[data-steps-manual-clear]`)?.addEventListener("click", () => void clearManual());
    $(`[data-steps-goal-form]`)?.addEventListener("submit", saveGoal);
    $$(`[data-steps-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    window.lucide?.createIcons?.();
  }
  return init();
});
