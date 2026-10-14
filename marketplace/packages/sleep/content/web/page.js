window.ForkMeshDashboard.registerPage("sleep", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const state = { installed: false, overview: null, night: localDay(new Date()) };
  const currentSession = () => session();

  function localDay(date) {
    const d = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
    return d.toISOString().slice(0, 10);
  }
  function dayLabel(day) {
    if (day === state.night) return "Last night";
    return new Date(day + "T00:00:00").toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" });
  }
  function shortDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { month: "numeric", day: "numeric" }); }
  function duration(minutes) {
    const total = Math.max(0, Math.round(Number(minutes || 0)));
    const hours = Math.floor(total / 60), rest = total % 60;
    if (!hours) return `${rest} min`;
    return rest ? `${hours} h ${rest} min` : `${hours} h`;
  }
  function clock(ms) { return new Date(Number(ms || 0)).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" }); }
  function clockMinutes(minutes) {
    if (Number(minutes) < 0) return "—";
    const date = new Date(2000, 0, 1, Math.floor(minutes / 60), minutes % 60);
    return date.toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
  }
  function message(kind, value = "") {
    const node = $(`[data-sleep-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-sleep-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
  }
  function apiPath(suffix = "") { return `/api/sleep${suffix}`; }
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
    $(`[data-sleep-install-panel]`)?.classList.toggle("hidden", state.installed);
    $(`[data-sleep-app]`)?.classList.toggle("hidden", !state.installed);
    $$(`[data-sleep-requires-install]`).forEach((node) => node.classList.toggle("hidden", !state.installed));
  }
  function ringSvg(value, goal) {
    const radius = 54, circumference = 2 * Math.PI * radius;
    const ratio = goal > 0 ? Math.min(1, value / goal) : 0;
    const color = goal > 0 && value >= goal ? "var(--sleep-goal-met)" : "var(--sleep-bar)";
    return `<svg viewBox="0 0 140 140" role="img" aria-label="${escapeHtml(`${duration(value)} of ${duration(goal)} sleep`)}">
      <circle class="sleep-ring-track" cx="70" cy="70" r="${radius}" fill="none" stroke-width="12"/>
      <circle cx="70" cy="70" r="${radius}" fill="none" stroke="${color}" stroke-width="12" stroke-linecap="round" stroke-dasharray="${(circumference * ratio).toFixed(1)} ${circumference.toFixed(1)}" transform="rotate(-90 70 70)"/>
      <text x="70" y="66" text-anchor="middle" font-size="20" font-weight="600" fill="var(--sleep-ink)">${escapeHtml(duration(value))}</text>
      <text x="70" y="86" text-anchor="middle" font-size="11" fill="var(--sleep-ink-muted)">${goal > 0 ? escapeHtml(`of ${duration(goal)}`) : "no goal"}</text>
    </svg>`;
  }
  function chartSvg(nights, goal) {
    const width = 720, height = 220, left = 52, bottom = 24, top = 8;
    const plotW = width - left - 8, plotH = height - bottom - top;
    const peak = Math.max(600, goal * 1.15, ...nights.map((night) => Number(night.asleepMinutes || 0) * 1.05));
    const slot = plotW / Math.max(1, nights.length), bar = Math.min(18, Math.max(3, slot - 4));
    const grid = [0, 1, 2, 3, 4].map((step) => {
      const y = top + plotH - plotH * step / 4;
      return `<line x1="${left}" x2="${width - 8}" y1="${y}" y2="${y}" stroke="var(--sleep-grid)"/><text x="${left - 6}" y="${y + 4}" text-anchor="end" font-size="10" fill="var(--sleep-ink-muted)">${(peak * step / 4 / 60).toFixed(step ? 1 : 0)} h</text>`;
    }).join("");
    const bars = nights.map((night, index) => {
      const minutes = Number(night.asleepMinutes || 0), x = left + index * slot + (slot - bar) / 2;
      const h = minutes > 0 ? Math.max(2, plotH * minutes / peak) : 0;
      const color = goal > 0 && minutes >= goal ? "var(--sleep-goal-met)" : "var(--sleep-bar)";
      const label = index % 5 === 0 || index === nights.length - 1
        ? `<text x="${x + bar / 2}" y="${height - 6}" text-anchor="middle" font-size="10" fill="var(--sleep-ink-muted)">${escapeHtml(shortDay(night.night))}</text>` : "";
      const span = minutes > 0 ? ` · ${clock(night.bedAt)} – ${clock(night.wakeAt)}` : "";
      return `<g><title>${escapeHtml(`${dayLabel(night.night)}: ${minutes > 0 ? duration(minutes) : "no sleep recorded"}${span}`)}</title><rect class="sleep-bar" x="${x.toFixed(1)}" y="${(top + plotH - h).toFixed(1)}" width="${bar.toFixed(1)}" height="${h.toFixed(1)}" rx="3" fill="${color}"/>${label}</g>`;
    }).join("");
    const goalY = top + plotH - plotH * goal / peak;
    const goalLine = goal > 0 ? `<line x1="${left}" x2="${width - 8}" y1="${goalY}" y2="${goalY}" stroke="var(--sleep-goal)" stroke-width="2" stroke-dasharray="6 4"/>` : "";
    return `<svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Hours slept for the last ${nights.length} nights">${grid}${bars}${goalLine}</svg>`;
  }
  function sourceLine(last) {
    const labels = state.overview?.sourceLabels || {};
    if (!last.source) return "Nothing for last night yet. Open Sleep in the ForkMesh iPhone app in the morning and it fills in.";
    const reading = last.sources?.[last.source] || {};
    const parts = [labels[last.source] || last.source];
    if (last.source === "phone" && last.confidence) parts.push(`${last.confidence} confidence`);
    if (reading.updatedAt) parts.push(`synced ${clock(reading.updatedAt)}`);
    return parts.join(" · ");
  }
  function render() {
    const overview = state.overview || {};
    const profile = overview.profile || {}, last = overview.lastNight || {}, summary = overview.summary || {};
    const goal = Number(profile.goalMinutes || 0), asleep = Number(last.asleepMinutes || 0);
    $(`[data-sleep-ring]`).innerHTML = ringSvg(asleep, goal);
    const detail = [];
    if (asleep > 0) {
      detail.push(`${clock(last.bedAt)} – ${clock(last.wakeAt)}`);
      if (last.awakeMinutes) detail.push(`${duration(last.awakeMinutes)} awake`);
      if (last.interruptions) detail.push(`${last.interruptions} ${last.interruptions === 1 ? "interruption" : "interruptions"}`);
      if (last.deepMinutes) detail.push(`${duration(last.deepMinutes)} deep`);
      if (last.remMinutes) detail.push(`${duration(last.remMinutes)} REM`);
      if (goal > 0) detail.push(asleep >= goal ? "Goal reached" : `${duration(goal - asleep)} short`);
    }
    $(`[data-sleep-detail]`).textContent = detail.join(" · ");
    $(`[data-sleep-source]`).textContent = sourceLine(last);
    const stats = [
      ["Last 7 nights", summary.weekNights ? `${duration(summary.weekAverageMinutes)} a night` : "—", goal > 0 ? `${duration(summary.debtMinutes)} sleep debt` : ""],
      ["30-night average", summary.loggedNights ? duration(summary.averageMinutes) : "—", `${summary.goalNights || 0} of ${summary.loggedNights || 0} nights met the goal`],
      ["Average bedtime", clockMinutes(summary.averageBedtimeMinutes), summary.bedtimeSpreadMinutes ? `± ${duration(summary.bedtimeSpreadMinutes)}` : ""],
      ["Average wake time", clockMinutes(summary.averageWakeMinutes), `Goal streak ${summary.streak || 0} ${summary.streak === 1 ? "night" : "nights"}`],
    ];
    $(`[data-sleep-stats]`).innerHTML = stats.map(([label, value, note]) => `<div><div class="text-xs text-muted-foreground">${escapeHtml(label)}</div><div class="text-base font-semibold text-foreground">${escapeHtml(value)}</div><div class="text-xs text-muted-foreground">${escapeHtml(note)}</div></div>`).join("");
    $(`[data-sleep-chart]`).innerHTML = chartSvg(overview.nights || [], goal);
    $(`[data-sleep-goal]`).value = Math.round(goal / 15) / 4;
    $(`[data-sleep-bedtime]`).value = profile.bedtime || "23:00";
    $(`[data-sleep-wake-time]`).value = profile.wakeTime || "07:00";
    const nightInput = $(`[data-sleep-manual-night]`);
    nightInput.max = state.night; if (!nightInput.value) nightInput.value = state.night;
  }
  async function load() {
    try {
      state.night = localDay(new Date());
      const payload = await request(apiPath(`?night=${state.night}`)); state.installed = payload.installed !== false;
      state.overview = state.installed ? payload : null; message("error", ""); renderInstall(); if (state.installed) render();
    } catch (error) {
      if (error.status === 409) { state.installed = false; state.overview = null; renderInstall(); }
      else message("error", `Sleep is unavailable: ${error.message}`);
    }
  }
  async function install() {
    const button = $(`[data-sleep-install]`); if (button) button.disabled = true;
    try { await request(apiPath("/install"), "POST", { tzOffsetMinutes: new Date().getTimezoneOffset() }); message("notice", "Sleep installed. Turn on sleep tracking in the ForkMesh iPhone app and leave your phone by your bed tonight."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function exportMine() {
    try { const snapshot = await request(apiPath("/export")); downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-sleep.json"); }
    catch (error) { message("error", `Could not export: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm("Uninstall Sleep? Your nights download first, then they are deleted from web, desktop, and mobile.")) return;
    try {
      const snapshot = await request(apiPath("/export"));
      if ((snapshot.export?.nights || []).length) downloadJson(snapshot.export, snapshot.filename || "forkmesh-sleep.json");
      await request(apiPath("/uninstall"), "POST", {}); state.installed = false; state.overview = null;
      $(`[data-sleep-manage-dialog]`).close(); message("notice", "Sleep uninstalled."); await load();
    } catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  function nightSpan(night, bed, wake) {
    const wakeAt = new Date(`${night}T${wake}:00`);
    const bedAt = new Date(`${night}T${bed}:00`);
    if (bed >= wake) bedAt.setDate(bedAt.getDate() - 1);
    return { bedAt: bedAt.getTime(), wakeAt: wakeAt.getTime() };
  }
  async function saveManual(event) {
    event.preventDefault();
    const night = $(`[data-sleep-manual-night]`).value;
    const span = nightSpan(night, $(`[data-sleep-manual-bed]`).value, $(`[data-sleep-manual-wake]`).value);
    const awakeMinutes = Number($(`[data-sleep-manual-awake]`).value || 0);
    try {
      await request(apiPath(`/nights/${night}`), "PUT", { ...span, awakeMinutes });
      message("notice", `Saved the night you added for ${dayLabel(night)}.`); await load();
    } catch (error) { message("error", `Could not save the night: ${error.message}`); }
  }
  async function clearManual() {
    const night = $(`[data-sleep-manual-night]`).value; if (!night) return;
    try { await request(apiPath(`/nights/${night}?source=manual`), "DELETE"); message("notice", `Removed the night you added for ${dayLabel(night)}.`); await load(); }
    catch (error) { message("error", `Could not remove the night: ${error.message}`); }
  }
  async function saveGoal(event) {
    event.preventDefault();
    try {
      await request(apiPath("/profile"), "PUT", {
        goalMinutes: Math.round(Number($(`[data-sleep-goal]`).value || 0) * 60),
        bedtime: $(`[data-sleep-bedtime]`).value, wakeTime: $(`[data-sleep-wake-time]`).value,
        tzOffsetMinutes: new Date().getTimezoneOffset(),
      });
      message("notice", "Goal and schedule saved."); await load();
    } catch (error) { message("error", `Could not save the goal: ${error.message}`); }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Sleep.");
    await load();
    $(`[data-sleep-install]`)?.addEventListener("click", () => void install());
    $(`[data-sleep-refresh]`)?.addEventListener("click", () => void load());
    $(`[data-sleep-manage]`)?.addEventListener("click", () => $(`[data-sleep-manage-dialog]`).showModal());
    $(`[data-sleep-export]`)?.addEventListener("click", () => void exportMine());
    $(`[data-sleep-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-sleep-manual-form]`)?.addEventListener("submit", saveManual);
    $(`[data-sleep-manual-clear]`)?.addEventListener("click", () => void clearManual());
    $(`[data-sleep-goal-form]`)?.addEventListener("submit", saveGoal);
    $$(`[data-sleep-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    window.lucide?.createIcons?.();
  }
  return init();
});
