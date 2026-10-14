// Calorie Counter plugin: private diaries, AI photo estimates, barcode lookups,
// goals, reminders, charts, and friend challenges. Everything the page shows
// comes from the package Worker API; the host supplies session, fetch, and
// escaping helpers only.
window.ForkMeshDashboard.registerPage("calories", ({
  select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel,
}) => {
  const KG_PER_LB = 0.45359237;
  const CM_PER_IN = 2.54;
  const MEALS = ["breakfast", "lunch", "dinner", "snack"];
  const KIND_LABELS = {
    weight_loss_percent: "Lose the most percent of body weight",
    weight_loss_kg: "Lose the most weight",
    calorie_streak: "Most days within the calorie goal",
    log_streak: "Most days logged",
  };
  // day is the local day the totals, streaks, and goal ring stand for; logDay
  // is the day new entries land on. The diary list itself is never day-scoped.
  const state = {
    installed: false, overview: null, day: localDay(new Date()), logDay: localDay(new Date()),
    tab: "diary", tables: {}, photo: { image: "", items: [] }, product: null,
    streams: { photo: null, scan: null }, scanTimer: 0, board: null,
    quick: { lists: { mine: [], frequent: [], common: [] }, busy: false }, unregisterComposer: null,
    searchResults: [], searchActive: -1, searchTimer: 0, searchQuery: "", searchCache: new Map(),
  };
  const currentSession = () => session();

  // --- helpers --------------------------------------------------------------
  function localDay(date) {
    const d = new Date(date.getTime() - date.getTimezoneOffset() * 60000);
    return d.toISOString().slice(0, 10);
  }
  function shiftDay(day, delta) {
    const date = new Date(day + "T00:00:00Z"); date.setUTCDate(date.getUTCDate() + delta);
    return date.toISOString().slice(0, 10);
  }
  function dayLabel(day) {
    const date = new Date(day + "T00:00:00");
    return date.toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" });
  }
  function shortDay(day) { return new Date(day + "T00:00:00").toLocaleDateString(undefined, { month: "numeric", day: "numeric" }); }
  function n(value) { return Number(value || 0).toLocaleString(undefined, { maximumFractionDigits: 0 }); }
  function g(value) { return Number(value || 0).toLocaleString(undefined, { maximumFractionDigits: 1 }); }
  function imperial() { return state.overview?.profile?.units === "imperial"; }
  // The units select holds the stored preference; "auto" renders in whatever
  // the Worker resolved for this location, which is not the same as the unit
  // on screen when the member is switching away from a pinned one.
  function autoImperial() {
    const auto = state.overview?.profile?.autoUnits;
    return auto ? auto === "imperial" : imperial();
  }
  function formImperial() {
    const choice = $(`[data-calories-goal="units"]`)?.value || "auto";
    return choice === "auto" ? autoImperial() : choice === "imperial";
  }
  function weightUnit() { return imperial() ? "lb" : "kg"; }
  function toDisplayWeight(kg) { return imperial() ? kg / KG_PER_LB : kg; }
  function toKg(value) { return imperial() ? Number(value) * KG_PER_LB : Number(value); }
  function fmtWeight(kg) { return kg ? `${g(toDisplayWeight(kg))} ${weightUnit()}` : "—"; }
  function message(kind, value = "") {
    const node = $(`[data-calories-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-calories-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
    if (value) node.scrollIntoView?.({ block: "nearest" });
  }
  function apiPath(suffix = "") { return `/api/calories${suffix}`; }
  function icons() { window.lucide?.createIcons?.(); }
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
  function stopStream(key) {
    const stream = state.streams[key];
    if (stream) { stream.getTracks().forEach((track) => track.stop()); state.streams[key] = null; }
    if (key === "scan" && state.scanTimer) { clearInterval(state.scanTimer); state.scanTimer = 0; }
  }
  async function openCamera(key, video) {
    if (!navigator.mediaDevices?.getUserMedia) throw new Error("camera not available in this browser");
    stopStream(key);
    const stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: { ideal: "environment" } }, audio: false });
    state.streams[key] = stream; video.srcObject = stream; await video.play().catch(() => {});
    return stream;
  }

  // --- install (personal: one diary per account, shared by web, desktop, and mobile) ---
  function renderInstall() {
    const installed = state.installed;
    $(`[data-calories-install-panel]`)?.classList.toggle("hidden", installed);
    $(`[data-calories-app]`)?.classList.toggle("hidden", !installed);
    $$(`[data-calories-requires-install]`).forEach((node) => node.classList.toggle("hidden", !installed));
  }
  async function install() {
    const button = $(`[data-calories-install]`); if (button) button.disabled = true;
    try { await request(apiPath("/install"), "POST", { tzOffsetMinutes: new Date().getTimezoneOffset() }); state.installed = true; message("notice", "Calorie Counter installed. Your diary follows your account to the desktop and mobile apps."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function uninstall() {
    if (!confirm("Uninstall Calorie Counter? Your data export downloads first, then your diary, weights, goals, reminders, and the challenges you created are deleted.")) return;
    try {
      const snapshot = await request(apiPath("/export"));
      downloadJson(snapshot.export || {}, snapshot.filename || "forkmesh-calories.json");
      await request(apiPath("/uninstall"), "POST", {}); state.installed = false; state.overview = null;
      $(`[data-calories-manage-dialog]`).close(); message("notice", "Calorie Counter uninstalled. Your export was downloaded."); await load();
    } catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }

  // --- tabs ------------------------------------------------------------------
  function showTab(tab) {
    state.tab = tab;
    $$(`[data-calories-tab]`).forEach((button) => button.setAttribute("aria-selected", String(button.dataset.caloriesTab === tab)));
    $$(`[data-calories-panel]`).forEach((panel) => panel.classList.toggle("hidden", panel.dataset.caloriesPanel !== tab));
    if (tab === "trends") renderTrends();
  }

  // --- diary -----------------------------------------------------------------
  function ringSvg(value, goal) {
    const radius = 54, circumference = 2 * Math.PI * radius;
    const ratio = goal > 0 ? Math.min(1, value / goal) : 0;
    const over = goal > 0 && value > goal;
    const color = over ? "var(--cal-series-2)" : "var(--cal-series-1)";
    return `<svg viewBox="0 0 140 140" role="img" aria-label="${escapeHtml(`${n(value)} of ${n(goal)} kcal`)}">
      <circle class="cal-ring-track" cx="70" cy="70" r="${radius}" fill="none" stroke-width="12"/>
      <circle cx="70" cy="70" r="${radius}" fill="none" stroke="${color}" stroke-width="12" stroke-linecap="round" stroke-dasharray="${(circumference * ratio).toFixed(1)} ${circumference.toFixed(1)}" transform="rotate(-90 70 70)"/>
      <text x="70" y="68" text-anchor="middle" font-size="28" font-weight="600" fill="var(--cal-ink)">${n(value)}</text>
      <text x="70" y="90" text-anchor="middle" font-size="13" fill="var(--cal-ink-muted)">of ${n(goal)} kcal</text>
    </svg>`;
  }
  function macroBar(label, value, goal, color) {
    const ratio = goal > 0 ? Math.min(1, value / goal) : 0;
    return `<div><div class="flex items-baseline justify-between text-xs"><span class="text-muted-foreground"><span class="cal-legend-swatch" style="background:${color}"></span>${label}</span><span class="text-foreground">${g(value)} g <span class="text-muted-foreground">/ ${n(goal)} g</span></span></div>
      <div class="mt-1 h-2 rounded-full" style="background:var(--cal-grid)"><div class="h-2 rounded-full" style="width:${(ratio * 100).toFixed(1)}%;background:${color}"></div></div></div>`;
  }
  function weekSvg(days, goal) {
    const W = 168, H = 52, gap = 4, bar = (W - gap * 6) / 7, today = state.overview?.today;
    const max = Math.max(goal || 0, ...days.map((day) => day.calories || 0), 1) * 1.1;
    const y = (value) => H - (value / max) * H;
    let out = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none" role="img" aria-label="Calories per day for the last 7 days">`;
    days.forEach((day, i) => {
      const value = day.calories || 0, x = i * (bar + gap), over = goal > 0 && value > goal;
      const tip = `${dayLabel(day.day)}: ${n(value)} kcal${goal ? ` of ${n(goal)}` : ""}`;
      out += `<g data-tip="${escapeHtml(tip)}"><rect class="cal-hit" x="${x}" y="0" width="${bar}" height="${H}"/>`
        + `<rect class="cal-bar" x="${x}" y="${value ? y(value).toFixed(1) : H - 1}" width="${bar}" height="${value ? (H - y(value)).toFixed(1) : 1}" rx="2" fill="${value ? (over ? "var(--cal-series-2)" : "var(--cal-series-1)") : "var(--cal-grid)"}"${day.day === today ? "" : ` opacity=".7"`}/></g>`;
    });
    if (goal) out += `<line x1="0" x2="${W}" y1="${y(goal).toFixed(1)}" y2="${y(goal).toFixed(1)}" stroke="var(--cal-series-4)" stroke-width="1" stroke-dasharray="3 3" vector-effect="non-scaling-stroke"/>`;
    return `${out}</svg>`;
  }
  function renderDiary() {
    const o = state.overview; if (!o) return;
    const goal = o.profile.dailyCalories || 0, totals = o.todayTotals || {};
    $(`[data-calories-ring]`).innerHTML = ringSvg(totals.calories || 0, goal);
    const remaining = goal - (totals.calories || 0);
    $(`[data-calories-ring-caption]`).textContent = goal ? (remaining >= 0 ? `${n(remaining)} kcal left` : `${n(-remaining)} kcal over goal`) : "Set a daily calorie goal to track progress.";
    const todays = (o.entries || []).filter((item) => item.day === o.today);
    $(`[data-calories-meals]`).innerHTML = MEALS.map((meal) => {
      const kcal = todays.filter((item) => item.meal === meal).reduce((total, item) => total + (Number(item.calories) || 0), 0);
      return `<p class="cal-kv"><span class="capitalize">${meal}</span><span>${n(kcal)} kcal</span></p>`;
    }).join("");
    const week = (o.days || []).slice(-7);
    const host = $(`[data-calories-week]`);
    host.innerHTML = weekSvg(week, goal); tooltip(host);
    $(`[data-calories-week-average]`).textContent = o.weekAverage ? `avg ${n(o.weekAverage)} kcal` : "";
    $(`[data-calories-macros]`).innerHTML = [
      macroBar("Protein", totals.proteinG, o.profile.proteinG, "var(--cal-series-1)"),
      macroBar("Carbs", totals.carbsG, o.profile.carbsG, "var(--cal-series-2)"),
      macroBar("Fat", totals.fatG, o.profile.fatG, "var(--cal-series-3)"),
    ].join("");
    $(`[data-calories-stat-logged]`).textContent = n(o.streaks?.logged);
    $(`[data-calories-stat-goal]`).textContent = n(o.streaks?.onGoal);
    $(`[data-calories-stat-weight]`).textContent = fmtWeight(o.latestWeight?.weightKg);
    const target = o.profile.targetWeightKg;
    $(`[data-calories-stat-weight-caption]`).textContent = o.latestWeight?.weightKg
      ? `${dayLabel(o.latestWeight.day)}${target ? ` · ${g(toDisplayWeight(Math.abs(o.latestWeight.weightKg - target)))} ${weightUnit()} ${o.latestWeight.weightKg > target ? "to go" : "below target"}` : ""}`
      : "No weigh-in yet";
    $(`[data-calories-log-day]`).value = state.logDay;
    const rows = [...(o.entries || [])].sort((a, b) => (a.day < b.day ? 1 : a.day > b.day ? -1 : 0)
      || MEALS.indexOf(a.meal) - MEALS.indexOf(b.meal) || a.loggedAt - b.loggedAt);
    const groups = [];
    rows.forEach((item) => {
      const last = groups[groups.length - 1];
      if (last && last.day === item.day) last.items.push(item);
      else groups.push({ day: item.day, items: [item] });
    });
    $(`[data-calories-diary-summary]`).textContent = rows.length
      ? `${n(rows.length)} entr${rows.length === 1 ? "y" : "ies"} over ${n(groups.length)} day${groups.length === 1 ? "" : "s"} · ${n(totals.calories)} kcal today`
      : "";
    $(`[data-calories-entries-more]`).textContent = o.entriesTruncated
      ? `Showing the ${n(rows.length)} most recent entries. Older ones stay in your export.` : "";
    $(`[data-calories-entries-more]`).classList.toggle("hidden", !o.entriesTruncated);
    const dayHeader = (group) => {
      const sum = (key) => group.items.reduce((total, item) => total + (Number(item[key]) || 0), 0);
      return `<tr class="bg-secondary/60"><th colspan="2" class="text-left text-xs font-semibold text-foreground">${escapeHtml(dayLabel(group.day))}${group.day === o.today ? " · today" : ""}</th>
        <td class="text-right text-xs font-semibold text-foreground">${n(sum("calories"))}</td>
        <td class="text-right text-xs text-muted-foreground">${g(sum("proteinG"))}</td>
        <td class="text-right text-xs text-muted-foreground">${g(sum("carbsG"))}</td>
        <td class="text-right text-xs text-muted-foreground">${g(sum("fatG"))}</td>
        <td></td></tr>`;
    };
    $(`[data-calories-entries]`).innerHTML = groups.map((group) => dayHeader(group) + group.items.map((item) => `<tr class="hover:bg-secondary">
      <td><button data-calories-entry-edit="${escapeHtml(item.id)}" class="block text-left font-semibold text-foreground">${escapeHtml(item.name)}</button><span class="text-xs text-muted-foreground">${escapeHtml([item.portion, item.brand, item.source !== "manual" ? `via ${item.source}` : ""].filter(Boolean).join(" · "))}</span></td>
      <td class="capitalize text-muted-foreground">${escapeHtml(item.meal)}</td>
      <td class="text-right text-foreground">${n(item.calories)}</td>
      <td class="text-right text-muted-foreground">${g(item.proteinG)}</td>
      <td class="text-right text-muted-foreground">${g(item.carbsG)}</td>
      <td class="text-right text-muted-foreground">${g(item.fatG)}</td>
      <td class="text-right"><button data-calories-entry-edit="${escapeHtml(item.id)}" class="fm-btn fm-btn-sm">Edit</button></td></tr>`).join("")).join("");
    $(`[data-calories-entries-empty]`).classList.toggle("hidden", rows.length > 0);
  }

  // --- charts ------------------------------------------------------------------
  function tooltip(host) {
    let tip = host.querySelector(".cal-tip");
    if (!tip) { tip = document.createElement("div"); tip.className = "cal-tip hidden"; host.appendChild(tip); }
    host.onmousemove = (event) => {
      const target = event.target.closest("[data-tip]"); if (!target) { tip.classList.add("hidden"); return; }
      tip.textContent = target.dataset.tip; tip.classList.remove("hidden");
      const rect = host.getBoundingClientRect();
      tip.style.left = `${Math.min(event.clientX - rect.left + 12, rect.width - tip.offsetWidth - 4)}px`;
      tip.style.top = `${event.clientY - rect.top - 28}px`;
    };
    host.onmouseleave = () => tip.classList.add("hidden");
  }
  function axisY(max, height, top, width, left, steps = 4) {
    let out = "";
    for (let i = 0; i <= steps; i += 1) {
      const value = max * i / steps, y = top + height - (height * i / steps);
      out += `<line x1="${left}" x2="${left + width}" y1="${y.toFixed(1)}" y2="${y.toFixed(1)}" stroke="var(--cal-grid)" stroke-width="1"/><text x="${left - 6}" y="${(y + 4).toFixed(1)}" text-anchor="end" font-size="10" fill="var(--cal-ink-muted)">${n(value)}</text>`;
    }
    return out;
  }
  function calorieChart() {
    const o = state.overview, days = o.days || [], goal = o.profile.dailyCalories || 0;
    const W = 1100, H = 300, left = 48, top = 14, right = 12, bottom = 30, width = W - left - right, height = H - top - bottom;
    const max = Math.max(goal * 1.15, ...days.map((d) => d.calories), 500);
    const slot = width / days.length, bar = Math.min(22, Math.max(4, slot - 6));
    let out = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Calories per day for the last 30 days">${axisY(max, height, top, width, left)}`;
    days.forEach((d, i) => {
      const h = height * d.calories / max, x = left + i * slot + 1, y = top + height - h;
      const color = goal && d.calories > goal ? "var(--cal-series-2)" : "var(--cal-series-1)";
      out += `<g data-tip="${escapeHtml(`${dayLabel(d.day)}: ${n(d.calories)} kcal${goal ? ` (goal ${n(goal)})` : ""}`)}"><rect class="cal-hit" x="${x.toFixed(1)}" y="${top}" width="${bar.toFixed(1)}" height="${height}"/><rect class="cal-bar" x="${x.toFixed(1)}" y="${y.toFixed(1)}" width="${bar.toFixed(1)}" height="${h.toFixed(1)}" rx="3" fill="${d.entries ? color : "var(--cal-grid)"}"/></g>`;
      if (i % 5 === 0 || i === days.length - 1) out += `<text x="${(x + bar / 2).toFixed(1)}" y="${H - 10}" text-anchor="middle" font-size="10" fill="var(--cal-ink-muted)">${shortDay(d.day)}</text>`;
    });
    if (goal) { const y = top + height - height * goal / max; out += `<line x1="${left}" x2="${left + width}" y1="${y.toFixed(1)}" y2="${y.toFixed(1)}" stroke="var(--cal-series-4)" stroke-width="2" stroke-dasharray="6 4"/><text x="${left + width}" y="${(y - 4).toFixed(1)}" text-anchor="end" font-size="10" fill="var(--cal-ink-muted)">Goal ${n(goal)}</text>`; }
    out += "</svg>";
    const table = `<table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="px-2 py-1">Day</th><th class="px-2 py-1 text-right">kcal</th><th class="px-2 py-1 text-right">Protein</th><th class="px-2 py-1 text-right">Carbs</th><th class="px-2 py-1 text-right">Fat</th><th class="px-2 py-1 text-right">Entries</th></tr></thead><tbody>${[...days].reverse().map((d) => `<tr class="border-t border-border"><td class="px-2 py-1">${dayLabel(d.day)}</td><td class="px-2 py-1 text-right">${n(d.calories)}</td><td class="px-2 py-1 text-right">${g(d.proteinG)}</td><td class="px-2 py-1 text-right">${g(d.carbsG)}</td><td class="px-2 py-1 text-right">${g(d.fatG)}</td><td class="px-2 py-1 text-right">${d.entries}</td></tr>`).join("")}</tbody></table>`;
    return { svg: out, table };
  }
  function macroChart() {
    const days = (state.overview.days || []).slice(-7);
    const W = 560, H = 260, left = 44, top = 14, right = 8, bottom = 30, width = W - left - right, height = H - top - bottom;
    const max = Math.max(50, ...days.map((d) => d.proteinG + d.carbsG + d.fatG));
    const slot = width / days.length, bar = Math.max(6, slot - 8);
    let out = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Protein, carbs, and fat grams per day for the last 7 days">${axisY(max, height, top, width, left, 2)}`;
    days.forEach((d, i) => {
      const x = left + i * slot + 4; let y = top + height;
      const parts = [["Protein", d.proteinG, "var(--cal-series-1)"], ["Carbs", d.carbsG, "var(--cal-series-2)"], ["Fat", d.fatG, "var(--cal-series-3)"]];
      parts.forEach(([label, value, color]) => {
        const h = height * value / max; y -= h;
        if (h > 0) out += `<rect class="cal-bar" data-tip="${escapeHtml(`${dayLabel(d.day)} · ${label}: ${g(value)} g`)}" x="${x.toFixed(1)}" y="${(y + 1).toFixed(1)}" width="${bar.toFixed(1)}" height="${Math.max(0, h - 2).toFixed(1)}" rx="2" fill="${color}"/>`;
      });
      out += `<text x="${(x + bar / 2).toFixed(1)}" y="${H - 10}" text-anchor="middle" font-size="10" fill="var(--cal-ink-muted)">${shortDay(d.day)}</text>`;
    });
    out += "</svg>";
    const table = `<table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="px-2 py-1">Day</th><th class="px-2 py-1 text-right">Protein g</th><th class="px-2 py-1 text-right">Carbs g</th><th class="px-2 py-1 text-right">Fat g</th></tr></thead><tbody>${[...days].reverse().map((d) => `<tr class="border-t border-border"><td class="px-2 py-1">${dayLabel(d.day)}</td><td class="px-2 py-1 text-right">${g(d.proteinG)}</td><td class="px-2 py-1 text-right">${g(d.carbsG)}</td><td class="px-2 py-1 text-right">${g(d.fatG)}</td></tr>`).join("")}</tbody></table>`;
    return { svg: out, table };
  }
  function weightChart() {
    const o = state.overview, points = o.weights || [], target = o.profile.targetWeightKg || 0;
    const W = 560, H = 260, left = 48, top = 14, right = 12, bottom = 30, width = W - left - right, height = H - top - bottom;
    if (!points.length) return { svg: `<p class="py-10 text-center text-sm text-muted-foreground">No weigh-ins yet. Use “Weigh in” on the Diary tab.</p>`, table: "" };
    const values = points.map((p) => toDisplayWeight(p.weightKg)).concat(target ? [toDisplayWeight(target)] : []);
    const lo = Math.floor(Math.min(...values) - 1), hi = Math.ceil(Math.max(...values) + 1);
    const first = new Date(points[0].day + "T00:00:00Z").getTime(), last = new Date(points[points.length - 1].day + "T00:00:00Z").getTime();
    const span = Math.max(1, last - first);
    const px = (day) => left + width * (new Date(day + "T00:00:00Z").getTime() - first) / span;
    const py = (value) => top + height - height * (value - lo) / (hi - lo);
    let out = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Weight trend">`;
    for (let i = 0; i <= 3; i += 1) { const v = lo + (hi - lo) * i / 3, y = py(v); out += `<line x1="${left}" x2="${left + width}" y1="${y.toFixed(1)}" y2="${y.toFixed(1)}" stroke="var(--cal-grid)"/><text x="${left - 6}" y="${(y + 4).toFixed(1)}" text-anchor="end" font-size="10" fill="var(--cal-ink-muted)">${g(v)}</text>`; }
    if (target) { const y = py(toDisplayWeight(target)); out += `<line x1="${left}" x2="${left + width}" y1="${y.toFixed(1)}" y2="${y.toFixed(1)}" stroke="var(--cal-series-4)" stroke-width="2" stroke-dasharray="6 4"/><text x="${left + width}" y="${(y - 4).toFixed(1)}" text-anchor="end" font-size="10" fill="var(--cal-ink-muted)">Target ${g(toDisplayWeight(target))}</text>`; }
    const path = points.map((p, i) => `${i ? "L" : "M"}${px(p.day).toFixed(1)},${py(toDisplayWeight(p.weightKg)).toFixed(1)}`).join(" ");
    out += `<path d="${path}" fill="none" stroke="var(--cal-series-3)" stroke-width="2" stroke-linejoin="round"/>`;
    points.forEach((p) => { out += `<circle data-tip="${escapeHtml(`${dayLabel(p.day)}: ${fmtWeight(p.weightKg)}`)}" cx="${px(p.day).toFixed(1)}" cy="${py(toDisplayWeight(p.weightKg)).toFixed(1)}" r="4" fill="var(--cal-series-3)" stroke="var(--cal-surface)" stroke-width="2"/>`; });
    out += `<text x="${left}" y="${H - 10}" font-size="10" fill="var(--cal-ink-muted)">${shortDay(points[0].day)}</text><text x="${left + width}" y="${H - 10}" text-anchor="end" font-size="10" fill="var(--cal-ink-muted)">${shortDay(points[points.length - 1].day)}</text></svg>`;
    const table = `<table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="px-2 py-1">Day</th><th class="px-2 py-1 text-right">Weight</th></tr></thead><tbody>${[...points].reverse().map((p) => `<tr class="border-t border-border"><td class="px-2 py-1"><button data-calories-weight-edit="${escapeHtml(p.day)}" class="underline">${dayLabel(p.day)}</button></td><td class="px-2 py-1 text-right">${fmtWeight(p.weightKg)}</td></tr>`).join("")}</tbody></table>`;
    return { svg: out, table };
  }
  function renderTrends() {
    if (!state.overview) return;
    for (const [key, build] of [["calories", calorieChart], ["macros", macroChart], ["weight", weightChart]]) {
      const { svg, table } = build();
      const host = $(`[data-calories-chart="${key}"]`), tableHost = $(`[data-calories-table="${key}"]`);
      host.innerHTML = svg; tableHost.innerHTML = table; tooltip(host);
      const showTable = Boolean(state.tables[key]);
      host.classList.toggle("hidden", showTable); tableHost.classList.toggle("hidden", !showTable);
      const button = $(`[data-calories-table-toggle="${key}"]`); if (button) button.textContent = showTable ? "Chart" : "Table";
    }
  }

  // --- goals & reminders ---------------------------------------------------------
  function reminderRow(item, index) {
    return `<div class="flex flex-wrap items-center gap-2 rounded-md border border-border p-2" data-calories-reminder="${index}">
      <input data-calories-reminder-time type="time" value="${escapeHtml(item.time || "12:30")}" required class="rounded-md border border-border bg-background px-2 py-1 text-sm text-foreground" />
      <input data-calories-reminder-label maxlength="80" value="${escapeHtml(item.label || "")}" placeholder="Log lunch" class="min-w-[120px] flex-1 rounded-md border border-border bg-background px-2 py-1 text-sm text-foreground" />
      <label class="flex items-center gap-1 text-xs text-muted-foreground"><input data-calories-reminder-unlogged type="checkbox" ${item.onlyIfUnlogged ? "checked" : ""} /> Only if nothing logged</label>
      <label class="flex items-center gap-1 text-xs text-muted-foreground"><input data-calories-reminder-enabled type="checkbox" ${item.enabled === false ? "" : "checked"} /> On</label>
      <button data-calories-reminder-remove type="button" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm" aria-label="Remove reminder"><svg aria-hidden="true" viewBox="0 0 16 16" fill="currentColor"><path d="M3.72 3.72a.75.75 0 0 1 1.06 0L8 6.94l3.22-3.22a.749.749 0 0 1 1.275.326.749.749 0 0 1-.215.734L9.06 8l3.22 3.22a.749.749 0 0 1-.326 1.275.749.749 0 0 1-.734-.215L8 9.06l-3.22 3.22a.751.751 0 0 1-1.042-.018.751.751 0 0 1-.018-1.042L6.94 8 3.72 4.78a.75.75 0 0 1 0-1.06Z"></path></svg></button></div>`;
  }
  function renderGoals() {
    const p = state.overview?.profile; if (!p) return;
    $(`[data-calories-goal="dailyCalories"]`).value = p.dailyCalories;
    $(`[data-calories-goal="units"]`).value = p.unitsPreference || p.units;
    // An <option> renders text only, so the resolved unit is written into the
    // label rather than marked up inside it.
    $(`[data-calories-goal-auto]`).textContent = p.autoUnits
      ? `Automatic — match my location (${p.autoUnits === "imperial" ? "lb, in" : "kg, cm"})`
      : "Automatic — match my location";
    $(`[data-calories-goal="proteinG"]`).value = p.proteinG;
    $(`[data-calories-goal="carbsG"]`).value = p.carbsG;
    $(`[data-calories-goal="fatG"]`).value = p.fatG;
    $(`[data-calories-goal="targetWeight"]`).value = p.targetWeightKg ? g(toDisplayWeight(p.targetWeightKg)) : "";
    $(`[data-calories-goal="startWeight"]`).value = p.startWeightKg ? g(toDisplayWeight(p.startWeightKg)) : "";
    $(`[data-calories-goal="height"]`).value = p.heightCm ? g(imperial() ? p.heightCm / CM_PER_IN : p.heightCm) : "";
    $$(`[data-calories-weight-unit]`).forEach((node) => { node.textContent = weightUnit(); });
    $$(`[data-calories-height-unit]`).forEach((node) => { node.textContent = imperial() ? "in" : "cm"; });
    const latest = state.overview.latestWeight?.weightKg;
    $(`[data-calories-bmi]`).textContent = latest && p.heightCm ? `BMI ${(latest / ((p.heightCm / 100) ** 2)).toFixed(1)} from your latest weigh-in.` : "";
    $(`[data-calories-tz]`).textContent = Intl.DateTimeFormat().resolvedOptions().timeZone || "local time";
    $(`[data-calories-reminders]`).innerHTML = (p.reminders || []).map(reminderRow).join("") || `<p class="text-xs text-muted-foreground">No reminders yet.</p>`;
    $(`[data-calories-usda-status]`).textContent = state.overview.sources?.usda ? "A USDA key is saved." : "No USDA key saved.";
    $$(`[data-calories-scan-usda], [data-calories-search-usda]`).forEach((node) => { node.textContent = state.overview.sources?.usda ? " and USDA FoodData Central" : ""; });
  }
  function collectReminders() {
    return $$(`[data-calories-reminder]`).map((row) => ({
      time: row.querySelector("[data-calories-reminder-time]").value,
      label: row.querySelector("[data-calories-reminder-label]").value,
      onlyIfUnlogged: row.querySelector("[data-calories-reminder-unlogged]").checked,
      enabled: row.querySelector("[data-calories-reminder-enabled]").checked,
    })).filter((item) => item.time);
  }
  async function saveGoals(event) {
    event.preventDefault();
    const unitsPreference = $(`[data-calories-goal="units"]`).value;
    const shownImperial = formImperial();
    const factorW = shownImperial ? KG_PER_LB : 1, factorH = shownImperial ? CM_PER_IN : 1;
    const body = {
      dailyCalories: Number($(`[data-calories-goal="dailyCalories"]`).value || 0),
      proteinG: Number($(`[data-calories-goal="proteinG"]`).value || 0),
      carbsG: Number($(`[data-calories-goal="carbsG"]`).value || 0),
      fatG: Number($(`[data-calories-goal="fatG"]`).value || 0),
      targetWeightKg: Number($(`[data-calories-goal="targetWeight"]`).value || 0) * factorW,
      startWeightKg: Number($(`[data-calories-goal="startWeight"]`).value || 0) * factorW,
      heightCm: Number($(`[data-calories-goal="height"]`).value || 0) * factorH,
      unitsPreference, tzOffsetMinutes: new Date().getTimezoneOffset(), reminders: collectReminders(),
    };
    try { await request(apiPath("/profile"), "PUT", body); $(`[data-calories-goals-saved]`).textContent = "Saved."; await load(); }
    catch (error) { message("error", `Could not save goals: ${error.message}`); }
  }
  async function saveSettings(event) {
    event.preventDefault();
    try { const payload = await request(apiPath("/settings"), "PUT", { usdaApiKey: $(`[data-calories-usda-key]`).value.trim() }); $(`[data-calories-usda-key]`).value = ""; message("notice", payload.settings?.usda ? "Your USDA key was saved." : "Your USDA key was removed."); await load(); }
    catch (error) { message("error", `Could not save settings: ${error.message}`); }
  }
  async function exportMine() {
    try { const payload = await request(apiPath("/export")); downloadJson(payload.export || {}, payload.filename || "forkmesh-calories-diary.json"); }
    catch (error) { message("error", `Export failed: ${error.message}`); }
  }

  // --- entry editor ---------------------------------------------------------------
  function openEntry(item = null, preset = null) {
    const values = item || preset || {};
    $(`[data-calories-entry-title]`).textContent = item ? "Edit food" : "Add food";
    $(`[data-calories-entry-id]`).value = item?.id || "";
    $(`[data-calories-entry-source]`).value = values.source || "manual";
    $(`[data-calories-entry-barcode]`).value = values.barcode || "";
    $(`[data-calories-entry-brand]`).value = values.brand || "";
    $(`[data-calories-entry-name]`).value = values.name || "";
    $(`[data-calories-entry-day]`).value = values.day || state.logDay;
    $(`[data-calories-entry-portion]`).value = values.portion || "";
    $(`[data-calories-entry-meal]`).value = values.meal || guessMeal();
    $(`[data-calories-entry-calories]`).value = values.calories ?? "";
    $(`[data-calories-entry-protein]`).value = values.proteinG ?? "";
    $(`[data-calories-entry-carbs]`).value = values.carbsG ?? "";
    $(`[data-calories-entry-fat]`).value = values.fatG ?? "";
    $(`[data-calories-entry-notes]`).value = values.notes || "";
    $(`[data-calories-entry-delete]`).classList.toggle("hidden", !item);
    $(`[data-calories-entry-dialog]`).showModal();
  }
  function guessMeal() { const hour = new Date().getHours(); return hour < 11 ? "breakfast" : hour < 15 ? "lunch" : hour < 21 ? "dinner" : "snack"; }
  async function saveEntry(event) {
    event.preventDefault();
    const id = $(`[data-calories-entry-id]`).value;
    const body = {
      day: $(`[data-calories-entry-day]`).value || state.logDay, name: $(`[data-calories-entry-name]`).value, portion: $(`[data-calories-entry-portion]`).value,
      meal: $(`[data-calories-entry-meal]`).value, calories: Number($(`[data-calories-entry-calories]`).value || 0),
      proteinG: Number($(`[data-calories-entry-protein]`).value || 0), carbsG: Number($(`[data-calories-entry-carbs]`).value || 0),
      fatG: Number($(`[data-calories-entry-fat]`).value || 0), notes: $(`[data-calories-entry-notes]`).value,
      source: $(`[data-calories-entry-source]`).value, barcode: $(`[data-calories-entry-barcode]`).value, brand: $(`[data-calories-entry-brand]`).value,
    };
    try { await request(apiPath(`/entries${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body); $(`[data-calories-entry-dialog]`).close(); message("notice", "Saved to your diary."); await load(); }
    catch (error) { message("error", `Could not save: ${error.message}`); }
  }
  async function removeEntry() {
    const id = $(`[data-calories-entry-id]`).value; if (!id || !confirm("Delete this entry?")) return;
    try { await request(apiPath(`/entries/${id}`), "DELETE", {}); $(`[data-calories-entry-dialog]`).close(); await load(); }
    catch (error) { message("error", `Could not delete: ${error.message}`); }
  }
  async function addEntries(items, meal, source, day = state.logDay) {
    let saved = 0;
    for (const item of items) {
      await request(apiPath("/entries"), "POST", { ...item, day, meal, source });
      saved += 1;
    }
    message("notice", `${saved} item${saved === 1 ? "" : "s"} added to ${dayLabel(day)}.`);
    await load();
    return saved;
  }

  // --- photo analysis ----------------------------------------------------------------
  async function fileToDataUrl(blob, maxSide = 1024) {
    const bitmap = await createImageBitmap(blob);
    const scale = Math.min(1, maxSide / Math.max(bitmap.width, bitmap.height));
    const canvas = document.createElement("canvas");
    canvas.width = Math.round(bitmap.width * scale); canvas.height = Math.round(bitmap.height * scale);
    canvas.getContext("2d").drawImage(bitmap, 0, 0, canvas.width, canvas.height);
    let quality = 0.85, url = canvas.toDataURL("image/jpeg", quality);
    while (url.length > 1_400_000 && quality > 0.4) { quality -= 0.15; url = canvas.toDataURL("image/jpeg", quality); }
    return url;
  }
  function setPhoto(url) {
    state.photo.image = url;
    const preview = $(`[data-calories-photo-preview]`); preview.src = url;
    $(`[data-calories-photo-preview-wrap]`).classList.toggle("hidden", !url);
    $(`[data-calories-photo-camera]`).classList.add("hidden"); $(`[data-calories-photo-snap]`).classList.add("hidden"); stopStream("photo");
  }
  function openPhoto() {
    state.photo = { image: "", items: [] }; setPhoto(""); $(`[data-calories-photo-preview-wrap]`).classList.add("hidden");
    $(`[data-calories-photo-results]`).innerHTML = ""; $(`[data-calories-photo-summary]`).textContent = ""; $(`[data-calories-photo-save]`).classList.add("hidden");
    $(`[data-calories-photo-status]`).textContent = state.overview?.sources?.ai === false ? "AI estimates are unavailable on this host; describe the meal and add it by hand." : "";
    $(`[data-calories-photo-meal]`).value = guessMeal(); $(`[data-calories-photo-dialog]`).showModal();
  }
  async function startPhotoCamera() {
    try { $(`[data-calories-photo-preview-wrap]`).classList.add("hidden"); $(`[data-calories-photo-camera]`).classList.remove("hidden"); await openCamera("photo", $(`[data-calories-photo-video]`)); $(`[data-calories-photo-snap]`).classList.remove("hidden"); }
    catch (error) { $(`[data-calories-photo-camera]`).classList.add("hidden"); $(`[data-calories-photo-status]`).textContent = `Camera unavailable: ${error.message}. Choose a file instead.`; }
  }
  async function snapPhoto() {
    const video = $(`[data-calories-photo-video]`); const canvas = document.createElement("canvas");
    canvas.width = video.videoWidth || 1024; canvas.height = video.videoHeight || 768;
    canvas.getContext("2d").drawImage(video, 0, 0);
    const blob = await new Promise((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.9));
    setPhoto(await fileToDataUrl(blob));
  }
  async function pickPhoto(event) {
    const file = event.target.files?.[0]; if (!file) return;
    try { setPhoto(await fileToDataUrl(file)); } catch (error) { $(`[data-calories-photo-status]`).textContent = `Could not read that image: ${error.message}`; }
    event.target.value = "";
  }
  function renderSuggestions(items) {
    state.photo.items = items;
    $(`[data-calories-photo-results]`).innerHTML = items.map((item, i) => `<label class="flex items-start gap-2 rounded-md border border-border p-2"><input data-calories-suggestion="${i}" type="checkbox" checked class="mt-1" /><span class="min-w-0 flex-1"><span class="block font-semibold text-foreground">${escapeHtml(item.name)}</span><span class="block text-xs text-muted-foreground">${escapeHtml(item.portion || "portion not given")} · confidence ${Math.round((item.confidence || 0) * 100)}%</span><span class="mt-1 flex flex-wrap items-center gap-1 text-xs"><input data-calories-suggestion-kcal="${i}" type="number" min="0" value="${Math.round(item.calories)}" class="w-20 rounded border border-border bg-background px-1 py-0.5 text-foreground" /> kcal · P ${g(item.proteinG)} g · C ${g(item.carbsG)} g · F ${g(item.fatG)} g</span></span></label>`).join("");
    $(`[data-calories-photo-save]`).classList.toggle("hidden", !items.length);
  }
  async function analyzePhoto() {
    const status = $(`[data-calories-photo-status]`);
    const description = $(`[data-calories-photo-description]`).value.trim();
    if (!state.photo.image && !description) { status.textContent = "Take or choose a photo, or describe the meal."; return; }
    status.textContent = "Asking the model…";
    try {
      const payload = await request(apiPath("/analyze"), "POST", state.photo.image ? { image: state.photo.image, hint: $(`[data-calories-photo-hint]`).value } : { description });
      renderSuggestions(payload.items || []);
      status.textContent = `${payload.items.length} item${payload.items.length === 1 ? "" : "s"} · about ${n(payload.totalCalories)} kcal${payload.model ? ` · ${payload.model.split("/").pop()}` : ""}`;
      $(`[data-calories-photo-summary]`).textContent = payload.summary || "";
    } catch (error) {
      status.textContent = error.status === 429 ? "You have used this hour's AI estimates. Try again later or add the meal by hand." : error.status === 503 ? "AI estimates are not available on this server. Add the meal by hand." : `Could not estimate: ${error.message}`;
    }
  }
  async function savePhotoItems() {
    const chosen = state.photo.items.map((item, i) => ({ item, i })).filter(({ i }) => $(`[data-calories-suggestion="${i}"]`)?.checked)
      .map(({ item, i }) => ({ name: item.name, portion: item.portion, calories: Number($(`[data-calories-suggestion-kcal="${i}"]`)?.value || item.calories), proteinG: item.proteinG, carbsG: item.carbsG, fatG: item.fatG, confidence: item.confidence }));
    if (!chosen.length) return;
    try { await addEntries(chosen, $(`[data-calories-photo-meal]`).value, state.photo.image ? "photo" : "text"); $(`[data-calories-photo-dialog]`).close(); }
    catch (error) { $(`[data-calories-photo-status]`).textContent = `Could not save: ${error.message}`; }
  }

  function dataUrlToBlob(dataUrl) {
    const [head, body = ""] = String(dataUrl).split(",", 2);
    const type = /^data:([^;,]+)/.exec(head)?.[1] || "image/jpeg";
    const bytes = Uint8Array.from(atob(body), (char) => char.charCodeAt(0));
    return new Blob([bytes], { type });
  }

  function foodKey(name) { return String(name || "").trim().toLowerCase(); }
  function frequentFoods(mine) {
    const known = new Set(mine.map((food) => foodKey(food.name)));
    const counts = new Map();
    for (const item of state.overview?.entries || []) {
      const key = foodKey(item.name); if (!key || known.has(key)) continue;
      const seen = counts.get(key);
      if (seen) seen.count += 1;
      else counts.set(key, { count: 1, food: { name: item.name, brand: item.brand || "", portion: item.portion || "", calories: item.calories, proteinG: item.proteinG, carbsG: item.carbsG, fatG: item.fatG } });
    }
    return [...counts.values()].filter((entry) => entry.count > 1).sort((a, b) => b.count - a.count).slice(0, 8).map((entry) => ({ ...entry.food, times: entry.count }));
  }
  function quickMeal() { return $(`[data-calories-quick-meal]`)?.value || guessMeal(); }
  function quickStatus(value) { const node = $(`[data-calories-quick-status]`); if (node) node.textContent = value; }
  function quickRow(food, key, side) {
    const meta = [food.brand, food.portion, `${n(food.calories)} kcal`, food.times ? `logged ${food.times}×` : ""].filter(Boolean).join(" · ");
    return `<div class="cal-quick-row" data-calories-quick-row="${key}"><button data-calories-quick-add="${key}" type="button" title="${escapeHtml(`Log ${food.name} (${meta})`)}"><span class="cal-quick-name text-sm text-foreground">${escapeHtml(food.name)}</span><span class="cal-quick-meta text-xs text-muted-foreground">${escapeHtml(meta)}</span></button>${side}</div>`;
  }
  function renderQuick() {
    const o = state.overview; if (!o) return;
    const mine = o.quickFoods || [];
    const lists = { mine, frequent: frequentFoods(mine), common: o.commonFoods || [] };
    state.quick.lists = lists;
    const filter = foodKey($(`[data-calories-quick-filter]`)?.value);
    const shown = (list) => list.map((food, i) => ({ food, i })).filter(({ food }) => !filter || foodKey(food.name).includes(filter) || foodKey(food.brand).includes(filter));
    const section = (title, rows, empty) => `<section><h3 class="mb-1 text-xs font-semibold text-muted-foreground">${title}</h3>${rows.length ? `<div class="cal-quick-list grid gap-0.5">${rows.join("")}</div>` : `<p class="text-xs text-muted-foreground">${empty}</p>`}</section>`;
    const mineRows = shown(lists.mine).map(({ food, i }) => quickRow(food, `mine:${i}`, `<button data-calories-quick-remove="${i}" type="button" class="cal-quick-side" aria-label="${escapeHtml(`Remove ${food.name} from My foods`)}" title="Remove from My foods">✕</button>`));
    const frequentRows = shown(lists.frequent).map(({ food, i }) => quickRow(food, `frequent:${i}`, `<button data-calories-quick-save="${i}" type="button" class="cal-quick-side" aria-label="${escapeHtml(`Save ${food.name} to My foods`)}" title="Save to My foods">☆</button>`));
    const commonRows = shown(lists.common).map(({ food, i }) => quickRow(food, `common:${i}`, `<button data-calories-quick-save-common="${i}" type="button" class="cal-quick-side" aria-label="${escapeHtml(`Save ${food.name} to My foods`)}" title="Save to My foods">☆</button>`));
    $(`[data-calories-quick-lists]`).innerHTML = [
      section("My foods", mineRows, filter ? "No match." : "Nothing saved yet. Upload a receipt or star a food."),
      lists.frequent.length ? section("You log often", frequentRows, "No match.") : "",
      section("Common foods", commonRows, "No match."),
    ].join("");
    const day = $(`[data-calories-quick-day]`); if (day) day.textContent = state.logDay === localDay(new Date()) ? "today" : dayLabel(state.logDay);
  }
  function quickFood(key) {
    const [kind, index] = String(key).split(":");
    return state.quick.lists[kind]?.[Number(index)] || null;
  }
  async function quickAdd(key) {
    const food = quickFood(key); if (!food) return;
    const row = $(`[data-calories-quick-row="${key}"]`); if (row) row.dataset.busy = "1";
    const meal = quickMeal();
    try {
      const { name, brand, portion, calories, proteinG, carbsG, fatG } = food;
      await addEntries([{ name, brand, portion, calories, proteinG, carbsG, fatG }], meal, "quick");
      quickStatus(`Logged ${food.name} (${n(food.calories)} kcal) to ${meal}.`);
    } catch (error) { quickStatus(`Could not log ${food.name}: ${error.message}`); if (row) row.dataset.busy = "0"; }
  }
  async function saveMyFoods(foods, note) {
    const payload = await request(apiPath("/quick-foods"), "PUT", { foods });
    state.overview.quickFoods = payload.quickFoods || []; renderQuick(); quickStatus(note);
  }
  async function starFood(food) {
    if (!food) return;
    const mine = state.overview?.quickFoods || [];
    if (mine.some((item) => foodKey(item.name) === foodKey(food.name))) { quickStatus(`${food.name} is already in My foods.`); return; }
    const { name, brand, portion, calories, proteinG, carbsG, fatG } = food;
    try { await saveMyFoods([{ name, brand, portion, calories, proteinG, carbsG, fatG }, ...mine], `Saved ${food.name} to My foods.`); }
    catch (error) { quickStatus(`Could not save: ${error.message}`); }
  }
  async function removeMyFood(index) {
    const mine = state.overview?.quickFoods || [], food = mine[index]; if (!food) return;
    try { await saveMyFoods(mine.filter((_, i) => i !== index), `Removed ${food.name} from My foods.`); }
    catch (error) { quickStatus(`Could not remove: ${error.message}`); }
  }
  async function readReceipt(blob) {
    const image = await fileToDataUrl(blob, 1600);
    try {
      const payload = await request(apiPath("/receipt"), "POST", { image });
      if (state.overview) { state.overview.quickFoods = payload.quickFoods || []; renderQuick(); }
      const added = payload.added || [], found = payload.found || [];
      const from = payload.store ? ` from ${payload.store}` : "";
      return added.length
        ? `Added ${added.length} food${added.length === 1 ? "" : "s"}${from} to My foods: ${added.join(", ")}.`
        : `Read ${found.length} food${found.length === 1 ? "" : "s"}${from}; all were already in My foods.`;
    } catch (error) {
      if (error.status === 429) throw new Error("You have used this hour's AI reads. Try the receipt again later.");
      if (error.status === 422) throw new Error("No foods were found on that receipt. Try a sharper, flatter photo.");
      if (error.status === 503) throw new Error("Receipt reading needs AI, which is unavailable on this host.");
      throw new Error(`Could not read the receipt: ${error.message}`);
    }
  }
  async function pickReceipt(event) {
    const file = event.target.files?.[0]; event.target.value = ""; if (!file) return;
    quickStatus("Reading the receipt…");
    try { quickStatus(await readReceipt(file)); } catch (error) { quickStatus(error.message); }
  }

  const NUMBER_WORDS = { a: 1, an: 1, one: 1, two: 2, three: 3, four: 4, five: 5, six: 6, seven: 7, eight: 8, nine: 9, ten: 10, half: 0.5, "a half": 0.5, couple: 2, "a couple of": 2, "a few": 3 };
  const MEAL_WORDS = { breakfast: "breakfast", brunch: "breakfast", lunch: "lunch", dinner: "dinner", supper: "dinner", snack: "snack", snacks: "snack", dessert: "snack" };
  function promptMeal(text) {
    const match = /\b(breakfast|brunch|lunch|dinner|supper|snacks?|dessert)\b/i.exec(text);
    return match ? MEAL_WORDS[match[1].toLowerCase()] : "";
  }
  function promptDay(text) {
    if (/\byesterday\b/i.test(text)) return shiftDay(localDay(new Date()), -1);
    if (/\btoday\b|\bthis (morning|afternoon|evening)\b|\btonight\b/i.test(text)) return localDay(new Date());
    return state.logDay;
  }
  function promptWeight(text) {
    const match = /^(?:i\s+)?(?:weigh(?:ed|t)?(?:\s+in)?|my weight(?:\s+is)?|weight(?:\s+is)?)\s*(?:at|is|was|:|=)?\s*(\d{2,3}(?:[.,]\d+)?)\s*(kg|kgs|kilos?|kilograms?|lb|lbs|pounds?)?\s*(?:today|yesterday|this morning)?\.?$/i.exec(text.trim());
    if (!match) return null;
    const value = Number(match[1].replace(",", "."));
    const unit = (match[2] || "").toLowerCase();
    const kg = unit.startsWith("k") ? value : unit ? value * KG_PER_LB : toKg(value);
    return kg > 0 ? { kg, day: promptDay(text) } : null;
  }
  function singular(word) { return word.replace(/(ch|sh|ss|x)es$/, "$1").replace(/([^s])s$/, "$1"); }
  function normaliseFood(name) { return String(name || "").toLowerCase().replace(/[^a-z0-9 ]+/g, " ").split(/\s+/).filter(Boolean).map(singular).join(" "); }
  function promptParts(text) {
    const cleaned = text.toLowerCase()
      .replace(/\b(for|at|with)\s+(breakfast|brunch|lunch|dinner|supper|snacks?|dessert)\b/g, " ")
      .replace(/\b(breakfast|brunch|lunch|dinner|supper|snacks?|dessert)\s*:/g, " ")
      .replace(/\b(yesterday|today|tonight|this (morning|afternoon|evening))\b/g, " ")
      .replace(/^\s*(i\s+)?(just\s+)?(had|ate|drank|log|add)\b/g, " ");
    return cleaned.split(/,|;|\n|\+|&|\band\b/).map((part) => part.trim()).filter(Boolean);
  }
  function matchLocally(text) {
    const foods = [...(state.overview?.quickFoods || []), ...state.quick.lists.frequent, ...(state.overview?.commonFoods || [])];
    const index = new Map();
    foods.forEach((food) => { const key = normaliseFood(food.name); if (key && !index.has(key)) index.set(key, food); });
    const matched = [];
    for (const part of promptParts(text)) {
      let rest = part, amount = 1;
      const numeric = /^(\d+\/\d+|\d+(?:[.,]\d+)?)\s*(?:x\s+)?/.exec(rest);
      const worded = /^(a couple of|a few|a half|an|a|one|two|three|four|five|six|seven|eight|nine|ten|half|couple)\s+/.exec(rest);
      if (numeric) { amount = numeric[1].includes("/") ? numeric[1].split("/").reduce((a, b) => Number(a) / Number(b)) : Number(numeric[1].replace(",", ".")); rest = rest.slice(numeric[0].length); }
      else if (worded) { amount = NUMBER_WORDS[worded[1]]; rest = rest.slice(worded[0].length); }
      rest = rest.replace(/^(?:(?:of|some|an?)\s+)+/, "");
      const food = index.get(normaliseFood(rest));
      if (!food || !(amount > 0) || amount > 20) return null;
      const factor = (value) => +(Number(value || 0) * amount).toFixed(1);
      matched.push({ name: food.name, brand: food.brand || "", portion: amount === 1 ? food.portion : `${g(amount)} × ${food.portion || "serving"}`, calories: Math.round(Number(food.calories || 0) * amount), proteinG: factor(food.proteinG), carbsG: factor(food.carbsG), fatG: factor(food.fatG) });
    }
    return matched.length ? matched : null;
  }
  function loggedMessage(items, meal, day) {
    const total = items.reduce((sum, item) => sum + (Number(item.calories) || 0), 0);
    const goal = state.overview?.profile?.dailyCalories || 0, today = state.overview?.todayTotals?.calories || 0;
    const onToday = day === localDay(new Date());
    return `Logged to ${meal}, ${onToday ? "today" : dayLabel(day)}: ${items.map((item) => `${item.name} (${n(item.calories)} kcal)`).join(", ")}. ${n(total)} kcal in all.${onToday && goal ? ` Today: ${n(today)} of ${n(goal)} kcal.` : ""}`;
  }
  async function estimate(body) {
    try { return await request(apiPath("/analyze"), "POST", body); }
    catch (error) {
      if (error.status === 429) throw new Error("You have used this hour's AI estimates. Try again later, or use Quick add.");
      if (error.status === 503) throw new Error("AI estimates are unavailable on this host. Use foods from Quick add, like “2 eggs and a banana”.");
      if (error.status === 422) throw new Error("Nothing recognisable as food there. Try naming the food and amount.");
      throw new Error(`Could not estimate: ${error.message}`);
    }
  }
  async function composerSubmit({ text, images }) {
    if (!state.installed) throw new Error("Install Calorie Counter first.");
    const image = images?.[0]?.dataUrl || "";
    if (image && (/\b(receipt|grocer(y|ies)|shopping)\b/i.test(text) || !text && /receipt/i.test(images[0].name || ""))) {
      const note = await readReceipt(dataUrlToBlob(image));
      return { status: "Receipt read.", message: note };
    }
    const meal = promptMeal(text) || guessMeal(), day = promptDay(text);
    if (image) {
      const payload = await estimate({ image: await fileToDataUrl(dataUrlToBlob(image)), hint: text });
      const items = payload.items || [];
      await addEntries(items, meal, "photo", day);
      return { status: `Logged ${items.length} item${items.length === 1 ? "" : "s"} from the photo.`, message: loggedMessage(items, meal, day) };
    }
    const weight = promptWeight(text);
    if (weight) {
      await request(apiPath(`/weights/${weight.day}`), "PUT", { weightKg: weight.kg });
      message("notice", "Weigh-in saved."); await load();
      return { status: "Weigh-in saved.", message: `Saved ${fmtWeight(weight.kg)} for ${weight.day === localDay(new Date()) ? "today" : dayLabel(weight.day)}.` };
    }
    let items = matchLocally(text), via = "text";
    if (!items) { items = (await estimate({ description: text })).items || []; }
    else via = "quick";
    await addEntries(items, meal, via, day);
    return { status: `Logged ${items.length} item${items.length === 1 ? "" : "s"}.`, message: loggedMessage(items, meal, day) };
  }
  function registerComposer() {
    const composer = window.ForkMeshComposer;
    if (!composer?.registerPageHandler || state.unregisterComposer) return;
    state.unregisterComposer = composer.registerPageHandler({
      id: "calories",
      label: "log",
      title: "Log this to your Calorie Counter diary",
      placeholder: "what did you eat? “2 eggs and toast for breakfast”, “weighed 72.5 kg”, or attach a meal photo or receipt",
      busyMessage: "Logging to your diary…",
      emptyMessage: "Type what you ate, or attach a meal photo or receipt.",
      isActive: () => { const app = $(`[data-calories-app]`); return Boolean(state.installed && app?.isConnected && !app.classList.contains("hidden")); },
      submit: composerSubmit,
    });
  }

  // --- barcode -----------------------------------------------------------------------
  function productCard(product) {
    const basis = product.perServing ? product.perServing : product.per100g;
    const label = product.perServing ? `per serving${product.servingSize ? ` (${product.servingSize})` : ""}` : "per 100 g";
    return `<p class="font-semibold text-foreground">${escapeHtml(product.name)}</p><p class="text-xs text-muted-foreground">${escapeHtml([product.brand, product.quantity, product.source === "usda" ? "USDA FoodData Central" : "Open Food Facts"].filter(Boolean).join(" · "))}</p>
      <p class="mt-2 text-sm text-foreground">${n(basis.calories)} kcal ${label}</p><p class="text-xs text-muted-foreground">P ${g(basis.proteinG)} g · C ${g(basis.carbsG)} g · F ${g(basis.fatG)} g</p>
      <button data-calories-product-add type="button" class="fm-btn fm-btn-sm fm-btn-cta mt-3">Add to diary…</button>`;
  }
  async function lookupBarcode(code) {
    const status = $(`[data-calories-scan-status]`), card = $(`[data-calories-product]`);
    status.textContent = `Looking up ${code}…`; card.classList.add("hidden");
    try {
      const payload = await request(apiPath(`/barcode/${encodeURIComponent(code)}`));
      state.product = payload.product; card.innerHTML = productCard(payload.product); card.classList.remove("hidden");
      status.textContent = payload.cached ? "From the shared product cache." : "Found.";
      stopStream("scan"); $(`[data-calories-scan-camera]`).classList.add("hidden"); $(`[data-calories-scan-stop]`).classList.add("hidden");
    } catch (error) { status.textContent = error.status === 404 ? `No product found for ${code}. Add it by hand or try the search.` : `Lookup failed: ${error.message}`; }
  }
  function openScan() {
    state.product = null; $(`[data-calories-product]`).classList.add("hidden"); $(`[data-calories-scan-status]`).textContent = ""; $(`[data-calories-scan-code]`).value = "";
    $(`[data-calories-scan-unsupported]`).classList.toggle("hidden", "BarcodeDetector" in window);
    $(`[data-calories-scan-dialog]`).showModal();
    if ("BarcodeDetector" in window) void startScan();
  }
  async function startScan() {
    const video = $(`[data-calories-scan-video]`), status = $(`[data-calories-scan-status]`);
    try {
      $(`[data-calories-scan-camera]`).classList.remove("hidden"); await openCamera("scan", video); $(`[data-calories-scan-stop]`).classList.remove("hidden");
      if (!("BarcodeDetector" in window)) { status.textContent = "Camera is on, but this browser cannot decode barcodes. Type the digits instead."; return; }
      const detector = new window.BarcodeDetector({ formats: ["ean_13", "ean_8", "upc_a", "upc_e", "code_128"] });
      status.textContent = "Point the camera at a barcode.";
      state.scanTimer = setInterval(async () => {
        if (!state.streams.scan || video.readyState < 2) return;
        try { const codes = await detector.detect(video); const value = codes.find((c) => /^\d{6,14}$/.test(c.rawValue))?.rawValue; if (value) { clearInterval(state.scanTimer); state.scanTimer = 0; $(`[data-calories-scan-code]`).value = value; await lookupBarcode(value); } }
        catch { /* keep scanning */ }
      }, 350);
    } catch (error) { $(`[data-calories-scan-camera]`).classList.add("hidden"); status.textContent = `Camera unavailable: ${error.message}. Type the barcode digits instead.`; }
  }
  function openPortion(product, source) {
    state.product = { ...product, source: source || "barcode" };
    $(`[data-calories-portion-title]`).textContent = product.name;
    $(`[data-calories-portion-brand]`).textContent = [product.brand, product.quantity].filter(Boolean).join(" · ");
    const basis = $(`[data-calories-portion-basis]`);
    basis.innerHTML = [product.perServing ? `<option value="serving">1 serving${product.servingSize ? ` (${escapeHtml(product.servingSize)})` : ""}</option>` : "", `<option value="100g">100 g</option>`, `<option value="1g">1 g</option>`].join("");
    $(`[data-calories-portion-amount]`).value = 1; $(`[data-calories-portion-meal]`).value = guessMeal();
    previewPortion(); $(`[data-calories-portion-dialog]`).showModal();
  }
  function portionValues() {
    const product = state.product, basis = $(`[data-calories-portion-basis]`).value, amount = Number($(`[data-calories-portion-amount]`).value || 0);
    const per = basis === "serving" ? product.perServing : product.per100g; const factor = basis === "1g" ? amount / 100 : amount;
    const portion = basis === "serving" ? `${amount} serving${amount === 1 ? "" : "s"}${product.servingSize ? ` (${product.servingSize})` : ""}` : basis === "1g" ? `${amount} g` : `${amount * 100} g`;
    return { name: product.name, brand: product.brand, barcode: product.barcode, portion, calories: Math.round(per.calories * factor), proteinG: +(per.proteinG * factor).toFixed(1), carbsG: +(per.carbsG * factor).toFixed(1), fatG: +(per.fatG * factor).toFixed(1) };
  }
  function previewPortion() { if (!state.product) return; const v = portionValues(); $(`[data-calories-portion-preview]`).textContent = `${n(v.calories)} kcal · P ${g(v.proteinG)} g · C ${g(v.carbsG)} g · F ${g(v.fatG)} g`; }
  async function savePortion(event) {
    event.preventDefault(); if (!state.product) return;
    try { await addEntries([portionValues()], $(`[data-calories-portion-meal]`).value, state.product.source); $(`[data-calories-portion-dialog]`).close(); $(`[data-calories-scan-dialog]`).close(); if (state.product.source === "search") { $(`[data-calories-search-query]`).value = ""; state.searchQuery = ""; searchOpen(false); } }
    catch (error) { message("error", `Could not save: ${error.message}`); }
  }

  // --- search ------------------------------------------------------------------------
  function searchOpen(open) {
    $(`[data-calories-search-panel]`)?.classList.toggle("hidden", !open);
    $(`[data-calories-search-query]`)?.setAttribute("aria-expanded", String(open));
  }
  function searchHighlight(index) {
    const options = $$(`[data-calories-food]`), input = $(`[data-calories-search-query]`);
    if (!options.length) return;
    state.searchActive = (index + options.length) % options.length;
    options.forEach((option, i) => option.setAttribute("aria-selected", String(i === state.searchActive)));
    options[state.searchActive].scrollIntoView?.({ block: "nearest" });
    input.setAttribute("aria-activedescendant", options[state.searchActive].id);
  }
  function foodLine(food) {
    const serving = food.perServing ? ` · ${n(food.perServing.calories)} kcal${food.servingSize ? ` per ${food.servingSize}` : " per serving"}` : "";
    return `${n(food.per100g.calories)} kcal /100 g${serving} · P ${g(food.per100g.proteinG)} · C ${g(food.per100g.carbsG)} · F ${g(food.per100g.fatG)}`;
  }
  function renderSearch(query, foods) {
    state.searchResults = foods; state.searchActive = -1;
    $(`[data-calories-search-query]`).removeAttribute("aria-activedescendant");
    $(`[data-calories-search-status]`).textContent = foods.length ? `${foods.length} ${foods.length === 1 ? "match" : "matches"} ·` : `Nothing found for “${query}” yet. Keep typing, try the brand first, or add it by hand ·`;
    $(`[data-calories-search-results]`).innerHTML = foods.map((food, i) => `<button id="calories-food-${i}" data-calories-food="${i}" role="option" aria-selected="false" tabindex="-1" type="button" class="cal-search-option"><span class="block font-semibold text-foreground">${escapeHtml(food.name)}</span><span class="block text-xs text-muted-foreground">${escapeHtml([food.brand, food.source === "usda" ? "USDA" : ""].filter(Boolean).join(" · "))}${food.brand || food.source === "usda" ? " · " : ""}${escapeHtml(foodLine(food))}</span></button>`).join("");
    searchOpen(true);
  }
  async function searchFoods(query) {
    const key = query.toLowerCase();
    state.searchQuery = key;
    if (state.searchCache.has(key)) { renderSearch(query, state.searchCache.get(key)); return; }
    $(`[data-calories-search-status]`).textContent = "Searching…"; searchOpen(true);
    try {
      const payload = await request(apiPath(`/foods?q=${encodeURIComponent(query)}`));
      state.searchCache.set(key, payload.foods || []);
      if (state.searchCache.size > 60) state.searchCache.delete(state.searchCache.keys().next().value);
      if (state.searchQuery === key) renderSearch(query, payload.foods || []);
    } catch (error) { if (state.searchQuery === key) $(`[data-calories-search-status]`).textContent = `Search failed: ${error.message} ·`; }
  }
  function queueSearch() {
    clearTimeout(state.searchTimer);
    const query = $(`[data-calories-search-query]`).value.trim().replace(/\s+/g, " ");
    if (query.length < 2) { state.searchQuery = ""; searchOpen(false); return; }
    if (query.toLowerCase() === state.searchQuery) return;
    state.searchTimer = setTimeout(() => void searchFoods(query), state.searchCache.has(query.toLowerCase()) ? 0 : 250);
  }
  function pickFood(index) {
    const food = state.searchResults[index];
    if (!food) return;
    searchOpen(false); openPortion(food, "search");
  }
  function searchKeys(event) {
    const open = !$(`[data-calories-search-panel]`).classList.contains("hidden");
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      if (!open && state.searchResults.length) searchOpen(true);
      searchHighlight(state.searchActive + (event.key === "ArrowDown" ? 1 : state.searchActive < 0 ? 0 : -1));
    } else if (event.key === "Enter") {
      event.preventDefault();
      const query = event.target.value.trim().replace(/\s+/g, " ");
      if (state.searchActive >= 0) pickFood(state.searchActive);
      else if (state.searchResults.length && query.toLowerCase() === state.searchQuery) pickFood(0);
      else if (query.length >= 2) { clearTimeout(state.searchTimer); void searchFoods(query); }
    } else if (event.key === "Escape" && open) {
      event.preventDefault(); event.stopPropagation(); searchOpen(false);
    }
  }

  // --- weight -----------------------------------------------------------------------------
  function openWeight(day = state.logDay) {
    const existing = (state.overview?.weights || []).find((p) => p.day === day);
    $(`[data-calories-weight-day]`).value = day;
    $(`[data-calories-weight-value]`).value = existing ? g(toDisplayWeight(existing.weightKg)) : (state.overview?.latestWeight?.weightKg ? g(toDisplayWeight(state.overview.latestWeight.weightKg)) : "");
    $(`[data-calories-weight-delete]`).classList.toggle("hidden", !existing);
    $(`[data-calories-weight-dialog]`).showModal();
  }
  async function saveWeight(event) {
    event.preventDefault();
    const day = $(`[data-calories-weight-day]`).value, weightKg = toKg($(`[data-calories-weight-value]`).value);
    try { await request(apiPath(`/weights/${day}`), "PUT", { weightKg }); $(`[data-calories-weight-dialog]`).close(); message("notice", "Weigh-in saved."); await load(); }
    catch (error) { message("error", `Could not save weight: ${error.message}`); }
  }
  async function removeWeight() {
    const day = $(`[data-calories-weight-day]`).value;
    try { await request(apiPath(`/weights/${day}`), "DELETE", {}); $(`[data-calories-weight-dialog]`).close(); await load(); }
    catch (error) { message("error", `Could not remove weight: ${error.message}`); }
  }

  // --- challenges -----------------------------------------------------------------------
  function challengeCard(item) {
    const status = { active: "Active", upcoming: "Upcoming", closed: "Finished" }[item.status] || item.status;
    const mine = { invited: "You are invited", accepted: "You are in", declined: "You declined", left: "You left", none: "" }[item.myStatus] || "";
    return `<button data-calories-challenge-open="${escapeHtml(item.id)}" type="button" class="rounded-lg border border-border bg-card p-4 text-left hover:bg-secondary">
      <div class="flex items-start justify-between gap-2"><span class="font-semibold text-foreground">${escapeHtml(item.title)}</span><span class="fm-label">${status}</span></div>
      <p class="mt-1 text-xs text-muted-foreground">${escapeHtml(KIND_LABELS[item.kind] || item.kind)}</p>
      <p class="mt-2 text-xs text-muted-foreground">${dayLabel(item.startsOn)} → ${dayLabel(item.endsOn)} · ${item.participants} in${item.invited ? ` · ${item.invited} invited` : ""} · by ${escapeHtml(item.createdBy)}</p>
      ${mine ? `<p class="mt-2 text-xs font-semibold ${item.myStatus === "invited" ? "text-primary" : "text-foreground"}">${mine}</p>` : ""}</button>`;
  }
  function renderChallenges() {
    const items = state.overview?.challenges || [];
    $(`[data-calories-challenges]`).innerHTML = items.map(challengeCard).join("");
    $(`[data-calories-challenges-empty]`).classList.toggle("hidden", items.length > 0);
    const invites = items.filter((item) => item.myStatus === "invited" && item.status !== "closed").length;
    const badge = $(`[data-calories-challenge-badge]`); badge.textContent = String(invites); badge.classList.toggle("hidden", !invites);
  }
  function parseInvitees(text) {
    const me = String(state.overview?.actor || "").toLowerCase();
    return [...new Set(String(text || "").split(/[\s,]+/).map((name) => name.trim().replace(/^@+/, "").toLowerCase()).filter((name) => name && name !== me))];
  }
  function openChallengeEditor(item = null) {
    $(`[data-calories-challenge-dialog-title]`).textContent = item ? "Edit challenge" : "New challenge";
    $(`[data-calories-challenge-id]`).value = item?.id || "";
    $(`[data-calories-challenge-title]`).value = item?.title || "";
    $(`[data-calories-challenge-kind]`).value = item?.kind || "weight_loss_percent"; $(`[data-calories-challenge-kind]`).disabled = Boolean(item);
    $(`[data-calories-challenge-start]`).value = item?.startsOn || state.day; $(`[data-calories-challenge-start]`).disabled = Boolean(item);
    $(`[data-calories-challenge-end]`).value = item?.endsOn || shiftDay(state.day, 28);
    $(`[data-calories-challenge-description]`).value = item?.description || "";
    $(`[data-calories-challenge-invite]`).value = "";
    $(`[data-calories-challenge-invite-hint]`).textContent = `Separate names with commas or spaces. ${item ? "Friends already invited stay on the board; add more usernames here." : "Friends install Calorie Counter for themselves, then accept from their Challenges tab."}`;
    $(`[data-calories-challenge-dialog]`).showModal();
  }
  async function saveChallenge(event) {
    event.preventDefault();
    const id = $(`[data-calories-challenge-id]`).value;
    const invite = parseInvitees($(`[data-calories-challenge-invite]`).value);
    const body = { title: $(`[data-calories-challenge-title]`).value, description: $(`[data-calories-challenge-description]`).value, endsOn: $(`[data-calories-challenge-end]`).value, invite };
    if (!id) { body.kind = $(`[data-calories-challenge-kind]`).value; body.startsOn = $(`[data-calories-challenge-start]`).value; }
    try {
      const payload = await request(apiPath(`/challenges${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body); $(`[data-calories-challenge-dialog]`).close();
      const invited = payload.invited || [], skipped = payload.skipped || [];
      message("notice", `${id ? "Challenge updated." : "Challenge created."}${invited.length ? ` Invited ${invited.join(", ")}.` : ""}${skipped.length ? ` Skipped ${skipped.join(", ")}: not a ForkMesh account, or the challenge is full.` : ""}`);
      await load(); openBoard(payload.challenge);
    }
    catch (error) { message("error", `Could not save challenge: ${error.message}`); }
  }
  function boardRow(member, kind) {
    const isWeight = kind.startsWith("weight");
    const scores = state.board.leaderboard.filter((m) => m.measured).map((m) => m.score);
    const max = Math.max(0.0001, ...scores.map(Math.abs));
    const width = member.measured ? Math.max(0, Math.min(100, member.score / max * 100)) : 0;
    return `<div class="flex items-center gap-3 rounded-md border border-border p-2 ${member.isMe ? "bg-secondary" : ""}"><span class="w-6 text-center text-sm font-semibold text-foreground">${member.rank || "–"}</span><span class="w-32 truncate text-sm text-foreground">${escapeHtml(member.name)}${member.isMe ? " (you)" : ""}</span><span class="flex-1"><span class="block h-2 rounded-full" style="background:var(--cal-grid)"><span class="block h-2 rounded-full" style="width:${width.toFixed(1)}%;background:var(--cal-series-1)"></span></span></span><span class="w-40 text-right text-xs text-muted-foreground">${escapeHtml(member.detail || (isWeight ? "No weigh-in yet" : "Nothing logged yet"))}</span></div>`;
  }
  function openBoard(item) {
    state.board = item;
    $(`[data-calories-board-title]`).textContent = item.title;
    $(`[data-calories-board-meta]`).textContent = `${KIND_LABELS[item.kind] || item.kind} · ${dayLabel(item.startsOn)} → ${dayLabel(item.endsOn)} · started by ${item.createdBy}${item.winner ? ` · winner: ${item.winner}` : ""}`;
    $(`[data-calories-board-description]`).textContent = item.description || "";
    const actions = [];
    if (item.status !== "closed") {
      if (item.myStatus === "invited") { actions.push(`<button data-calories-board-respond="accept" class="fm-btn fm-btn-sm fm-btn-cta">Accept challenge</button>`, `<button data-calories-board-respond="decline" class="fm-btn fm-btn-sm">Decline</button>`); }
      else if (item.myStatus === "accepted" && !item.isCreator) actions.push(`<button data-calories-board-respond="decline" class="fm-btn fm-btn-sm">Leave challenge</button>`);
      else if (["none", "declined", "left"].includes(item.myStatus) && !item.isCreator) actions.push(`<span class="text-xs text-muted-foreground">Ask ${escapeHtml(item.createdBy)} for an invitation.</span>`);
      if (item.isCreator) actions.push(`<button data-calories-board-edit class="fm-btn fm-btn-sm">Edit / invite</button>`, `<button data-calories-board-close class="fm-btn fm-btn-sm">End now</button>`);
    }
    if (item.isCreator) actions.push(`<button data-calories-board-delete class="fm-btn fm-btn-sm fm-btn-danger">Delete</button>`);
    $(`[data-calories-board-actions]`).innerHTML = actions.join("");
    $(`[data-calories-board]`).innerHTML = item.leaderboard?.length ? item.leaderboard.map((m) => boardRow(m, item.kind)).join("") : `<div class="fm-blankslate"><p class="fm-blankslate-title">Nobody has accepted yet</p><p class="fm-blankslate-desc">Standings appear once invited friends accept.</p></div>`;
    const pending = (item.pending || []).map((m) => `${m.name} (${m.status})`);
    $(`[data-calories-board-pending]`).textContent = pending.length ? `Invited or left: ${pending.join(", ")}` : "";
    $(`[data-calories-board-dialog]`).showModal();
  }
  async function loadBoard(id) {
    try { const payload = await request(apiPath(`/challenges/${id}`)); openBoard(payload.challenge); }
    catch (error) { message("error", `Could not open challenge: ${error.message}`); }
  }
  async function boardAction(event) {
    const respond = event.target.closest("[data-calories-board-respond]");
    const item = state.board; if (!item) return;
    try {
      if (respond) { const payload = await request(apiPath(`/challenges/${item.id}/respond`), "POST", { accept: respond.dataset.caloriesBoardRespond === "accept" }); if (payload.challenge) openBoard(payload.challenge); else { state.board = null; $(`[data-calories-board-dialog]`).close(); } await load(); }
      else if (event.target.closest("[data-calories-board-edit]")) { $(`[data-calories-board-dialog]`).close(); openChallengeEditor(item); }
      else if (event.target.closest("[data-calories-board-close]")) { if (!confirm("End this challenge now? Standings freeze at today's values.")) return; const payload = await request(apiPath(`/challenges/${item.id}`), "PATCH", { title: item.title, status: "closed", endsOn: state.day < item.startsOn ? item.startsOn : state.day }); openBoard(payload.challenge); await load(); }
      else if (event.target.closest("[data-calories-board-delete]")) { if (!confirm("Delete this challenge for everyone?")) return; await request(apiPath(`/challenges/${item.id}`), "DELETE", {}); $(`[data-calories-board-dialog]`).close(); await load(); }
    } catch (error) { message("error", `Challenge action failed: ${error.message}`); }
  }

  // --- load ------------------------------------------------------------------------------------
  function renderAll() { renderDiary(); renderQuick(); renderGoals(); renderChallenges(); if (state.tab === "trends") renderTrends(); icons(); }
  async function load() {
    try {
      const payload = await request(apiPath(`?day=${state.day}`)); state.installed = payload.installed !== false;
      state.overview = state.installed ? payload : null; message("error", ""); renderInstall(); if (state.installed) renderAll();
    } catch (error) { if (error.status === 409) { state.installed = false; state.overview = null; renderInstall(); } else message("error", `Calorie Counter is unavailable: ${error.message}`); }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Calorie Counter.");
    try {
      const payload = await request(apiPath(`?day=${state.day}`)); state.installed = payload.installed !== false;
      state.overview = state.installed ? payload : null; renderInstall(); if (state.installed) renderAll();
      const wanted = new URLSearchParams(location.search).get("challenge");
      if (wanted && state.installed) { showTab("challenges"); void loadBoard(wanted); }
      else if (wanted) message("notice", "Install Calorie Counter to answer this challenge invitation.");
    } catch (error) { return message("error", `Calorie Counter is unavailable: ${error.message}`); }

    $(`[data-calories-install]`)?.addEventListener("click", () => void install());
    $(`[data-calories-manage]`)?.addEventListener("click", () => $(`[data-calories-manage-dialog]`).showModal());
    $(`[data-calories-export]`)?.addEventListener("click", () => void exportMine());
    $(`[data-calories-uninstall]`)?.addEventListener("click", () => void uninstall());
    $$(`[data-calories-tab]`).forEach((button) => button.addEventListener("click", () => showTab(button.dataset.caloriesTab)));
    $(`[data-calories-log-day]`)?.addEventListener("change", (event) => { if (event.target.value) { state.logDay = event.target.value; renderQuick(); } });
    $(`[data-calories-log-today]`)?.addEventListener("click", () => { state.logDay = localDay(new Date()); $(`[data-calories-log-day]`).value = state.logDay; renderQuick(); });
    $(`[data-calories-quick-filter]`)?.addEventListener("input", renderQuick);
    $(`[data-calories-receipt-file]`)?.addEventListener("change", (event) => void pickReceipt(event));
    $(`[data-calories-quick-lists]`)?.addEventListener("click", (event) => {
      const add = event.target.closest("[data-calories-quick-add]"), remove = event.target.closest("[data-calories-quick-remove]");
      const save = event.target.closest("[data-calories-quick-save]"), saveCommon = event.target.closest("[data-calories-quick-save-common]");
      if (add) void quickAdd(add.dataset.caloriesQuickAdd);
      else if (remove) void removeMyFood(Number(remove.dataset.caloriesQuickRemove));
      else if (save) void starFood(state.quick.lists.frequent[Number(save.dataset.caloriesQuickSave)]);
      else if (saveCommon) void starFood(state.quick.lists.common[Number(saveCommon.dataset.caloriesQuickSaveCommon)]);
    });
    if (window.ForkMeshComposer) registerComposer();
    else window.addEventListener("forkmesh:composer-ready", registerComposer, { once: true });
    $(`[data-calories-open-manual]`)?.addEventListener("click", () => openEntry());
    $(`[data-calories-open-photo]`)?.addEventListener("click", openPhoto);
    $(`[data-calories-open-scan]`)?.addEventListener("click", openScan);
    $(`[data-calories-open-weight]`)?.addEventListener("click", () => openWeight());
    $(`[data-calories-entries]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-calories-entry-edit]"); if (button) openEntry((state.overview?.entries || []).find((item) => item.id === button.dataset.caloriesEntryEdit)); });
    $(`[data-calories-entry-form]`)?.addEventListener("submit", saveEntry);
    $(`[data-calories-entry-delete]`)?.addEventListener("click", () => void removeEntry());
    $$(`[data-calories-table-toggle]`).forEach((button) => button.addEventListener("click", () => { state.tables[button.dataset.caloriesTableToggle] = !state.tables[button.dataset.caloriesTableToggle]; renderTrends(); }));
    $(`[data-calories-table="weight"]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-calories-weight-edit]"); if (button) openWeight(button.dataset.caloriesWeightEdit); });
    $(`[data-calories-goals-form]`)?.addEventListener("submit", saveGoals);
    $(`[data-calories-goal="units"]`)?.addEventListener("change", () => { const unit = formImperial(); $$(`[data-calories-weight-unit]`).forEach((node) => { node.textContent = unit ? "lb" : "kg"; }); $$(`[data-calories-height-unit]`).forEach((node) => { node.textContent = unit ? "in" : "cm"; }); });
    $(`[data-calories-reminder-add]`)?.addEventListener("click", () => { const host = $(`[data-calories-reminders]`); if ($$(`[data-calories-reminder]`).length >= 8) return; if (!$$(`[data-calories-reminder]`).length) host.innerHTML = ""; host.insertAdjacentHTML("beforeend", reminderRow({ time: "12:30", label: "Log lunch", enabled: true }, $$(`[data-calories-reminder]`).length)); });
    $(`[data-calories-reminders]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-calories-reminder-remove]"); if (button) button.closest("[data-calories-reminder]").remove(); });
    $(`[data-calories-settings-form]`)?.addEventListener("submit", saveSettings);
    $(`[data-calories-export-mine]`)?.addEventListener("click", () => void exportMine());
    $(`[data-calories-photo-start]`)?.addEventListener("click", () => void startPhotoCamera());
    $(`[data-calories-photo-snap]`)?.addEventListener("click", () => void snapPhoto());
    $(`[data-calories-photo-file]`)?.addEventListener("change", (event) => void pickPhoto(event));
    $(`[data-calories-photo-analyze]`)?.addEventListener("click", () => void analyzePhoto());
    $(`[data-calories-photo-save]`)?.addEventListener("click", () => void savePhotoItems());
    $(`[data-calories-scan-start]`)?.addEventListener("click", () => void startScan());
    $(`[data-calories-scan-stop]`)?.addEventListener("click", () => { stopStream("scan"); $(`[data-calories-scan-camera]`).classList.add("hidden"); $(`[data-calories-scan-stop]`).classList.add("hidden"); });
    $(`[data-calories-scan-form]`)?.addEventListener("submit", (event) => { event.preventDefault(); const code = $(`[data-calories-scan-code]`).value.replace(/\D/g, ""); if (code.length >= 6) void lookupBarcode(code); });
    $(`[data-calories-product]`)?.addEventListener("click", (event) => { if (event.target.closest("[data-calories-product-add]") && state.product) openPortion(state.product, "barcode"); });
    $(`[data-calories-search-query]`)?.addEventListener("input", queueSearch);
    $(`[data-calories-search-query]`)?.addEventListener("keydown", searchKeys);
    $(`[data-calories-search-query]`)?.addEventListener("focus", () => { if (state.searchQuery && state.searchResults.length) searchOpen(true); });
    $(`[data-calories-search-results]`)?.addEventListener("mousedown", (event) => event.preventDefault());
    $(`[data-calories-search-results]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-calories-food]"); if (button) pickFood(Number(button.dataset.caloriesFood)); });
    $(`[data-calories-app]`)?.addEventListener("pointerdown", (event) => { if (!event.target.closest?.("[data-calories-search]")) searchOpen(false); });
    $(`[data-calories-portion-form]`)?.addEventListener("submit", savePortion);
    $(`[data-calories-portion-basis]`)?.addEventListener("change", previewPortion);
    $(`[data-calories-portion-amount]`)?.addEventListener("input", previewPortion);
    $(`[data-calories-weight-form]`)?.addEventListener("submit", saveWeight);
    $(`[data-calories-weight-delete]`)?.addEventListener("click", () => void removeWeight());
    $(`[data-calories-challenge-new]`)?.addEventListener("click", () => openChallengeEditor());
    $(`[data-calories-challenge-form]`)?.addEventListener("submit", saveChallenge);
    $(`[data-calories-challenges]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-calories-challenge-open]"); if (button) void loadBoard(button.dataset.caloriesChallengeOpen); });
    $(`[data-calories-board-actions]`)?.addEventListener("click", (event) => void boardAction(event));
    $$(`[data-calories-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    $$(`dialog[data-calories-photo-dialog], dialog[data-calories-scan-dialog]`).forEach((dialog) => dialog.addEventListener("close", () => { stopStream("photo"); stopStream("scan"); }));
  }
  return init();
});
