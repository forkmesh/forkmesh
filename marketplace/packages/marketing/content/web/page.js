// Organization Marketing plugin: shared content pipeline and channel analytics.
window.ForkMeshDashboard.registerPage("marketing", ({
  select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel,
}) => {
  const state = { get session() { return session(); }, marketing: {
    organizations: [], organization: "", installed: false, content: [], metrics: [],
    settings: {}, summary: {}, channelCatalog: [], addingChannel: "",
    canManageSettings: false, ads: [], adsSummary: {}, adPlatforms: [],
    experiments: [], experimentsSummary: {}, funnel: [], funnelReport: {},
    redditThreads: [], redditDesk: {}, posts: [], channelPlaybooks: {}, performance: null,
    campaigns: [], campaignTemplates: [], keywords: [], suggestions: [], assistantAvailable: false,
    traffic: null, assistant: null, emailSuggestions: null, discordPosts: null, readyPosts: {}, readyPost: null,
    appStore: null,
  }, view: {
    rangeDays: 30, chartMetric: "reach", channelSort: "reach", status: "", contentLimit: 50,
    page: "dashboard", adStatus: "", adChannel: "", adCurrency: "", experimentStatus: "",
    redditStatus: "", redditPostsAll: false, healthFilter: "", performanceDays: 28, performanceProject: "",
    campaignStatus: "open", keywordView: "tracked", appStoreRange: 30, appStoreMetric: "downloads",
  }};

  const DAY = 86400000;
  const STALE_DAYS = 14;
  const REVIEW_STUCK_DAYS = 3;
  const SERIES = ["--mk-s1", "--mk-s2", "--mk-s3", "--mk-s4"];
  const STATUSES = [["draft", "Draft"], ["review", "Review"], ["queued", "Queued"],
    ["scheduled", "Scheduled"], ["published", "Published"], ["archived", "Archived"]];
  const METRIC_LABELS = { reach: "Reach", engagements: "Engagements", followers: "Followers" };
  const svgIcon = (paths) => `<svg class="mk-icon" viewBox="0 0 24 24" aria-hidden="true">${paths}</svg>`;
  const STATUS_ICONS = {
    draft: svgIcon('<path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 1 1 3 3L7 19l-4 1 1-4Z"/>'),
    review: svgIcon('<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/>'),
    queued: svgIcon('<path d="M4 6h16M4 12h10M4 18h6"/>'),
    scheduled: svgIcon('<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>'),
    published: svgIcon('<path d="m5 12 5 5L20 7"/>'),
    archived: svgIcon('<rect x="3" y="4" width="18" height="4" rx="1"/><path d="M5 8v11h14V8M10 12h4"/>'),
  };
  const WARN_ICON = svgIcon('<path d="M12 9v4M12 17h.01"/><path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"/>');
  const CLOCK_ICON = STATUS_ICONS.scheduled;
  const CHECK_ICON = STATUS_ICONS.published;
  const INFO_ICON = svgIcon('<circle cx="12" cy="12" r="9"/><path d="M12 11v5M12 8h.01"/>');
  const ALERT_ICON = svgIcon('<circle cx="12" cy="12" r="9"/><path d="M12 8v4M12 16h.01"/>');
  const CLOSE_ICON = svgIcon('<path d="M18 6 6 18M6 6l12 12"/>');
  const PLUS_ICON = svgIcon('<path d="M12 5v14M5 12h14"/>');
  const BLANK_ICONS = {
    chart: '<path d="M3 3v18h18"/><path d="m7 15 4-4 3 3 5-6"/>',
    ads: '<path d="m3 11 18-5v12L3 14v-3Z"/><path d="M11.6 16.8a3 3 0 1 1-5.8-1.6"/>',
    funnel: '<path d="M3 4h18l-7 8v6l-4 2v-8Z"/>',
    campaign: '<path d="M4 22V4M4 4h13l-2 4 2 4H4"/>',
    search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>',
  };
  const blankslateBody = (icon, title, desc = "", action = "") =>
    `${svgIcon(BLANK_ICONS[icon] || BLANK_ICONS.search)}<p class="fm-blankslate-title">${title}</p>${desc ? `<p class="fm-blankslate-desc">${desc}</p>` : ""}${action}`;
  const blankslate = (icon, title, desc = "", action = "", style = "") =>
    `<div class="mk-empty fm-blankslate"${style ? ` style="${style}"` : ""}>${blankslateBody(icon, title, desc, action)}</div>`;

  const compact = new Intl.NumberFormat(undefined, { notation: "compact", maximumFractionDigits: 1 });
  const fmtCompact = (value) => compact.format(Number(value || 0));
  const fmtFull = (value) => Number(value || 0).toLocaleString();
  const shortDate = (ts) => new Date(ts).toLocaleDateString(undefined, { month: "short", day: "numeric" });
  const longDate = (ts) => new Date(ts).toLocaleString(undefined, {
    weekday: "short", month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
  const timeOnly = (ts) => new Date(ts).toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
  const startOfDay = (ts) => { const date = new Date(ts); date.setHours(0, 0, 0, 0); return date.getTime(); };

  // The host helper only describes the past; schedules also need the future.
  function relativeLabel(ts) {
    const diff = Number(ts || 0) - Date.now();
    if (diff <= 0) return relativeTimeLabel(ts);
    const minutes = Math.round(diff / 60000);
    if (minutes < 60) return `in ${Math.max(1, minutes)} min`;
    const hours = Math.round(minutes / 60);
    if (hours < 48) return `in ${hours} hour${hours === 1 ? "" : "s"}`;
    return `in ${Math.round(hours / 24)} days`;
  }
  function initials(value) {
    const words = String(value || "").trim().split(/[\s@._-]+/).filter(Boolean);
    if (!words.length) return "?";
    return (words.length > 1 ? words[0][0] + words[1][0] : words[0][0]).toUpperCase();
  }

  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    if (state.session?.sessionToken) headers.authorization = `Bearer ${state.session.sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store",
      body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; error.payload = payload; throw error;
    }
    return payload;
  }

  function message(kind, value = "") {
    const node = $(`[data-marketing-${kind}]`); if (!node) return;
    (node.querySelector(`[data-marketing-${kind}-text]`) || node).textContent = value; node.classList.toggle("hidden", !value);
    if (value && kind === "error") $(`[data-marketing-notice]`)?.classList.add("hidden");
  }
  function orgPath(suffix = "") { return `/api/marketing/${encodeURIComponent(state.marketing.organization)}${suffix}`; }
  function crmPath() { return `/dashboard/crm?org=${encodeURIComponent(state.marketing.organization || "")}`; }
  function pluginPath(suffix = "") { return `/api/plugins/${encodeURIComponent(state.marketing.organization)}/marketing${suffix}`; }
  function selectedOrg() { return state.marketing.organizations.find((org) => org.name === state.marketing.organization); }
  function canManage() { return ["owner", "admin"].includes(selectedOrg()?.role || ""); }
  function contentById(id) { return state.marketing.content.find((item) => item.id === id); }
  function channelLabel(id) {
    const configured = (state.marketing.settings.channels || []).find((item) => item.id === id);
    const catalog = state.marketing.channelCatalog.find((item) => item.id === id);
    return configured?.displayName || catalog?.name || id;
  }

  function renderInstall() {
    const installed = Boolean(selectedOrg()?.installed); state.marketing.installed = installed;
    $(`[data-marketing-install-panel]`)?.classList.toggle("hidden", installed);
    $(`[data-marketing-app]`)?.classList.toggle("hidden", !installed);
    syncHeader();
    $$(`[data-marketing-menu] [data-marketing-export-all]`).forEach((node) => { node.hidden = !canManage(); });
    $(`[data-marketing-install-org]`).textContent = state.marketing.organization;
    $(`[data-marketing-install]`)?.classList.toggle("hidden", !canManage());
    $(`[data-marketing-install-denied]`)?.classList.toggle("hidden", canManage());
  }

  // Header controls belong to one section; the catalog keeps the dashboard's.
  function syncHeader() {
    const page = state.view.page === "catalog" ? "dashboard" : state.view.page;
    $$(`[data-marketing-requires-install]`).forEach((node) => {
      node.hidden = !state.marketing.installed
        || ("marketingDashboardOnly" in node.dataset && page !== "dashboard")
        || ("marketingAdsOnly" in node.dataset && page !== "ads")
        || ("marketingGrowthOnly" in node.dataset && page !== "growth")
        || ("marketingRedditOnly" in node.dataset && page !== "reddit")
        || ("marketingAppStoreOnly" in node.dataset && page !== "app-store")
        || ("marketingChannelsOnly" in node.dataset && page !== "channels")
        || ("marketingWebsiteOnly" in node.dataset && page !== "website");
    });
    const picker = $(`[data-marketing-performance-project]`);
    if (picker) picker.hidden = !state.marketing.installed || page !== "website"
      || (state.marketing.performance?.projects || []).length < 2;
    $$(`[data-marketing-page]`).forEach((button) =>
      button.setAttribute("aria-pressed", String(button.dataset.marketingPage === (["website", "reddit", "app-store"].includes(page) ? "channels" : page))));
  }

  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  async function installPlugin() {
    try { await request(pluginPath("/install"), "POST", {}); selectedOrg().installed = true;
      message("notice", "Marketing installed for this organization."); await load(); }
    catch (error) { message("error", `Could not install Marketing: ${error.message}`); }
  }
  async function exportAll() {
    try { const payload = await request(pluginPath("/export"));
      downloadJson(payload.export || {}, payload.filename || "forkmesh-marketing-export.json"); }
    catch (error) { message("error", `Export failed: ${error.message}`); }
  }
  async function uninstallPlugin() {
    if (!confirm(`Uninstall Marketing from ${state.marketing.organization}? A JSON export downloads first, then all content, ads, metrics, and settings are deleted.`)) return;
    try { const snapshot = await request(pluginPath("/export"));
      if (snapshot.hasData) downloadJson(snapshot.export || {}, snapshot.filename);
      await request(pluginPath("/uninstall"), "POST", {}); selectedOrg().installed = false;
      $(`[data-marketing-settings-dialog]`)?.close(); message("notice", "Marketing uninstalled."); await load(); }
    catch (error) { message("error", `Could not uninstall Marketing: ${error.message}`); }
  }

  // ---- Metric history -------------------------------------------------------
  // Snapshots are logged by hand at irregular times, so every figure is read
  // as "the latest snapshot at or before t" rather than as a daily series.

  function seriesByChannel() {
    const series = new Map();
    for (const metric of state.marketing.metrics || []) {
      if (!series.has(metric.channel)) series.set(metric.channel, []);
      series.get(metric.channel).push(metric);
    }
    for (const list of series.values()) list.sort((a, b) => a.observedAt - b.observedAt);
    return series;
  }
  function snapshotAt(list, ts) {
    let found = null;
    for (const item of list || []) { if (item.observedAt <= ts) found = item; else break; }
    return found;
  }
  function totalsAt(series, ts, onlyChannels = null) {
    const totals = { reach: 0, engagements: 0, followers: 0, channels: 0 };
    for (const [channel, list] of series) {
      if (onlyChannels && !onlyChannels.has(channel)) continue;
      const snapshot = snapshotAt(list, ts); if (!snapshot) continue;
      totals.reach += snapshot.reach; totals.engagements += snapshot.engagements;
      totals.followers += snapshot.followers; totals.channels += 1;
    }
    return totals;
  }
  function channelColors(series) {
    // Colors follow the channel, not the current metric or range: the four
    // channels with the largest latest reach keep their slot while toggling.
    const ranked = [...series.entries()]
      .map(([channel, list]) => [channel, list[list.length - 1]?.reach || 0])
      .sort((a, b) => b[1] - a[1]).slice(0, SERIES.length);
    return new Map(ranked.map(([channel], index) => [channel, SERIES[index]]));
  }
  const colorFor = (colors, channel) => colors.get(channel) ? `var(${colors.get(channel)})` : "var(--mk-muted)";

  function setDelta(key, value, kind, text) {
    const node = $(`[data-marketing-delta-${key}]`); if (!node) return;
    node.className = `mk-delta mk-num ${kind || ""}`; node.textContent = text ?? value;
  }
  function percentDelta(key, current, previous) {
    if (!previous) return setDelta(key, "", "", current ? "New" : "—");
    const change = (current - previous) / previous * 100;
    setDelta(key, "", change > 0.05 ? "up" : change < -0.05 ? "down" : "",
      `${change > 0.05 ? "▲" : change < -0.05 ? "▼" : "•"} ${Math.abs(change).toFixed(1)}%`);
  }

  function sparkline(svg, values, width, height, color) {
    if (!svg) return;
    if (values.length < 2 || values.every((value) => value === values[0] && !value)) { svg.innerHTML = ""; return; }
    const min = Math.min(...values), max = Math.max(...values), span = max - min || 1;
    const x = (index) => index / (values.length - 1) * width;
    const y = (value) => height - 3 - (value - min) / span * (height - 6);
    const line = values.map((value, index) => `${index ? "L" : "M"}${x(index).toFixed(1)},${y(value).toFixed(1)}`).join("");
    const id = `mk-spark-${Math.random().toString(36).slice(2, 8)}`;
    svg.innerHTML = `<defs><linearGradient id="${id}" x1="0" x2="0" y1="0" y2="1"><stop offset="0" style="stop-color:${color};stop-opacity:.22"/><stop offset="1" style="stop-color:${color};stop-opacity:0"/></linearGradient></defs>`
      + `<path d="${line}L${width},${height}L0,${height}Z" fill="url(#${id})"/>`
      + `<path d="${line}" fill="none" style="stroke:${color}" stroke-width="1.5" stroke-linejoin="round" stroke-linecap="round"/>`
      + `<circle cx="${x(values.length - 1)}" cy="${y(values[values.length - 1])}" r="2.5" style="fill:${color};stroke:var(--mk-card)" stroke-width="1.5"/>`;
  }

  function renderKpis(series) {
    const now = Date.now(), range = state.view.rangeDays * DAY, start = now - range;
    const current = totalsAt(series, now);
    const comparable = new Set([...series.keys()].filter((channel) => snapshotAt(series.get(channel), start)));
    const before = totalsAt(series, start, comparable), after = totalsAt(series, now, comparable);
    const summary = state.marketing.summary;
    const reach = Number(summary.reach || current.reach);
    const engagements = Number(summary.engagements || current.engagements);
    const followers = Number(summary.followers || current.followers);
    $(`[data-marketing-stat-followers]`).textContent = fmtCompact(followers);
    $(`[data-marketing-stat-followers]`).title = fmtFull(followers);
    percentDelta("followers", after.followers, before.followers);
    $(`[data-marketing-stat-reach]`).textContent = fmtCompact(reach);
    $(`[data-marketing-stat-reach]`).title = fmtFull(reach);
    percentDelta("reach", after.reach, before.reach);
    $(`[data-marketing-stat-rate]`).textContent = reach ? `${(engagements / reach * 100).toFixed(1)}%` : "—";
    const samples = Array.from({ length: 12 }, (_, index) => start + range * index / 11);
    sparkline($(`[data-marketing-spark-followers]`), samples.map((ts) => totalsAt(series, ts).followers), 84, 26, "var(--mk-good)");

    const content = state.marketing.content;
    const publishedIn = (from, to) => content.filter((item) => item.status === "published"
      && item.publishedAt >= from && item.publishedAt < to).length;
    const published = publishedIn(start, now + 1), previous = publishedIn(start - range, start);
    $(`[data-marketing-published-label]`).textContent = `Published · ${state.view.rangeDays}D`;
    $(`[data-marketing-stat-published]`).textContent = fmtFull(published);
    const diff = published - previous;
    setDelta("published", "", diff > 0 ? "up" : diff < 0 ? "down" : "",
      diff ? `${diff > 0 ? "▲" : "▼"} ${Math.abs(diff)}` : "• 0");

    const today = startOfDay(now), weekEnd = today + 7 * DAY;
    const upcoming = content.filter((item) => ["queued", "scheduled"].includes(item.status)
      && item.scheduledAt >= now && item.scheduledAt < weekEnd);
    const covered = new Set(upcoming.map((item) => startOfDay(item.scheduledAt)));
    const openDays = Array.from({ length: 7 }, (_, index) => today + index * DAY).filter((day) => !covered.has(day)).length;
    $(`[data-marketing-stat-queued]`).textContent = fmtFull(upcoming.length);
    const coverage = $(`[data-marketing-coverage]`);
    coverage.textContent = openDays ? `· ${openDays} day${openDays === 1 ? "" : "s"} open` : "· every day covered";
    coverage.className = openDays ? "mk-warn" : "";
  }

  function niceStep(max, ticks = 4) {
    const raw = Math.max(max, 1) / ticks, power = 10 ** Math.floor(Math.log10(raw));
    const unit = raw / power;
    return (unit <= 1 ? 1 : unit <= 2 ? 2 : unit <= 2.5 ? 2.5 : unit <= 5 ? 5 : 10) * power;
  }

  function renderChart(series, colors) {
    const host = $(`[data-marketing-chart]`), legend = $(`[data-marketing-chart-legend]`); if (!host) return;
    const metric = state.view.chartMetric, now = Date.now(), start = now - state.view.rangeDays * DAY;
    $(`[data-marketing-chart-title]`).textContent = `${METRIC_LABELS[metric]} by channel`;
    const lines = [...colors.keys()].map((channel) => {
      const list = series.get(channel) || [];
      const carried = snapshotAt(list, start);
      const points = list.filter((item) => item.observedAt > start)
        .map((item) => ({ ts: item.observedAt, value: item[metric], logged: true }));
      if (carried) points.unshift({ ts: start, value: carried[metric], logged: false });
      return { channel, list, points, color: colorFor(colors, channel) };
    }).filter((line) => line.points.length);

    if (!lines.length) {
      legend.innerHTML = "";
      host.innerHTML = blankslate("chart", "No metrics to chart", `No metric snapshots in the last ${state.view.rangeDays} days.`, `<button data-marketing-metric-open type="button" class="fm-btn fm-btn-sm">Log metrics</button>`);
      return;
    }
    const W = 760, H = 260, L = 48, R = 122, T = 10, B = 26;
    const step = niceStep(Math.max(...lines.flatMap((line) => line.points.map((point) => point.value))));
    const top = Math.max(step, Math.ceil(Math.max(...lines.flatMap((line) => line.points.map((point) => point.value))) / step) * step);
    const x = (ts) => L + (ts - start) / (now - start) * (W - L - R);
    const y = (value) => T + (1 - value / top) * (H - T - B);
    let svg = "";
    for (let value = 0; value <= top + step / 2; value += step) {
      svg += `<line x1="${L}" x2="${W - R}" y1="${y(value)}" y2="${y(value)}" style="stroke:var(${value ? "--mk-line-soft" : "--mk-line"})" stroke-width="1"/>`
        + `<text x="${L - 8}" y="${y(value) + 3.5}" text-anchor="end">${escapeHtml(fmtCompact(value))}</text>`;
    }
    for (let index = 0; index < 5; index += 1) {
      const ts = start + (now - start) * index / 4;
      svg += `<text x="${x(ts)}" y="${H - 8}" text-anchor="${index === 0 ? "start" : index === 4 ? "end" : "middle"}">${escapeHtml(shortDate(ts))}</text>`;
    }
    for (const line of lines) {
      svg += `<path d="${line.points.map((point, index) => `${index ? "L" : "M"}${x(point.ts).toFixed(1)},${y(point.value).toFixed(1)}`).join("")}" fill="none" style="stroke:${line.color}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
      for (const point of line.points) {
        svg += point.logged
          ? `<circle cx="${x(point.ts)}" cy="${y(point.value)}" r="3" style="fill:${line.color};stroke:var(--mk-card)" stroke-width="2"/>`
          : `<circle cx="${x(point.ts)}" cy="${y(point.value)}" r="2.5" style="fill:var(--mk-card);stroke:${line.color}" stroke-width="1.5"/>`;
      }
    }
    const labels = lines.map((line) => ({ line, y: y(line.points[line.points.length - 1].value) })).sort((a, b) => a.y - b.y);
    for (let index = 1; index < labels.length; index += 1) labels[index].y = Math.max(labels[index].y, labels[index - 1].y + 13);
    for (const { line, y: labelY } of labels) {
      const last = line.points[line.points.length - 1];
      const name = channelLabel(line.channel);
      svg += `<text x="${W - R + 8}" y="${Math.min(H - B, labelY) + 3.5}">${escapeHtml(name.length > 10 ? `${name.slice(0, 9)}…` : name)} <tspan style="fill:var(--mk-fg)">${escapeHtml(fmtCompact(last.value))}</tspan></text>`;
    }
    svg += `<line data-marketing-crosshair x1="0" x2="0" y1="${T}" y2="${H - B}" style="stroke:var(--mk-muted)" stroke-dasharray="3 3" opacity="0"/>`
      + `<rect data-marketing-chart-hit x="${L}" y="${T}" width="${W - L - R}" height="${H - T - B}" fill="transparent"/>`;
    host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${escapeHtml(`${METRIC_LABELS[metric]} by channel over the last ${state.view.rangeDays} days`)}">${svg}</svg><div data-marketing-chart-tip class="mk-tip" hidden></div>`;
    legend.innerHTML = lines.map((line) => `<span><i style="background:${line.color}"></i>${escapeHtml(channelLabel(line.channel))}</span>`).join("")
      + `<span>● logged snapshot · ○ carried from before</span>`;

    const chart = host.querySelector("svg"), tip = host.querySelector("[data-marketing-chart-tip]");
    const hit = host.querySelector("[data-marketing-chart-hit]"), crosshair = host.querySelector("[data-marketing-crosshair]");
    hit.addEventListener("pointermove", (event) => {
      const box = chart.getBoundingClientRect();
      const sx = (event.clientX - box.left) * W / box.width;
      const ts = start + Math.max(0, Math.min(1, (sx - L) / (W - L - R))) * (now - start);
      crosshair.setAttribute("x1", x(ts)); crosshair.setAttribute("x2", x(ts)); crosshair.setAttribute("opacity", ".7");
      const dayStart = startOfDay(ts);
      tip.innerHTML = `<b>${escapeHtml(new Date(ts).toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" }))}</b>`
        + lines.map((line) => {
          const snapshot = snapshotAt(line.list, dayStart + DAY - 1);
          const logged = snapshot && snapshot.observedAt >= dayStart;
          return `<div><span><i style="background:${line.color}"></i>${escapeHtml(channelLabel(line.channel))}</span><span class="mk-num" style="${logged ? "" : "color:var(--mk-muted)"}">${snapshot ? escapeHtml(fmtFull(snapshot[metric])) : "—"}</span></div>`;
        }).join("")
        + `<p style="margin:4px 0 0;font-size:11px;color:var(--mk-muted)">Grey values carry the last snapshot.</p>`;
      tip.hidden = false;
      const px = x(ts) * box.width / W, width = tip.offsetWidth;
      tip.style.left = `${px + 12 + width > box.width ? px - 12 - width : px + 12}px`;
    });
    hit.addEventListener("pointerleave", () => { tip.hidden = true; crosshair.setAttribute("opacity", "0"); });
  }

  function renderFlow() {
    const content = state.marketing.content, now = Date.now();
    const counts = Object.fromEntries(STATUSES.map(([status]) => [status, 0]));
    for (const item of content) counts[item.status] = (counts[item.status] || 0) + 1;
    const max = Math.max(1, ...Object.values(counts));
    $(`[data-marketing-stages]`).innerHTML = STATUSES.map(([status, label]) => {
      const tone = status === "published" ? "background:var(--mk-good)" : status === "review" ? "background:var(--mk-warn)" : "";
      return `<button data-marketing-stage="${status}" type="button" class="mk-stage" aria-pressed="${state.view.status === status}"><span>${STATUS_ICONS[status]}${label}</span><span class="mk-track"><span class="mk-bar" style="width:${counts[status] ? Math.max(3, counts[status] / max * 100) : 0}%;${tone}"></span></span><span class="mk-num">${counts[status]}</span></button>`;
    }).join("");

    const notes = [];
    const stuck = content.filter((item) => item.status === "review" && item.updatedAt && now - item.updatedAt > REVIEW_STUCK_DAYS * DAY)
      .sort((a, b) => a.updatedAt - b.updatedAt);
    if (stuck.length) {
      const oldest = stuck[0];
      notes.push(`<p class="mk-warn">${WARN_ICON}<span><b>${stuck.length} in review for ${REVIEW_STUCK_DAYS}+ days.</b> <span style="color:var(--mk-muted)">Oldest: <button data-marketing-content-edit="${escapeHtml(oldest.id)}" type="button" class="mk-link" style="padding:0">“${escapeHtml(oldest.title)}”</button>${oldest.owner ? ` (${escapeHtml(oldest.owner)})` : ""}</span></span></p>`);
    }
    const overdue = content.filter((item) => ["queued", "scheduled"].includes(item.status) && item.scheduledAt && item.scheduledAt < now);
    if (overdue.length) notes.push(`<p class="mk-warn">${WARN_ICON}<span><b>${overdue.length} past their scheduled time</b> and not marked published.</span></p>`);
    const cycles = content.filter((item) => item.status === "published" && item.publishedAt > item.createdAt && item.createdAt)
      .map((item) => item.publishedAt - item.createdAt).sort((a, b) => a - b);
    if (cycles.length) {
      const middle = Math.floor(cycles.length / 2);
      const median = cycles.length % 2 ? cycles[middle] : (cycles[middle - 1] + cycles[middle]) / 2;
      notes.push(`<p>${CLOCK_ICON}<span>Median created → published: <b class="mk-num">${(median / DAY).toFixed(1)} days</b></span></p>`);
    }
    if (!stuck.length && !overdue.length) notes.unshift(`<p>${CHECK_ICON}<span>Nothing stuck in review or overdue.</span></p>`);
    $(`[data-marketing-flow-notes]`).innerHTML = notes.join("");
  }

  function syncChannelControls(selected = "") {
    const configured = state.marketing.settings.channels || [];
    const active = configured.filter((channel) => channel.enabled !== false);
    const choices = active.length ? active : configured;
    const options = choices.map((channel) =>
      `<option value="${escapeHtml(channel.id)}">${escapeHtml(channel.displayName || channelLabel(channel.id))}</option>`).join("");
    for (const selector of ["content-channel", "metric-channel"]) {
      const input = $(`[data-marketing-${selector}]`); if (!input) continue;
      const previous = selected || input.value;
      input.innerHTML = options;
      if (previous && !choices.some((channel) => channel.id === previous))
        input.insertAdjacentHTML("beforeend", `<option value="${escapeHtml(previous)}">${escapeHtml(channelLabel(previous))}</option>`);
      if (previous) input.value = previous;
    }
    const filter = $(`[data-marketing-channel-filter]`);
    if (filter) {
      const previous = filter.value;
      filter.innerHTML = `<option value="">All channels</option>` + configured.map((channel) =>
        `<option value="${escapeHtml(channel.id)}">${escapeHtml(channel.displayName || channelLabel(channel.id))}</option>`).join("");
      filter.value = configured.some((channel) => channel.id === previous) ? previous : "";
    }
    const campaigns = [...new Set(state.marketing.content.map((item) => item.campaign).filter(Boolean))].sort((a, b) => a.localeCompare(b));
    const campaignFilter = $(`[data-marketing-campaign-filter]`);
    if (campaignFilter) {
      const previous = campaignFilter.value;
      campaignFilter.innerHTML = `<option value="">All campaigns</option>` + campaigns.map((campaign) =>
        `<option value="${escapeHtml(campaign)}">${escapeHtml(campaign)}</option>`).join("");
      campaignFilter.value = campaigns.includes(previous) ? previous : "";
    }
    const datalist = $(`[data-marketing-campaign-options]`);
    if (datalist) datalist.innerHTML = campaigns.map((campaign) => `<option value="${escapeHtml(campaign)}"></option>`).join("");
  }

  function renderCatalog() {
    const catalog = state.marketing.channelCatalog || [];
    const configured = new Set((state.marketing.settings.channels || []).map((channel) => channel.id));
    const search = String($(`[data-marketing-catalog-search]`)?.value || "").trim().toLowerCase();
    const categorySelect = $(`[data-marketing-catalog-category]`);
    const category = categorySelect?.value || "";
    const categories = [...new Set(catalog.map((channel) => channel.category))].sort();
    if (categorySelect) {
      categorySelect.innerHTML = `<option value="">All categories</option>` + categories.map((value) =>
        `<option value="${escapeHtml(value)}">${escapeHtml(value)}</option>`).join("");
      categorySelect.value = categories.includes(category) ? category : "";
    }
    const items = catalog.filter((channel) => (!category || channel.category === category) &&
      (!search || `${channel.name} ${channel.category} ${channel.id}`.toLowerCase().includes(search)));
    const host = $(`[data-marketing-catalog-list]`); if (!host) return;
    const card = (channel) => {
      const added = configured.has(channel.id);
      const adding = state.marketing.addingChannel === channel.id;
      const disabled = added || Boolean(state.marketing.addingChannel) || !state.marketing.canManageSettings;
      const label = added ? "Added" : adding ? "Adding…" : state.marketing.canManageSettings ? "Add" : "Admin only";
      return `<article class="mk-catalog-item"><span class="mk-glyph">${escapeHtml(initials(channel.name))}</span><div><h4>${escapeHtml(channel.name)}</h4><p><span class="mk-num">#${Number(channel.rank)}</span> by global audience</p></div><button data-marketing-channel-add="${escapeHtml(channel.id)}" type="button" ${disabled ? "disabled" : ""} class="fm-btn fm-btn-sm">${added ? CHECK_ICON : adding ? "" : PLUS_ICON}${label}</button></article>`;
    };
    // Groups keep the catalog's audience ranking inside each category and
    // order categories by their highest-ranked channel.
    const groups = new Map();
    for (const channel of [...items].sort((a, b) => a.rank - b.rank)) {
      if (!groups.has(channel.category)) groups.set(channel.category, []);
      groups.get(channel.category).push(channel);
    }
    host.innerHTML = [...groups.entries()].map(([name, channels]) =>
      `<section class="mk-catalog-group"><h3>${escapeHtml(name)} · ${channels.length}</h3><div class="mk-catalog-grid">${channels.map(card).join("")}</div></section>`).join("");
    $(`[data-marketing-catalog-empty]`)?.classList.toggle("hidden", items.length > 0);
  }

  function showPage(name) {
    state.view.page = name;
    $(`[data-marketing-dashboard-page]`)?.classList.toggle("hidden", name !== "dashboard");
    $(`[data-marketing-advisor-page]`)?.classList.toggle("hidden", name !== "advisor");
    $(`[data-marketing-catalog-page]`)?.classList.toggle("hidden", name !== "catalog");
    $(`[data-marketing-ads-page]`)?.classList.toggle("hidden", name !== "ads");
    $(`[data-marketing-growth-page]`)?.classList.toggle("hidden", name !== "growth");
    $(`[data-marketing-reddit-page]`)?.classList.toggle("hidden", name !== "reddit");
    $(`[data-marketing-app-store-page]`)?.classList.toggle("hidden", name !== "app-store");
    $(`[data-marketing-channels-page]`)?.classList.toggle("hidden", name !== "channels");
    $(`[data-marketing-website-page]`)?.classList.toggle("hidden", name !== "website");
    syncHeader();
    if (name === "catalog") renderCatalog();
    if (name === "ads") { renderAds(); renderAdPlan(); void loadAdvisor(); }
    if (name === "advisor") { renderAdvisor(); void loadAdvisor(); }
    if (name === "growth") renderGrowth();
    if (name === "reddit") renderReddit();
    if (name === "app-store") { renderAppStore(); void loadAppStore(); }
    if (name === "channels") renderChannelHealth();
    if (name === "website") { renderPerformance(); void loadPerformance(); }
  }

  // ---- Ads manager ----------------------------------------------------------
  // Ads are planned and measured here and bought in each platform's console.
  // Money arrives as whole hundredths of the ad's currency; totals are kept
  // per currency because amounts in different currencies never add up.

  const AD_STATUSES = [["draft", "Draft"], ["review", "Review"], ["scheduled", "Scheduled"],
    ["active", "Active"], ["paused", "Paused"], ["completed", "Completed"], ["archived", "Archived"]];
  const AD_OBJECTIVES = [["awareness", "Awareness"], ["traffic", "Traffic"], ["engagement", "Engagement"],
    ["video-views", "Video views"], ["leads", "Leads"], ["app-installs", "App installs"],
    ["conversions", "Conversions"], ["sales", "Sales"]];
  const AD_STATUS_ICONS = { ...STATUS_ICONS,
    active: svgIcon('<path d="M6 4v16l13-8Z"/>'),
    paused: svgIcon('<path d="M8 5v14M16 5v14"/>'),
    completed: STATUS_ICONS.published };
  const PACING = { "on-track": ["On pace", CHECK_ICON], over: ["Spending fast", WARN_ICON],
    under: ["Spending slow", WARN_ICON], overspent: ["Over budget", WARN_ICON] };
  const AD_STALE_DAYS = 7;
  const labelOf = (pairs, value) => (pairs.find(([id]) => id === value) || [, value])[1];
  function fmtMoney(cents, currency, short = false) {
    const options = { style: "currency", currency: currency || "USD" };
    if (short) Object.assign(options, { notation: "compact", maximumFractionDigits: 1 });
    try { return new Intl.NumberFormat(undefined, options).format(Number(cents || 0) / 100); }
    catch { return `${(Number(cents || 0) / 100).toLocaleString()} ${currency || ""}`.trim(); }
  }
  const toCents = (value) => Math.max(0, Math.round(Number(value || 0) * 100));
  const fromCents = (cents) => cents ? String(Number(cents) / 100) : "";
  function adById(id) { return state.marketing.ads.find((item) => item.id === id); }
  function adPlatform(channel) { return state.marketing.adPlatforms.find((item) => item.channel === channel); }
  function adCurrencyTotals() {
    const currencies = state.marketing.adsSummary.currencies || [];
    return currencies.find((item) => item.currency === state.view.adCurrency) || currencies[0] || null;
  }

  function adAttention(now) {
    const notes = [];
    for (const ad of state.marketing.ads) {
      if (ad.status === "archived") continue;
      const pacing = ad.pacing || {};
      if (pacing.state === "overspent") notes.push([ad, "Over budget", `${fmtMoney(ad.spendCents, ad.currency)} spent of ${fmtMoney(pacing.totalBudgetCents, ad.currency)}`]);
      else if (pacing.state === "over" || pacing.state === "under") notes.push([ad, PACING[pacing.state][0], `${fmtMoney(ad.spendCents, ad.currency)} spent · ${fmtMoney(pacing.expectedCents, ad.currency)} expected by now`]);
      if (ad.status === "active" && ad.endsAt && ad.endsAt < now) notes.push([ad, "Flight ended", `Ended ${relativeTimeLabel(ad.endsAt)} and still marked active`]);
      else if (ad.status === "scheduled" && ad.startsAt && ad.startsAt < now) notes.push([ad, "Start date passed", `Was due to start ${relativeTimeLabel(ad.startsAt)}`]);
      else if (ad.status === "active" && now - (ad.resultsAt || ad.createdAt) > AD_STALE_DAYS * DAY) notes.push([ad, "Results are stale", ad.resultsAt ? `Last logged ${relativeTimeLabel(ad.resultsAt)}` : "No results logged yet"]);
    }
    return notes;
  }

  function renderAds() {
    const host = $(`[data-marketing-ads-list]`); if (!host) return;
    const now = Date.now(), ads = state.marketing.ads || [];
    const currencies = state.marketing.adsSummary.currencies || [];
    const totals = adCurrencyTotals();
    const currency = totals?.currency || "USD";
    const picker = $(`[data-marketing-ads-currency]`);
    picker.hidden = currencies.length < 2;
    picker.innerHTML = currencies.map((item) => `<option value="${escapeHtml(item.currency)}">${escapeHtml(item.currency)} · ${item.ads} ad${item.ads === 1 ? "" : "s"}</option>`).join("");
    picker.value = currency;

    const set = (key, value, title = "") => { const node = $(`[data-marketing-ads-stat-${key}]`); if (node) { node.textContent = value; node.title = title; } };
    set("spend", totals ? fmtMoney(totals.spendCents, currency, true) : "—", totals ? fmtMoney(totals.spendCents, currency) : "");
    set("budget", totals?.budgetCents ? `${Math.round(totals.spendCents / totals.budgetCents * 100)}% of ${fmtMoney(totals.budgetCents, currency, true)} budget` : "No budget set");
    set("impressions", fmtCompact(totals?.impressions), fmtFull(totals?.impressions));
    set("cpm", totals?.impressions ? `${fmtMoney(totals.cpmCents, currency)} per 1,000` : "");
    set("clicks", fmtCompact(totals?.clicks), fmtFull(totals?.clicks));
    set("ctr", totals?.impressions ? `${totals.ctr.toFixed(2)}% CTR · ${fmtMoney(totals.cpcCents, currency)} per click` : "");
    set("conversions", fmtCompact(totals?.conversions), fmtFull(totals?.conversions));
    set("cpa", totals?.conversions ? `${fmtMoney(totals.cpaCents, currency)} per conversion` : "");
    set("roas", totals?.spendCents && totals.revenueCents ? `${totals.roas.toFixed(2)}×` : "—");
    set("revenue", totals?.revenueCents ? `${fmtMoney(totals.revenueCents, currency, true)} revenue` : "No revenue logged");

    const spendHost = $(`[data-marketing-ads-spend]`);
    $(`[data-marketing-ads-spend-title]`).textContent = currencies.length > 1 ? `Spend by channel · ${currency}` : "Spend by channel";
    const channels = totals?.channels || [];
    const maxSpend = Math.max(1, ...channels.map((item) => item.spendCents));
    spendHost.innerHTML = channels.length ? channels.map((item) => {
      const detail = [`${item.ads} ad${item.ads === 1 ? "" : "s"}`, item.impressions ? `${item.ctr.toFixed(2)}% CTR` : "", item.conversions ? `${fmtMoney(item.cpaCents, currency)} CPA` : ""].filter(Boolean).join(" · ");
      return `<button data-marketing-ads-channel="${escapeHtml(item.channel)}" type="button" class="mk-spend-row" aria-pressed="${state.view.adChannel === item.channel}" title="${escapeHtml(`${channelLabel(item.channel)}: ${fmtMoney(item.spendCents, currency)} spent · ${fmtFull(item.impressions)} impressions · ${fmtFull(item.clicks)} clicks · ${fmtFull(item.conversions)} conversions`)}"><span>${escapeHtml(channelLabel(item.channel))}</span><span class="mk-track"><span class="mk-bar" style="width:${item.spendCents ? Math.max(1.5, item.spendCents / maxSpend * 100) : 0}%"></span></span><small><b>${escapeHtml(fmtMoney(item.spendCents, currency))}</b> · ${escapeHtml(detail)}</small></button>`;
    }).join("") : blankslate("ads", "No ad spend yet", "No ads yet. Plan the first one for any of your channels.", `<button data-marketing-ad-open type="button" class="fm-btn fm-btn-sm">New ad</button>`);

    const attention = adAttention(now);
    $(`[data-marketing-ads-attention-count]`).textContent = attention.length ? `${attention.length} to review` : "";
    $(`[data-marketing-ads-attention]`).innerHTML = attention.length
      ? attention.slice(0, 8).map(([ad, label, detail]) => `<button data-marketing-ad-edit="${escapeHtml(ad.id)}" type="button">${WARN_ICON}<span><b>${escapeHtml(label)}</b> · ${escapeHtml(ad.name)}<small>${escapeHtml(`${channelLabel(ad.channel)} · ${detail}`)}</small></span></button>`).join("")
        + (attention.length > 8 ? `<p class="mk-hint" style="margin:6px 0 0">and ${attention.length - 8} more</p>` : "")
      : `<p class="mk-hint" style="display:flex;gap:8px;align-items:center;margin:0">${CHECK_ICON}Every active ad is on pace and recently logged.</p>`;

    const filter = $(`[data-marketing-ads-channel-filter]`);
    const used = [...new Set(ads.map((ad) => ad.channel))].sort((a, b) => channelLabel(a).localeCompare(channelLabel(b)));
    if (!used.includes(state.view.adChannel)) state.view.adChannel = "";
    filter.innerHTML = `<option value="">All channels</option>` + used.map((id) => `<option value="${escapeHtml(id)}">${escapeHtml(channelLabel(id))}</option>`).join("");
    filter.value = state.view.adChannel;
    const query = String($(`[data-marketing-ads-search]`)?.value || "").trim().toLowerCase();
    const base = ads.filter((ad) => (!state.view.adChannel || ad.channel === state.view.adChannel)
      && (!query || [ad.name, ad.campaign, ad.owner, ad.audience, ad.headline, ad.platform, ...(ad.tags || [])]
        .some((value) => String(value || "").toLowerCase().includes(query))));
    const counts = Object.fromEntries(AD_STATUSES.map(([status]) => [status, 0]));
    for (const ad of base) counts[ad.status] = (counts[ad.status] || 0) + 1;
    $(`[data-marketing-ads-status-tabs]`).innerHTML = [["", "All", base.length], ...AD_STATUSES.map(([status, label]) => [status, label, counts[status]])]
      .map(([status, label, count]) => `<button data-marketing-ads-status-tab="${status}" type="button" role="tab" class="mk-tab" aria-selected="${state.view.adStatus === status}">${label}<em>${count}</em></button>`).join("");
    const items = state.view.adStatus ? base.filter((ad) => ad.status === state.view.adStatus) : base;
    $(`[data-marketing-ads-count]`).textContent = `${fmtFull(ads.length)} ad${ads.length === 1 ? "" : "s"} · ${fmtFull(state.marketing.adsSummary.active || 0)} active`;
    $(`[data-marketing-ads-empty]`).hidden = items.length > 0;
    const dash = `<span style="color:var(--mk-muted)">—</span>`;
    host.innerHTML = items.map((ad) => {
      const pacing = ad.pacing || {}, total = pacing.totalBudgetCents || 0;
      const flight = ad.startsAt || ad.endsAt
        ? `<b class="mk-num">${ad.startsAt ? escapeHtml(shortDate(ad.startsAt)) : "…"} – ${ad.endsAt ? escapeHtml(shortDate(ad.endsAt)) : "ongoing"}</b><small>${ad.startsAt > now ? `Starts ${escapeHtml(relativeLabel(ad.startsAt))}` : ad.endsAt && ad.endsAt < now ? `Ended ${escapeHtml(relativeTimeLabel(ad.endsAt))}` : ad.endsAt ? `Ends ${escapeHtml(relativeLabel(ad.endsAt))}` : "No end date"}</small>`
        : `<b style="color:var(--mk-muted)">No dates</b>`;
      const pace = PACING[pacing.state];
      const budget = `<div class="mk-budget"><small><b class="mk-num" style="color:var(--mk-fg);font-weight:500">${escapeHtml(fmtMoney(ad.spendCents, ad.currency))}</b><span class="mk-num">${total ? `of ${escapeHtml(fmtMoney(total, ad.currency))}` : ad.budgetCents ? `${escapeHtml(fmtMoney(ad.budgetCents, ad.currency))}/day` : "no budget"}</span></small>`
        + (total ? `<span class="mk-track"><span class="mk-bar${pacing.state === "overspent" ? " over" : ""}" style="width:${Math.min(100, ad.spendCents / total * 100)}%"></span>${pacing.expectedCents && ad.status === "active" ? `<span class="mk-expected" style="left:${Math.min(100, pacing.expectedCents / total * 100)}%" title="Expected spend by now: ${escapeHtml(fmtMoney(pacing.expectedCents, ad.currency))}"></span>` : ""}</span>` : "")
        + (pace ? `<span class="mk-pace ${escapeHtml(pacing.state)}">${pace[1]}${pace[0]}</span>` : "") + `</div>`;
      const consoleUrl = ad.managerUrl || ad.platformUrl;
      return `<tr style="${ad.status === "archived" ? "opacity:.55" : ""}">
        <td><button data-marketing-ad-edit="${escapeHtml(ad.id)}" type="button" class="mk-content-title"><b>${escapeHtml(ad.name)}</b><small>${escapeHtml([labelOf(AD_OBJECTIVES, ad.objective), ad.campaign, ad.headline].filter(Boolean).join(" · "))}</small></button></td>
        <td><b style="display:block;font-weight:500;font-size:12.5px">${escapeHtml(channelLabel(ad.channel))}</b><small style="font-size:11.5px;color:var(--mk-muted)">${escapeHtml(ad.platform || "Booked directly")}</small></td>
        <td><span class="fm-label mk-pill ${escapeHtml(ad.status)}">${AD_STATUS_ICONS[ad.status] || ""}${escapeHtml(labelOf(AD_STATUSES, ad.status))}</span></td>
        <td class="mk-when">${flight}</td>
        <td>${budget}</td>
        <td class="r mk-num">${ad.impressions ? fmtCompact(ad.impressions) : dash}</td>
        <td class="r mk-num">${ad.impressions ? `${ad.ctr.toFixed(2)}%` : dash}</td>
        <td class="r mk-num">${ad.conversions ? fmtFull(ad.conversions) : dash}</td>
        <td class="r mk-num">${ad.conversions ? escapeHtml(fmtMoney(ad.cpaCents, ad.currency)) : dash}</td>
        <td class="r mk-num">${ad.spendCents && ad.revenueCents ? `${ad.roas.toFixed(2)}×` : dash}</td>
        <td class="r">${consoleUrl?.startsWith("https://") ? `<a href="${escapeHtml(consoleUrl)}" target="_blank" rel="noopener noreferrer" class="mk-link" aria-label="Open ${escapeHtml(ad.name)} in ${escapeHtml(ad.platform || "its console")}">Console ↗</a>` : ""}<button data-marketing-ad-results="${escapeHtml(ad.id)}" type="button" class="mk-link">Log results</button><button data-marketing-ad-edit="${escapeHtml(ad.id)}" type="button" class="mk-link">Edit</button></td>
      </tr>`;
    }).join("");
  }

  function describeAdPlatform() {
    const platform = adPlatform($(`[data-marketing-ad-channel]`).value);
    $(`[data-marketing-ad-platform]`).textContent = platform
      ? `Bought in ${platform.name}. Plan and measure it here.`
      : "This channel has no self-serve ad console; placements are booked directly. Plan and measure it here.";
  }
  function openAd(ad = null) {
    const configured = state.marketing.settings.channels || [];
    const ids = new Set(configured.filter((channel) => channel.enabled !== false).map((channel) => channel.id));
    const group = (label, list) => list.length ? `<optgroup label="${label}">${list.map((channel) => `<option value="${escapeHtml(channel.id)}">${escapeHtml(channelLabel(channel.id))}</option>`).join("")}</optgroup>` : "";
    // Any catalog channel takes an ad, whether or not it is on the dashboard.
    $(`[data-marketing-ad-channel]`).innerHTML = group("Your channels", configured.filter((channel) => ids.has(channel.id)))
      + group("All channels", state.marketing.channelCatalog.filter((channel) => !ids.has(channel.id)));
    $(`[data-marketing-ad-status]`).innerHTML = AD_STATUSES.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    $(`[data-marketing-ad-objective]`).innerHTML = AD_OBJECTIVES.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    $(`[data-marketing-ad-title]`).textContent = ad ? "Edit ad" : "New ad";
    $(`[data-marketing-ad-id]`).value = ad?.id || "";
    const defaults = { channel: state.view.adChannel || [...ids][0] || "youtube", status: "draft", objective: "awareness",
      "budget-type": "lifetime", currency: adCurrencyTotals()?.currency || "USD" };
    const values = { name: ad?.name, channel: ad?.channel, status: ad?.status, objective: ad?.objective,
      campaign: ad?.campaign, owner: ad?.owner, audience: ad?.audience, headline: ad?.headline, body: ad?.body,
      notes: ad?.notes, destination: ad?.destinationUrl, manager: ad?.managerUrl, "budget-type": ad?.budgetType,
      currency: ad?.currency, budget: fromCents(ad?.budgetCents), tags: (ad?.tags || []).join(", "),
      starts: localDateTime(ad?.startsAt), ends: localDateTime(ad?.endsAt) };
    for (const [field, value] of Object.entries(values)) $(`[data-marketing-ad-${field}]`).value = value || defaults[field] || "";
    $(`[data-marketing-ad-delete]`).classList.toggle("hidden", !ad);
    describeAdPlatform();
    $(`[data-marketing-ad-dialog]`).showModal();
  }
  async function saveAd(event) {
    event.preventDefault(); const id = $(`[data-marketing-ad-id]`).value;
    const field = (name) => $(`[data-marketing-ad-${name}]`).value;
    const body = { name: field("name"), channel: field("channel"), status: field("status"), objective: field("objective"),
      campaign: field("campaign"), owner: field("owner"), audience: field("audience"), headline: field("headline"),
      body: field("body"), notes: field("notes"), destinationUrl: field("destination"), managerUrl: field("manager"),
      budgetType: field("budget-type"), currency: (field("currency") || "USD").toUpperCase(), budgetCents: toCents(field("budget")),
      tags: field("tags").split(",").map((tag) => tag.trim()).filter(Boolean),
      startsAt: field("starts") ? new Date(field("starts")).getTime() : 0,
      endsAt: field("ends") ? new Date(field("ends")).getTime() : 0 };
    try { await request(orgPath(`/ads${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-marketing-ad-dialog]`).close(); message("notice", "Ad saved."); await load(); }
    catch (error) { message("error", `Could not save the ad: ${error.message}`); }
  }
  async function deleteAd() {
    const id = $(`[data-marketing-ad-id]`).value; if (!id || !confirm("Delete this ad and its logged results?")) return;
    try { await request(orgPath(`/ads/${id}`), "DELETE", {}); $(`[data-marketing-ad-dialog]`).close(); message("notice", "Ad deleted."); await load(); }
    catch (error) { message("error", `Could not delete the ad: ${error.message}`); }
  }
  const AD_RESULT_FIELDS = [["spend", "spendCents", true], ["revenue", "revenueCents", true],
    ["impressions", "impressions", false], ["clicks", "clicks", false], ["conversions", "conversions", false]];
  function openAdResults(ad) {
    if (!ad) return;
    $(`[data-marketing-ad-results-id]`).value = ad.id;
    $(`[data-marketing-ad-results-hint]`).textContent = `${ad.name} · ${channelLabel(ad.channel)} · totals to date in ${ad.currency}, as ${ad.platform || "the channel"} reports them. ${ad.resultsAt ? `Last logged ${relativeTimeLabel(ad.resultsAt)}.` : "Nothing logged yet."}`;
    for (const [field, key, money] of AD_RESULT_FIELDS)
      $(`[data-marketing-ad-results-${field}]`).value = money ? fromCents(ad[key]) : ad[key] || "";
    $(`[data-marketing-ad-results-dialog]`).showModal();
  }
  async function saveAdResults(event) {
    event.preventDefault(); const id = $(`[data-marketing-ad-results-id]`).value; const body = {};
    for (const [field, key, money] of AD_RESULT_FIELDS) {
      const value = $(`[data-marketing-ad-results-${field}]`).value;
      body[key] = money ? toCents(value) : Math.max(0, Math.round(Number(value || 0)));
    }
    try { await request(orgPath(`/ads/${id}`), "PATCH", body);
      $(`[data-marketing-ad-results-dialog]`).close(); message("notice", "Ad results saved."); await load(); }
    catch (error) { message("error", `Could not save ad results: ${error.message}`); }
  }

  const EXPERIMENT_STATUSES = [["idea", "Idea"], ["planned", "Planned"], ["running", "Running"],
    ["review", "In review"], ["done", "Done"], ["dropped", "Dropped"]];
  const EXPERIMENT_STAGES = [["acquisition", "Acquisition"], ["activation", "Activation"],
    ["retention", "Retention"], ["referral", "Referral"], ["revenue", "Revenue"], ["measurement", "Measurement"]];
  const EXPERIMENT_DECISIONS = [["", "Not decided"], ["keep", "Keep"], ["iterate", "Iterate"],
    ["revert", "Revert"], ["extend", "Extend the window"]];
  const EXPERIMENT_ORDER = { running: 0, review: 1, planned: 2, idea: 3, done: 4, dropped: 5 };
  const RATE_KEYS = ["signupRate", "activationRate", "retentionRate", "referralRate"];
  const missing = (value) => value === null || value === undefined;
  const fmtCount = (value) => missing(value) ? "—" : fmtFull(value);
  const fmtRate = (value) => missing(value) ? "—" : `${Number(value).toLocaleString(undefined, { maximumFractionDigits: 1 })}%`;
  const weekLabel = (ts) => new Date(ts).toLocaleDateString(undefined, { timeZone: "UTC", month: "short", day: "numeric", year: "numeric" });
  const utcDate = (ts) => new Date(ts).toISOString().slice(0, 10);
  function experimentById(id) { return state.marketing.experiments.find((item) => item.id === id); }
  function funnelById(id) { return state.marketing.funnel.find((item) => item.id === id); }

  function growthDelta(key, change, unit) {
    const node = $(`[data-marketing-growth-delta="${key}"]`); if (!node) return;
    node.className = `mk-delta${change > 0 ? " up" : change < 0 ? " down" : ""}`;
    node.textContent = missing(change) ? "—"
      : `${change > 0 ? "▲ " : change < 0 ? "▼ " : ""}${Math.abs(change).toLocaleString(undefined, { maximumFractionDigits: 2 })}${unit}`;
  }

  function renderGrowth() {
    const report = state.marketing.funnelReport || {};
    const rates = Object.fromEntries((report.rates || []).map((item) => [item.key, item]));
    const counts = Object.fromEntries((report.counts || []).map((item) => [item.key, item]));
    const stat = (key, value, title = "") => { const node = $(`[data-marketing-growth-stat="${key}"]`); if (node) { node.textContent = value; node.title = title; } };
    stat("northStar", fmtCount(counts.northStar?.value));
    growthDelta("northStar", counts.northStar?.change, "");
    const week = $(`[data-marketing-growth-week]`);
    if (week) week.textContent = report.weekStart ? `week of ${weekLabel(report.weekStart)}` : "no week logged";
    for (const key of RATE_KEYS) {
      const item = rates[key] || {};
      stat(key, fmtRate(item.value), missing(item.denominator) ? "" : `${fmtCount(item.numerator)} of ${fmtCount(item.denominator)}`);
      growthDelta(key, item.change, " pt");
      const note = $(`[data-marketing-growth-note="${key}"]`);
      if (note) {
        note.textContent = item.small && !missing(item.denominator) ? `base ${fmtCount(item.denominator)}, too small` : item.target ? `target ${fmtRate(item.target)}` : "";
        note.className = item.small ? "mk-warn" : "";
      }
    }

    const stages = [["visits", "Visits", ""], ["signups", "Signups", "signupRate"], ["activated", "Activated", "activationRate"],
      ["referrals", "Referral signups", "referralRate"], ["retained", "Retained", "retentionRate"]];
    const top = Math.max(1, ...stages.map(([key]) => Number(counts[key]?.value || 0)));
    $(`[data-marketing-funnel]`).innerHTML = report.weekStart ? stages.map(([key, label, rateKey]) => {
      const value = counts[key]?.value, rate = rateKey ? rates[rateKey] : null;
      const width = value ? Math.max(1.5, value / top * 100) : 0;
      return `<div class="mk-stage${rateKey && report.bottleneck === rateKey ? " tight" : ""}"><span>${escapeHtml(label)}</span><span class="mk-track"><span class="mk-bar" style="width:${width}%"></span></span><span class="mk-num">${escapeHtml(fmtCount(value))}${rate && !missing(rate.value) ? ` <small>${escapeHtml(fmtRate(rate.value))}</small>` : ""}</span></div>`;
    }).join("") : blankslate("funnel", "No funnel week logged yet", "Copy a week's numbers from your analytics.", `<button data-marketing-funnel-open type="button" class="fm-btn fm-btn-sm">Log week</button>`);
    $(`[data-marketing-bottleneck]`).innerHTML = `${report.bottleneck ? WARN_ICON : ""}<span>${escapeHtml(report.bottleneckReason || "")}</span>`;
    $(`[data-marketing-funnel-source]`).textContent = report.source ? `Source: ${report.source}` : "";

    const attribution = state.marketing.settings.attribution || {};
    for (const field of ["sources", "mediums", "campaigns"]) {
      const list = $(`[data-marketing-utm-${field}]`);
      if (list) list.innerHTML = (attribution[field] || []).map((value) => `<option value="${escapeHtml(value)}"></option>`).join("");
    }
    const landing = $(`[data-marketing-link-url]`);
    if (landing) landing.placeholder = attribution.landingUrl || "https://…";

    const experiments = state.marketing.experiments || [];
    const summary = state.marketing.experimentsSummary || {};
    const topIds = new Set(summary.top || []);
    const notes = [];
    if (summary.overLimit) notes.push(`${summary.running} experiments are running. Run at most ${summary.runningLimit} at a time so each result can be read.`);
    for (const id of summary.reviewsDue || []) {
      const item = experimentById(id);
      if (item) notes.push(`Review due: ${item.title}${item.ended ? " (its window has ended)" : ""}.`);
    }
    if (summary.unscored) notes.push(`${summary.unscored} open experiment${summary.unscored === 1 ? " has" : "s have"} no score.`);
    const notesHost = $(`[data-marketing-experiments-notes]`);
    notesHost.innerHTML = notes.map((text) => `<p>${WARN_ICON}<span>${escapeHtml(text)}</span></p>`).join("");
    notesHost.hidden = !notes.length;
    const openItems = experiments.filter((item) => !["done", "dropped"].includes(item.status));
    const statusCounts = Object.fromEntries(EXPERIMENT_STATUSES.map(([status]) => [status, 0]));
    for (const item of experiments) statusCounts[item.status] = (statusCounts[item.status] || 0) + 1;
    $(`[data-marketing-experiments-tabs]`).innerHTML = [["", "Open", openItems.length], ...EXPERIMENT_STATUSES.map(([status, label]) => [status, label, statusCounts[status]])]
      .map(([status, label, count]) => `<button data-marketing-experiments-tab="${status}" type="button" role="tab" class="mk-tab" aria-selected="${state.view.experimentStatus === status}">${label}<em>${count}</em></button>`).join("");
    const shown = (state.view.experimentStatus ? experiments.filter((item) => item.status === state.view.experimentStatus) : openItems)
      .slice().sort((a, b) => (EXPERIMENT_ORDER[a.status] - EXPERIMENT_ORDER[b.status]) || (b.score - a.score) || (b.updatedAt - a.updatedAt));
    $(`[data-marketing-experiments-count]`).textContent = `${fmtFull(experiments.length)} experiment${experiments.length === 1 ? "" : "s"} · ${fmtFull(summary.running || 0)} of ${summary.runningLimit || 3} running`;
    $(`[data-marketing-experiments-empty]`).hidden = shown.length > 0;
    const dash = `<span style="color:var(--mk-muted)">—</span>`;
    $(`[data-marketing-experiments-list]`).innerHTML = shown.map((item) => {
      const dates = [item.startsAt ? `${shortDate(item.startsAt)} – ${item.endsAt ? shortDate(item.endsAt) : "open"}` : "", item.reviewAt ? `review ${shortDate(item.reviewAt)}` : ""].filter(Boolean);
      return `<tr style="${["done", "dropped"].includes(item.status) ? "opacity:.6" : ""}">
        <td><button data-marketing-experiment-edit="${escapeHtml(item.id)}" type="button" class="mk-content-title"><b>${escapeHtml(item.title)}</b><small>${escapeHtml([item.metric, item.owner, item.campaign].filter(Boolean).join(" · "))}</small></button></td>
        <td>${escapeHtml(labelOf(EXPERIMENT_STAGES, item.stage))}</td>
        <td><span class="fm-label mk-pill ${escapeHtml(item.status)}">${escapeHtml(labelOf(EXPERIMENT_STATUSES, item.status))}</span>${item.decision ? `<small style="display:block;font-size:11.5px;color:var(--mk-muted)">${escapeHtml(labelOf(EXPERIMENT_DECISIONS, item.decision))}</small>` : ""}</td>
        <td class="r mk-num">${item.impact || dash}</td><td class="r mk-num">${item.confidence || dash}</td><td class="r mk-num">${item.ease || dash}</td>
        <td class="r"><span class="mk-score${topIds.has(item.id) ? " top" : ""}"${topIds.has(item.id) ? ' title="Top three of the backlog"' : ""}>${item.score || "—"}</span></td>
        <td class="mk-when">${dates.length ? `<small>${escapeHtml(dates.join(" · "))}</small>` : dash}</td>
        <td class="r"><button data-marketing-experiment-edit="${escapeHtml(item.id)}" type="button" class="mk-link">Edit</button></td>
      </tr>`;
    }).join("");

    const weeks = state.marketing.funnel || [];
    $(`[data-marketing-funnel-empty]`).hidden = weeks.length > 0;
    $(`[data-marketing-funnel-list]`).innerHTML = weeks.map((item) => `<tr>
      <td class="mk-num">${escapeHtml(weekLabel(item.weekStart))}</td>
      ${["northStar", "visits", "signups", "activated"].map((key) => `<td class="r mk-num">${escapeHtml(fmtCount(item[key]))}</td>`).join("")}
      <td class="r mk-num">${missing(item.retained) ? "—" : escapeHtml(`${fmtFull(item.retained)}${missing(item.retainedBase) ? "" : ` / ${fmtFull(item.retainedBase)}`}`)}</td>
      <td class="r mk-num">${escapeHtml(fmtCount(item.referrals))}</td>
      <td class="r mk-num">${missing(item.revenueCents) ? "—" : escapeHtml(fmtMoney(item.revenueCents, item.currency))}</td>
      <td><small style="color:var(--mk-muted)">${escapeHtml(item.source || "")}</small></td>
      <td class="r"><button data-marketing-funnel-edit="${escapeHtml(item.id)}" type="button" class="mk-link">Edit</button><button data-marketing-funnel-delete="${escapeHtml(item.id)}" type="button" class="mk-link">Delete</button></td>
    </tr>`).join("");
  }

  function openExperiment(item = null) {
    $(`[data-marketing-experiment-stage]`).innerHTML = EXPERIMENT_STAGES.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    $(`[data-marketing-experiment-status]`).innerHTML = EXPERIMENT_STATUSES.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    $(`[data-marketing-experiment-decision]`).innerHTML = EXPERIMENT_DECISIONS.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    $(`[data-marketing-experiment-title]`).textContent = item ? "Edit experiment" : "New experiment";
    $(`[data-marketing-experiment-id]`).value = item?.id || "";
    const values = { name: item?.title, stage: item?.stage || "acquisition", status: item?.status || "idea",
      impact: item?.impact, confidence: item?.confidence, ease: item?.ease, hypothesis: item?.hypothesis,
      audience: item?.audience, metric: item?.metric, baseline: item?.baseline, target: item?.target,
      owner: item?.owner, campaign: item?.campaign, decision: item?.decision, result: item?.result, notes: item?.notes,
      starts: localDateTime(item?.startsAt), ends: localDateTime(item?.endsAt), review: localDateTime(item?.reviewAt) };
    for (const [field, value] of Object.entries(values)) $(`[data-marketing-experiment-${field}]`).value = value || "";
    $(`[data-marketing-experiment-delete]`).classList.toggle("hidden", !item);
    $(`[data-marketing-experiment-dialog]`).showModal();
  }
  async function saveExperiment(event) {
    event.preventDefault(); const id = $(`[data-marketing-experiment-id]`).value;
    const field = (name) => $(`[data-marketing-experiment-${name}]`).value;
    const when = (name) => field(name) ? new Date(field(name)).getTime() : 0;
    const body = { title: field("name"), startsAt: when("starts"), endsAt: when("ends"), reviewAt: when("review") };
    for (const name of ["stage", "status", "hypothesis", "audience", "metric", "baseline", "target", "owner", "campaign", "decision", "result", "notes"]) body[name] = field(name);
    for (const name of ["impact", "confidence", "ease"]) body[name] = Number(field(name) || 0);
    try { await request(orgPath(`/experiments${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-marketing-experiment-dialog]`).close(); message("notice", "Experiment saved."); await load(); }
    catch (error) { message("error", `Could not save the experiment: ${error.message}`); }
  }
  async function deleteExperiment() {
    const id = $(`[data-marketing-experiment-id]`).value; if (!id || !confirm("Delete this experiment?")) return;
    try { await request(orgPath(`/experiments/${id}`), "DELETE", {}); $(`[data-marketing-experiment-dialog]`).close(); message("notice", "Experiment deleted."); await load(); }
    catch (error) { message("error", `Could not delete the experiment: ${error.message}`); }
  }

  function weekStartOf(ts) {
    const date = new Date(ts); date.setUTCHours(0, 0, 0, 0);
    return date.getTime() - ((date.getUTCDay() + 6) % 7) * DAY;
  }
  function funnelForWeek(ts) {
    const start = weekStartOf(ts);
    return state.marketing.funnel.find((item) => item.weekStart === start) || null;
  }
  function fillFunnel(week) {
    $$(`[data-marketing-funnel-field]`).forEach((input) => { const value = week?.[input.dataset.marketingFunnelField]; input.value = missing(value) ? "" : String(value); });
    $(`[data-marketing-funnel-revenue]`).value = missing(week?.revenueCents) ? "" : String(week.revenueCents / 100);
    $(`[data-marketing-funnel-currency]`).value = week?.currency || "USD";
    $(`[data-marketing-funnel-source-input]`).value = week?.source || "";
    $(`[data-marketing-funnel-note]`).value = week?.note || "";
  }
  function openFunnel(week = null) {
    const start = week?.weekStart || weekStartOf(Date.now());
    $(`[data-marketing-funnel-week]`).value = utcDate(start);
    fillFunnel(week || funnelForWeek(start));
    $(`[data-marketing-funnel-dialog]`).showModal();
  }
  async function saveFunnel(event) {
    event.preventDefault();
    const day = $(`[data-marketing-funnel-week]`).value;
    const body = { weekStart: Date.parse(`${day}T00:00:00Z`), currency: $(`[data-marketing-funnel-currency]`).value,
      source: $(`[data-marketing-funnel-source-input]`).value, note: $(`[data-marketing-funnel-note]`).value };
    $$(`[data-marketing-funnel-field]`).forEach((input) => { body[input.dataset.marketingFunnelField] = input.value === "" ? null : Number(input.value); });
    const revenue = $(`[data-marketing-funnel-revenue]`).value;
    body.revenueCents = revenue === "" ? null : toCents(revenue);
    try { await request(orgPath("/funnel"), "POST", body); $(`[data-marketing-funnel-dialog]`).close(); message("notice", "Funnel week saved."); await load(); }
    catch (error) { message("error", `Could not save the funnel week: ${error.message}`); }
  }
  async function deleteFunnel(id) {
    const week = funnelById(id); if (!week || !confirm(`Delete the funnel numbers for the week of ${weekLabel(week.weekStart)}?`)) return;
    try { await request(orgPath(`/funnel/${id}`), "DELETE", {}); message("notice", "Funnel week deleted."); await load(); }
    catch (error) { message("error", `Could not delete the funnel week: ${error.message}`); }
  }

  async function buildLink(event) {
    event.preventDefault();
    const body = {};
    for (const name of ["url", "source", "medium", "campaign", "content"]) body[name] = $(`[data-marketing-link-${name}]`).value;
    try { const payload = await request(orgPath("/links"), "POST", body);
      $(`[data-marketing-link-output]`).textContent = payload.url || "";
      const checks = payload.checks || [];
      $(`[data-marketing-link-checks]`).innerHTML = checks.length
        ? checks.map((check) => `<li class="${escapeHtml(check.level)}">${WARN_ICON}<span>${escapeHtml(check.message)}</span></li>`).join("")
        : `<li class="ok">${CHECK_ICON}<span>Tagged, on the allowlists and landing on a tracked page.</span></li>`;
      $(`[data-marketing-link-result]`).hidden = false; message("error", ""); }
    catch (error) { message("error", `Could not build the link: ${error.message}`); }
  }
  function copyText(text, done) {
    navigator.clipboard?.writeText(text).then(() => message("notice", done), () => message("error", "Copy failed; select the text and copy it."));
  }
  async function openReport() {
    try { const payload = await request(orgPath("/report"));
      $(`[data-marketing-report-text]`).value = payload.markdown || "";
      $(`[data-marketing-report-dialog]`).showModal(); }
    catch (error) { message("error", `Could not build the report: ${error.message}`); }
  }
  function downloadReport() {
    const url = URL.createObjectURL(new Blob([$(`[data-marketing-report-text]`).value], { type: "text/markdown" }));
    const link = document.createElement("a"); link.href = url;
    link.download = `marketing-report-${state.marketing.organization}-${utcDate(Date.now())}.md`; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  const PERF_ICONS = { setup: svgIcon('<path d="M12 5v14M5 12h14"/>'), action: svgIcon('<path d="m9 18 6-6-6-6"/>'),
    warning: WARN_ICON, win: CHECK_ICON, info: svgIcon('<circle cx="12" cy="12" r="9"/><path d="M12 16v-4M12 8h.01"/>') };
  const fmtShare = (value) => missing(value) ? "—" : fmtRate(Number(value) * 100);
  function perfDelta(key, current, previous, lowerIsBetter = false) {
    const node = $(`[data-marketing-performance-delta="${key}"]`); if (!node) return;
    if (missing(current) || missing(previous)) { node.className = "mk-delta"; node.textContent = "—"; return; }
    if (!previous) { node.className = "mk-delta"; node.textContent = current ? "New" : "—"; return; }
    const change = (current - previous) / previous * 100;
    const good = lowerIsBetter ? change < -0.05 : change > 0.05, bad = lowerIsBetter ? change > 0.05 : change < -0.05;
    node.className = `mk-delta${good ? " up" : bad ? " down" : ""}`;
    node.textContent = `${change > 0.05 ? "▲" : change < -0.05 ? "▼" : "•"} ${Math.abs(change).toFixed(1)}%`;
  }
  function insightItems(list) {
    return (list || []).map((item) => `<li data-level="${escapeHtml(item.level)}">${PERF_ICONS[item.level] || PERF_ICONS.info}<span>${escapeHtml(item.title)}${item.detail ? `<small>${escapeHtml(item.detail)}</small>` : ""}</span></li>`).join("");
  }
  function campaignLabel(row) { return row.campaign && row.campaign !== "(none)" ? row.campaign : "No campaign"; }
  function spendPerVisit(row) {
    return (row.spend || []).map((item) => missing(item.perSessionCents) ? fmtMoney(item.cents, item.currency) : fmtMoney(item.perSessionCents, item.currency)).join(" · ") || "—";
  }
  async function loadPerformance() {
    if (!state.marketing.installed) return;
    const to = Date.now(), from = to - state.view.performanceDays * DAY;
    const params = new URLSearchParams({ from: String(from), to: String(to) });
    if (state.view.performanceProject) params.set("project", state.view.performanceProject);
    try { const payload = await request(`${orgPath("/performance")}?${params}`);
      state.marketing.performance = payload.performance || null; renderPerformance(); }
    catch (error) { message("error", `Could not read campaign results: ${error.message}`); }
  }
  function renderWebsiteVisits(days, from, to) {
    const host = $(`[data-marketing-website-visits-chart]`); if (!host) return;
    const values = new Map((days || []).map(([day, count]) => [Number(day), Number(count)]));
    const start = startOfDay(from), end = Math.max(start + DAY, startOfDay(to));
    const points = Array.from({ length: Math.max(2, Math.ceil((end - start) / DAY) + 1) }, (_, index) => {
      const day = start + index * DAY; return [day, values.get(day) || 0];
    });
    if (!values.size) { host.innerHTML = blankslate("chart", "No visits yet", "Visits appear here when Analytics records website traffic."); return; }
    const W = 720, H = 230, L = 42, R = 12, T = 10, B = 26;
    const maximum = Math.max(...points.map(([, value]) => value));
    const step = niceStep(maximum);
    const top = Math.max(step, Math.ceil(maximum / step) * step);
    const x = (day) => L + (day - start) / (end - start) * (W - L - R);
    const y = (value) => T + (1 - value / top) * (H - T - B);
    let svg = "";
    for (let value = 0; value <= top; value += step) {
      svg += `<line x1="${L}" x2="${W - R}" y1="${y(value)}" y2="${y(value)}" style="stroke:var(--mk-line-soft)"/><text x="${L - 7}" y="${y(value) + 3}" text-anchor="end">${fmtCompact(value)}</text>`;
    }
    for (let index = 0; index < 4; index += 1) {
      const day = start + (end - start) * index / 3;
      svg += `<text x="${x(day)}" y="${H - 7}" text-anchor="${index === 0 ? "start" : index === 3 ? "end" : "middle"}">${escapeHtml(shortDate(day))}</text>`;
    }
    svg += `<path d="${points.map(([day, value], index) => `${index ? "L" : "M"}${x(day).toFixed(1)},${y(value).toFixed(1)}`).join("")}" fill="none" style="stroke:var(--mk-s1)" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
    host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Visits over time">${svg}</svg>`;
  }
  function renderPerformance() {
    $$(`[data-marketing-performance-range]`).forEach((button) => button.setAttribute("aria-pressed", String(Number(button.dataset.marketingPerformanceRange) === state.view.performanceDays)));
    const perf = state.marketing.performance; if (!perf) return;
    const ready = perf.status === "ok";
    $(`[data-marketing-performance-setup]`).hidden = ready;
    $(`[data-marketing-performance-body]`).hidden = !ready;
    const projects = perf.projects || [], picker = $(`[data-marketing-performance-project]`);
    picker.innerHTML = projects.map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(item.name)}${item.siteDefault ? " · site" : ""}</option>`).join("");
    picker.hidden = state.view.page !== "website" || projects.length < 2;
    if (perf.project) picker.value = perf.project.id;
    if (!ready) { $(`[data-marketing-performance-setup-list]`).innerHTML = insightItems(perf.insights); return; }
    const totals = perf.totals || {}, previous = perf.previous || {}, goals = (perf.goals || []).length;
    const stat = (key, value) => { const node = $(`[data-marketing-performance-stat="${key}"]`); if (node) node.textContent = value; };
    stat("sessions", fmtFull(totals.sessions)); perfDelta("sessions", totals.sessions, previous.sessions);
    stat("visitors", fmtFull(totals.visitors)); perfDelta("visitors", totals.visitors, previous.visitors);
    stat("conversions", goals ? fmtFull(totals.conversions) : "—"); perfDelta("conversions", goals ? totals.conversions : null, previous.conversions);
    $(`[data-marketing-performance-note="conversions"]`).textContent = goals ? `${fmtShare(totals.conversionRate)} of visits` : "No goal set";
    stat("bounceRate", fmtShare(totals.bounceRate)); perfDelta("bounceRate", totals.bounceRate, previous.bounceRate, true);
    stat("taggedShare", fmtShare(totals.taggedShare));
    $(`[data-marketing-performance-note="taggedShare"]`).textContent = `${fmtFull(totals.taggedSessions)} carry a campaign`;
    renderWebsiteVisits(perf.visitsDays, perf.from, perf.to);
    const terms = perf.keywords || [];
    $(`[data-marketing-website-search-terms]`).innerHTML = terms.map((row) => `<tr><td title="${escapeHtml((row.topSources || []).map((item) => item.value).join(", "))}">${escapeHtml(row.term)}</td><td class="r mk-num">${fmtFull(row.sessions)}</td><td class="r mk-num">${fmtShare(row.bounceRate)}</td></tr>`).join("");
    $(`[data-marketing-website-search-terms-empty]`).hidden = terms.length > 0;
    const campaigns = perf.campaigns || [];
    $(`[data-marketing-performance-campaigns-count]`).textContent = campaigns.length ? `${campaigns.length} sources` : "";
    $(`[data-marketing-performance-campaigns]`).innerHTML = campaigns.map((row) => {
      const change = row.sessions - row.previousSessions;
      return `<tr><td title="${escapeHtml(row.key)}">${escapeHtml(campaignLabel(row))}${row.listed ? "" : ` <span class="mk-warn mk-inline-icon" title="Not on the campaign allowlist">${WARN_ICON}</span>`}</td><td>${escapeHtml(`${row.source} / ${row.medium}`)}</td><td class="r mk-num">${fmtFull(row.sessions)}</td><td class="r mk-num">${row.previousSessions ? `${change > 0 ? "+" : ""}${fmtFull(change)}` : "new"}</td><td class="r mk-num">${fmtShare(row.bounceRate)}</td><td class="r mk-num">${goals ? fmtFull(row.conversions) : "—"}</td><td class="r mk-num">${goals ? fmtShare(row.conversionRate) : "—"}</td><td class="r mk-num">${row.ads ? escapeHtml(spendPerVisit(row)) : "—"}</td><td>${escapeHtml((row.topLanding || [])[0]?.value || "")}</td></tr>`;
    }).join("");
    $(`[data-marketing-performance-campaigns-empty]`).hidden = campaigns.length > 0;
    const insights = perf.insights || [];
    $(`[data-marketing-performance-insights-count]`).textContent = insights.length ? String(insights.length) : "";
    $(`[data-marketing-performance-insights]`).innerHTML = insights.length ? insightItems(insights)
      : `<li data-level="win">${CHECK_ICON}<span>Nothing stands out in this window.</span></li>`;
    const ads = perf.ads || [];
    $(`[data-marketing-performance-ads]`).innerHTML = ads.map((ad) => {
      const money = (cents) => missing(cents) ? "—" : fmtMoney(cents, ad.currency);
      return `<tr><td>${escapeHtml(ad.name)}${ad.tagged ? "" : ` <span class="mk-warn mk-inline-icon" title="The landing link has no campaign tags">${WARN_ICON}</span>`}</td><td>${escapeHtml(channelLabel(ad.channel))}</td><td class="r mk-num">${fmtFull(ad.clicks)}</td><td class="r mk-num">${fmtCount(ad.sessions)}</td><td class="r mk-num">${fmtShare(ad.clickToVisit)}</td><td class="r mk-num">${fmtCount(ad.conversions)}</td><td class="r mk-num">${money(ad.costPerSessionCents)}</td><td class="r mk-num">${money(ad.costPerConversionCents)}</td></tr>`;
    }).join("");
    $(`[data-marketing-performance-ads-empty]`).hidden = ads.length > 0;
    $(`[data-marketing-performance-landing]`).innerHTML = (perf.landing || []).slice(0, 12).map((row) => `<tr><td>${escapeHtml(row.landing)}</td><td class="r mk-num">${fmtFull(row.sessions)}</td><td class="r mk-num">${fmtShare(row.bounceRate)}</td><td class="r mk-num">${goals ? fmtFull(row.conversions) : "—"}</td></tr>`).join("");
    const reddit = perf.reddit || [];
    $(`[data-marketing-performance-reddit-panel]`).hidden = !reddit.length;
    $(`[data-marketing-performance-reddit]`).innerHTML = reddit.map((row) => `<tr><td>r/${escapeHtml(row.subreddit)}</td><td class="r mk-num">${fmtFull(row.threads)}</td><td class="r mk-num">${fmtFull(row.replied)}</td><td class="r mk-num">${fmtFull(row.sessions)}</td><td class="r mk-num">${row.sessions ? fmtShare(row.bounceRate) : "—"}</td><td class="r mk-num">${goals ? fmtCount(row.conversions) : "—"}</td></tr>`).join("");
    $(`[data-marketing-performance-source]`).textContent = `From the Analytics project “${perf.project?.name || ""}”, ${shortDate(perf.from)} to ${shortDate(perf.to)}${perf.lastEventAt ? `; last visit recorded ${relativeLabel(perf.lastEventAt)}` : ""}${perf.truncated ? "; this window holds more events than one read covers, so totals are a sample" : ""}. Only visitors who allow analytics are counted. Cost per visit uses each ad's logged lifetime spend, so pick a window that covers the flight.`;
  }

  const GOAL_PRESETS = { pageview: { label: "Signup page", event: "$pageview", property: "$pathname", operator: "icontains", value: "/signup" },
    action: { label: "Install click", event: "forkmesh_product_action", property: "action", operator: "icontains", value: "install" },
    event: { label: "", event: "", property: "", operator: "exact", value: "" } };
  function goalRow(goal) {
    const disabled = state.marketing.canManageSettings ? "" : "disabled";
    const field = (name, label, value, placeholder, size = "") => `<label class="grid gap-1 text-xs text-muted-foreground ${size}">${label}<input data-marketing-goal-field="${name}" maxlength="200" value="${escapeHtml(value || "")}" placeholder="${escapeHtml(placeholder)}" ${disabled} class="rounded border border-border bg-background px-2 py-1.5 text-foreground" /></label>`;
    return `<div data-marketing-goal class="mk-goal-row rounded-md border border-border bg-secondary p-3">${field("label", "Name", goal.label, "Signup")}${field("event", "Event", goal.event, "$pageview")}${field("property", "Property (optional)", goal.property, "$pathname")}<label class="grid gap-1 text-xs text-muted-foreground">Match<select data-marketing-goal-field="operator" ${disabled} class="rounded border border-border bg-background px-2 py-1.5 text-foreground"><option value="exact" ${goal.operator === "exact" ? "selected" : ""}>is</option><option value="icontains" ${goal.operator === "icontains" ? "selected" : ""}>contains</option></select></label>${field("value", "Value", goal.value, "/signup")}<button data-marketing-goal-remove type="button" aria-label="Remove goal" class="fm-btn fm-btn-invisible fm-btn-icon fm-btn-sm" ${disabled}>${CLOSE_ICON}</button></div>`;
  }
  function openGoals() {
    const analytics = state.marketing.settings.analytics || {}, perf = state.marketing.performance || {};
    $(`[data-marketing-goals-list]`).innerHTML = (analytics.goals || []).map(goalRow).join("");
    $(`[data-marketing-goals-project]`).innerHTML = `<option value="">Site default, else the first project</option>${(perf.projects || []).map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(item.name)}</option>`).join("")}`;
    $(`[data-marketing-goals-project]`).value = analytics.project || "";
    $(`[data-marketing-goals-host]`).value = analytics.host || "";
    const canSave = state.marketing.canManageSettings;
    for (const selector of ["goals-project", "goals-host", "goals-save"]) $(`[data-marketing-${selector}]`).disabled = !canSave;
    $$(`[data-marketing-goal-add]`).forEach((button) => { button.disabled = !canSave; });
    $(`[data-marketing-goals-access]`).textContent = canSave ? "" : "Only organization owners and admins can change goals.";
    $(`[data-marketing-goals-dialog]`).showModal();
  }
  function addGoal(kind) {
    const list = $(`[data-marketing-goals-list]`);
    if (list.querySelectorAll("[data-marketing-goal]").length >= 5) return message("error", "Up to 5 goals.");
    list.insertAdjacentHTML("beforeend", goalRow(GOAL_PRESETS[kind] || GOAL_PRESETS.event));
  }
  async function saveGoals(event) {
    event.preventDefault();
    const goals = $$(`[data-marketing-goal]`).map((row) => Object.fromEntries([...row.querySelectorAll("[data-marketing-goal-field]")].map((input) => [input.dataset.marketingGoalField, input.value.trim()]))).filter((goal) => goal.event);
    const analytics = { goals, project: $(`[data-marketing-goals-project]`).value, host: $(`[data-marketing-goals-host]`).value };
    try { await request(orgPath("/settings"), "PUT", { analytics }); $(`[data-marketing-goals-dialog]`).close(); message("notice", "Goals saved."); await load(); }
    catch (error) { message("error", `Could not save goals: ${error.message}`); }
  }
  async function fillFunnelFromAnalytics() {
    const day = $(`[data-marketing-funnel-week]`).value; if (!day) return;
    const from = weekStartOf(Date.parse(`${day}T00:00:00Z`)), to = Math.min(from + 7 * DAY, Date.now());
    const note = $(`[data-marketing-funnel-analytics-note]`);
    try { const payload = await request(`${orgPath("/performance")}?${new URLSearchParams({ from: String(from), to: String(to) })}`);
      const perf = payload.performance || {};
      if (perf.status !== "ok") { note.textContent = (perf.insights || [])[0]?.title || "Analytics is not set up."; return; }
      $(`[data-marketing-funnel-field="visits"]`).value = String(perf.totals.sessions || 0);
      const source = $(`[data-marketing-funnel-source-input]`);
      if (!source.value.trim()) source.value = `Analytics: ${perf.project.name}`;
      note.textContent = `${fmtFull(perf.totals.sessions)} visits from ${fmtFull(perf.totals.visitors)} visitors${to < from + 7 * DAY ? " so far this week" : ""}.`; }
    catch (error) { note.textContent = `Could not read Analytics: ${error.message}`; }
  }

  const REDDIT_STATUSES = [["to_review", "To review"], ["drafting", "Drafting"],
    ["replied", "Replied"], ["skipped", "Skipped"]];
  const REDDIT_ORDER = { to_review: 0, drafting: 1, replied: 2, skipped: 3 };
  function redditThreadById(id) { return state.marketing.redditThreads.find((item) => item.id === id); }
  const outLink = (url, label, extra = "") => `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer" ${extra}>${label}</a>`;

  function renderReddit() {
    const desk = state.marketing.redditDesk || {}, summary = desk.summary || {};
    const threads = state.marketing.redditThreads || [];
    renderRedditRules(desk);
    renderRedditIdeas(desk);
    renderRedditWalk();
    renderRedditPosts();
    for (const key of ["loggedThisWeek", "repliedThisWeek", "skippedThisWeek", "open"]) {
      const node = $(`[data-marketing-reddit-stat="${key}"]`);
      if (node) node.textContent = threads.length ? fmtFull(summary[key] || 0) : "—";
    }
    const subreddits = $(`[data-marketing-reddit-subreddits]`), queries = $(`[data-marketing-reddit-queries]`);
    if (subreddits) subreddits.innerHTML = (desk.subreddits || []).map((item) => `<li>${outLink(item.url, `r/${escapeHtml(item.name)}`)}</li>`).join("")
      || `<li class="mk-hint">No subreddits on the watch list.</li>`;
    if (queries) queries.innerHTML = (desk.queries || []).map((item) => `<li>${outLink(item.url, escapeHtml(item.query))}</li>`).join("")
      || `<li class="mk-hint">No searches on the watch list.</li>`;
    const accounts = $(`[data-marketing-reddit-accounts]`);
    if (accounts) accounts.innerHTML = (desk.accounts || []).map((item) => `<li>${outLink(item.url, `u/${escapeHtml(item.name)}`)}</li>`).join("")
      || `<li class="mk-hint">No accounts yet. Add yours to the watch list, or press Update on your Reddit profile in the extension.</li>`;
    const open = threads.filter((item) => ["to_review", "drafting"].includes(item.status));
    const counts = Object.fromEntries(REDDIT_STATUSES.map(([status]) => [status, 0]));
    for (const item of threads) counts[item.status] = (counts[item.status] || 0) + 1;
    const monitored = threads.filter((item) => item.monitored);
    $(`[data-marketing-reddit-tabs]`).innerHTML = [["", "Open", open.length], ...REDDIT_STATUSES.map(([status, label]) => [status, label, counts[status]]), ["monitored", "Monitored", monitored.length]]
      .map(([status, label, count]) => `<button data-marketing-reddit-tab="${status}" type="button" role="tab" class="mk-tab" aria-selected="${state.view.redditStatus === status}">${label}<em>${count}</em></button>`).join("");
    const shown = (state.view.redditStatus === "monitored" ? monitored : state.view.redditStatus ? threads.filter((item) => item.status === state.view.redditStatus) : open)
      .slice().sort((a, b) => (REDDIT_ORDER[a.status] - REDDIT_ORDER[b.status]) || (b.updatedAt - a.updatedAt));
    $(`[data-marketing-reddit-count]`).textContent = `${fmtFull(threads.length)} thread${threads.length === 1 ? "" : "s"} · ${fmtFull(open.length)} open`;
    $(`[data-marketing-reddit-empty]`).hidden = shown.length > 0;
    const tick = (on, label) => `<small style="display:block;font-size:11.5px;color:var(${on ? "--mk-good" : "--mk-muted"})">${on ? "✓" : "·"} ${label}</small>`;
    $(`[data-marketing-reddit-list]`).innerHTML = shown.map((item) => {
      const checks = item.linkChecks || [];
      const read = redditBrowserDetails(item), watched = redditMonitorDetails(item);
      return `<tr style="${item.status === "skipped" ? "opacity:.6" : ""}">
        <td>${outLink(item.url, `<b>${escapeHtml(item.title || item.url)}</b><small>${escapeHtml(`r/${item.subreddit} · logged by ${item.createdBy || "a teammate"} ${relativeTimeLabel(item.createdAt)}`)}</small>`, `class="mk-content-title" aria-label="Open the thread on Reddit"`)}${read ? `<small data-marketing-reddit-read style="display:block;font-size:11.5px;color:var(--mk-muted)">${escapeHtml(read)}</small>` : ""}${watched ? `<small data-marketing-reddit-monitor style="display:block;font-size:11.5px">${escapeHtml(watched)}</small>` : ""}${item.note ? `<small style="display:block;font-size:11.5px;color:var(--mk-muted)">${escapeHtml(item.note)}</small>` : ""}</td>
        <td><span class="fm-label mk-pill ${escapeHtml(item.status)}">${escapeHtml(labelOf(REDDIT_STATUSES, item.status))}</span>${item.replyUrl ? `<small style="display:block;font-size:11.5px">${outLink(item.replyUrl, "Reply ↗", `class="mk-link" style="padding:0"`)}</small>` : ""}</td>
        <td>${tick(item.rulesChecked, "Rules read")}${tick(item.disclosed, "Disclosed")}</td>
        <td style="min-width:16rem">${item.taggedLink ? `<code class="mk-link-out">${escapeHtml(item.taggedLink)}</code>` : ""}${checks.length ? `<ul class="mk-checks">${checks.map((check) => `<li class="${escapeHtml(check.level)}">${WARN_ICON}<span>${escapeHtml(check.message)}</span></li>`).join("")}</ul>` : ""}</td>
        <td class="r">${item.taggedLink ? `<button data-marketing-reddit-copy="${escapeHtml(item.id)}" type="button" class="mk-link">Copy link</button>` : ""}<button data-marketing-reddit-edit="${escapeHtml(item.id)}" type="button" class="mk-link">Edit</button></td>
      </tr>`;
    }).join("");
  }

  function redditBrowserDetails(item) {
    const parts = [];
    if (item.author) parts.push(`u/${item.author}`);
    if (Number.isFinite(item.comments)) parts.push(`${fmtFull(item.comments)} comment${item.comments === 1 ? "" : "s"}`);
    if (Number.isFinite(item.score)) parts.push(`score ${item.score.toLocaleString()}`);
    if (item.postedAt) parts.push(`posted ${relativeTimeLabel(item.postedAt)}`);
    if (item.source === "extension") parts.push("from the browser extension");
    return parts.join(" · ");
  }

  function redditViewsChange(item) {
    const points = (item.history || []).filter((point) => Number.isFinite(point.views));
    const latest = points[points.length - 1];
    if (!latest) return "";
    const day = points.slice().reverse().find((point) => point.at <= latest.at - 24 * 3600 * 1000);
    const base = day || (points.length > 1 ? points[0] : null);
    if (!base) return "";
    const change = latest.views - base.views;
    return ` (${change >= 0 ? "+" : "−"}${fmtFull(Math.abs(change))} ${day ? "in the last day" : "since the first read"})`;
  }

  function redditMonitorDetails(item) {
    if (!item.monitored) return "";
    if (!item.checkedAt) return "Monitored · not read yet: open the thread with the ForkMesh browser extension, or press Check now in its popup";
    const parts = [];
    if (Number.isFinite(item.views)) parts.push(`${fmtFull(item.views)} view${item.views === 1 ? "" : "s"}${redditViewsChange(item)}`);
    else parts.push("views not shown to this account");
    if (item.monitorCommentUrl) {
      const comment = [];
      if (Number.isFinite(item.commentScore)) comment.push(`${item.commentScore.toLocaleString()} point${item.commentScore === 1 ? "" : "s"}`);
      if (Number.isFinite(item.commentViews)) comment.push(`${fmtFull(item.commentViews)} view${item.commentViews === 1 ? "" : "s"}`);
      parts.push(`comment: ${comment.join(", ") || "not found on the page"}`);
    }
    parts.push(`read ${relativeTimeLabel(item.checkedAt)}`);
    return `Monitored · ${parts.join(" · ")}`;
  }

  function renderRedditIdeas(desk) {
    const ideas = desk.ideas || {};
    const places = ideas.subreddits || [];
    const body = $(`[data-marketing-reddit-places]`); if (!body) return;
    const hint = $(`[data-marketing-reddit-ideas-hint]`);
    if (hint) hint.textContent = places.length ? `${fmtFull((ideas.ready || []).length)} ready · ${fmtFull((ideas.rulesMissing || []).length)} need their rules read` : "";
    body.innerHTML = places.map((item) => {
      const blockers = item.blockers.length
        ? item.blockers.map((text) => `<small style="display:block;font-size:11.5px;color:var(--mk-muted)">${escapeHtml(text)}</small>`).join("")
        : `<small style="display:block;font-size:11.5px;color:var(--mk-good)">Rules on file; nothing blocking a reply</small>`;
      const quiet = item.quietDays === null ? "never logged" : `last logged ${fmtFull(item.quietDays)}d ago`;
      return `<tr><td>${outLink(item.url, `<b>r/${escapeHtml(item.name)}</b><small>${escapeHtml(quiet)}</small>`, `class="mk-content-title" aria-label="Open r/${escapeHtml(item.name)}'s newest posts"`)}</td>
        <td>${escapeHtml(item.reason)}</td>
        <td class="r mk-num">${fmtFull(item.threads)}</td>
        <td class="r mk-num">${fmtFull(item.open)}</td>
        <td class="r mk-num">${fmtFull(item.replied)}</td>
        <td>${blockers}${item.rulesRead ? "" : ` ${outLink(item.rulesUrl, "Read the rules ↗", `class="mk-link" style="padding:0"`)}`}${item.draftPrompt && ideas.draftUrl ? `<small style="display:block;font-size:11.5px">${outLink(ideas.draftUrl + encodeURIComponent(item.draftPrompt), "Draft with ChatGPT ↗", `class="mk-link" style="padding:0" data-marketing-reddit-draft title="Opens ChatGPT with ForkMesh, our tagged links, r/${escapeHtml(item.name)}'s rules and recent posts, and how our past Reddit posts did"`)}</small>` : ""}</td></tr>`;
    }).join("");
    const empty = $(`[data-marketing-reddit-places-empty]`);
    if (empty) empty.hidden = places.length > 0;
    const topics = ideas.topics || [];
    const list = $(`[data-marketing-reddit-topics]`);
    const topicHint = $(`[data-marketing-reddit-topics-hint]`);
    if (topicHint) topicHint.textContent = topics.length ? `from the last ${fmtFull(ideas.windowDays || 90)} days of threads` : "";
    if (!list) return;
    list.innerHTML = topics.map((item) => {
      const where = item.subreddits.length ? ` · ${item.subreddits.map((name) => `r/${escapeHtml(name)}`).join(", ")}` : "";
      const counts = item.threads
        ? `${fmtFull(item.threads)} thread${item.threads === 1 ? "" : "s"} logged, ${fmtFull(item.replied)} answered${where}`
        : item.source === "watch" ? "on the watch list, nothing logged yet" : "";
      const evidence = (item.evidence || []).map((entry) => outLink(entry.url, `Link ${escapeHtml(entry.title)} ↗`, `class="mk-link" style="padding:0"`)).join(" ");
      const examples = (item.examples || []).map((entry) => `<li>${outLink(entry.url, escapeHtml(entry.title), `class="mk-link" style="padding:0"`)} <small>r/${escapeHtml(entry.subreddit)} · ${escapeHtml(labelOf(REDDIT_STATUSES, entry.status))}</small></li>`).join("");
      return `<li>${outLink(item.searchUrl, `<b>${escapeHtml(item.topic)}</b>`, `aria-label="Search Reddit for ${escapeHtml(item.topic)}"`)}<small style="display:block;font-size:11.5px;color:var(--mk-muted)">${escapeHtml(counts)}</small>${evidence ? `<small style="display:block;font-size:11.5px">${evidence}</small>` : ""}${examples ? `<ul style="margin:.25rem 0 0 .75rem;font-size:11.5px">${examples}</ul>` : ""}</li>`;
    }).join("") || `<li class="mk-hint">No topics yet: log a few threads or add searches to the watch list.</li>`;
  }

  const redditWalk = { available: false, requests: new Map(), sequence: 0, job: null };
  const REDDIT_WALK_STATES = { running: "Walking", stopping: "Stopping", done: "Walked", stopped: "Stopped", error: "Stopped" };
  const REDDIT_WALK_ERRORS = {
    not_signed_in: "Sign in to the ForkMesh browser extension first (click its toolbar icon).",
    reddit_walk_running: "A walk is already running.",
    reddit_watch_empty: "The watch list is empty. Add subreddits and searches first.",
    extension_unavailable: "The browser extension did not answer. Reload this page.",
    forbidden_sender: "Reload this page and try again.",
  };

  function renderRedditWalk() {
    const node = $(`[data-marketing-reddit-walk-status]`);
    const button = $(`[data-marketing-reddit-walk]`);
    if (!node || !button) return;
    const walk = redditWalk.job;
    const running = Boolean(walk) && ["running", "stopping"].includes(walk.state);
    button.textContent = running ? "Stop the walk" : "Walk the watch list";
    node.hidden = !walk;
    if (!walk) return;
    const stops = walk.stops || [];
    const read = stops.filter((item) => item.state === "read").length;
    const at = running && stops[walk.index] ? ` ${stops[walk.index].label}` : "";
    const sync = walk.synced;
    const sent = sync && !sync.error
      ? ` Sent: ${fmtFull(sync.threads)} new thread${sync.threads === 1 ? "" : "s"}, ${fmtFull(sync.already)} already logged${sync.rules ? `, ${fmtFull(sync.rules)} rule set${sync.rules === 1 ? "" : "s"}` : ""}.`
      : sync?.error ? ` The send stopped: ${String(sync.error).replaceAll("_", " ")}.` : "";
    node.textContent = `${REDDIT_WALK_STATES[walk.state] || "Walking"}${at}: ${fmtFull(read)} of ${fmtFull(stops.length)} pages read, ${fmtFull(walk.posts || 0)} post${walk.posts === 1 ? "" : "s"} seen.${sent}`;
  }

  function askRedditWalk(type) {
    const request = `reddit-walk-${++redditWalk.sequence}`;
    redditWalk.requests.set(request, type);
    window.postMessage({ source: "forkmesh-marketing-page", type, request }, location.origin);
  }

  function toggleRedditWalk() {
    if (!redditWalk.available) {
      message("notice", "Install the ForkMesh browser extension (Extension page) and reload this page to walk the watch list from here.");
      return;
    }
    const running = redditWalk.job && ["running", "stopping"].includes(redditWalk.job.state);
    askRedditWalk(running ? "marketing-reddit-walk-stop" : "marketing-reddit-walk");
  }

  function redditWalkProgress(walk) {
    const before = redditWalk.job;
    redditWalk.job = walk || null;
    renderRedditWalk();
    if (walk && walk.finishedAt && before?.finishedAt !== walk.finishedAt) void load();
  }

  function renderRedditRules(desk) {
    const list = $(`[data-marketing-reddit-rules-list]`); if (!list) return;
    const subreddits = desk.subreddits || [];
    const count = $(`[data-marketing-reddit-rules-count]`);
    if (count) count.textContent = subreddits.length ? `${fmtFull(desk.rulesOnFile || 0)} of ${fmtFull(subreddits.length)} read` : "";
    list.innerHTML = subreddits.map((item) => {
      const name = `r/${escapeHtml(item.name)}`;
      if (!item.rulesRead) {
        return `<li class="mk-rules-item"><div class="mk-rules-head"><b>${name}</b>${outLink(item.rulesUrl, "Open rules ↗", `class="mk-link" style="padding:0"`)}</div><small class="mk-hint">Not read yet — open the rules with the ForkMesh browser extension installed.</small></li>`;
      }
      const rules = item.rules || [];
      const when = `Read ${relativeTimeLabel(item.rulesReadAt)}${item.rulesReadBy ? ` by ${escapeHtml(item.rulesReadBy)}` : ""}`;
      const body = rules.length ? `<ol class="mk-rules">${rules.map((rule) => rule.description
        ? `<li><details><summary>${escapeHtml(rule.title || "Untitled rule")}</summary><p>${escapeHtml(rule.description)}</p></details></li>`
        : `<li><span>${escapeHtml(rule.title)}</span></li>`).join("")}</ol>` : `<small class="mk-hint">The rules page listed no rules.</small>`;
      return `<li class="mk-rules-item"><div class="mk-rules-head"><b>${name}</b><span class="mk-hint">${when}</span>${outLink(item.rulesUrl, "Rules ↗", `class="mk-link" style="padding:0"`)}</div>${body}</li>`;
    }).join("") || `<li class="mk-hint">No subreddits on the watch list.</li>`;
  }

  function redditRulesMarkdown(desk) {
    const read = (desk.subreddits || []).filter((item) => item.rulesRead);
    if (!read.length) return "";
    const flat = (text) => String(text || "").replace(/\s+/g, " ").trim();
    return [`# Subreddit rules from the Reddit desk (copied ${utcDate(Date.now())})`, "",
      ...read.flatMap((item) => [`## r/${item.name}`, "",
        `Read ${utcDate(item.rulesReadAt)} from ${item.rulesUrl}`, "",
        ...((item.rules || []).length
          ? item.rules.map((rule, index) => `${index + 1}. ${flat(rule.title) || "Untitled rule"}${rule.description ? `: ${flat(rule.description)}` : ""}`)
          : ["The rules page listed no rules."]), ""]),
      `Not read yet: ${(desk.subreddits || []).filter((item) => !item.rulesRead).map((item) => `r/${item.name}`).join(", ") || "none"}`].join("\n");
  }
  function copyRedditRules() {
    const text = redditRulesMarkdown(state.marketing.redditDesk || {});
    if (!text) { message("error", "No rules on file yet: open a watched subreddit's rules page with the ForkMesh browser extension and press Send to Reddit desk."); return; }
    copyText(text, "Rules copied for the scout.");
  }

  function openRedditThread(item = null) {
    $(`[data-marketing-reddit-status]`).innerHTML = REDDIT_STATUSES.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    $(`[data-marketing-reddit-title]`).textContent = item ? "Edit thread" : "Log thread";
    $(`[data-marketing-reddit-id]`).value = item?.id || "";
    const values = { url: item?.url, name: item?.title, status: item?.status || "to_review", landing: item?.landingPath || "/",
      note: item?.note, reply: item?.replyUrl, comment: item?.commentUrl };
    for (const [field, value] of Object.entries(values)) $(`[data-marketing-reddit-${field}]`).value = value || "";
    $(`[data-marketing-reddit-rules]`).checked = Boolean(item?.rulesChecked);
    $(`[data-marketing-reddit-disclosed]`).checked = Boolean(item?.disclosed);
    $(`[data-marketing-reddit-monitored]`).checked = Boolean(item?.monitored);
    $(`[data-marketing-reddit-delete]`).classList.toggle("hidden", !item);
    $(`[data-marketing-reddit-dialog]`).showModal();
  }
  async function saveRedditThread(event) {
    event.preventDefault(); const id = $(`[data-marketing-reddit-id]`).value;
    const field = (name) => $(`[data-marketing-reddit-${name}]`).value;
    const body = { url: field("url"), title: field("name"), status: field("status"), landingPath: field("landing"),
      note: field("note"), replyUrl: field("reply"), rulesChecked: $(`[data-marketing-reddit-rules]`).checked,
      disclosed: $(`[data-marketing-reddit-disclosed]`).checked, monitored: $(`[data-marketing-reddit-monitored]`).checked,
      commentUrl: field("comment") };
    try { await request(orgPath(`/reddit${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-marketing-reddit-dialog]`).close(); message("notice", "Thread saved."); await load(); }
    catch (error) {
      const existing = error.status === 409 ? redditThreadById(error.payload?.threadId) : null;
      if (existing) { $(`[data-marketing-reddit-dialog]`).close(); message("notice", "That thread is already logged; here it is."); openRedditThread(existing); }
      else message("error", `Could not save the thread: ${error.message}`);
    }
  }
  async function deleteRedditThread() {
    const id = $(`[data-marketing-reddit-id]`).value; if (!id || !confirm("Delete this thread from the log?")) return;
    try { await request(orgPath(`/reddit/${id}`), "DELETE", {}); $(`[data-marketing-reddit-dialog]`).close(); message("notice", "Thread deleted."); await load(); }
    catch (error) { message("error", `Could not delete the thread: ${error.message}`); }
  }
  const REDDIT_POSTS_SHOWN = 25;
  const redditPosts = { available: false, requests: new Set(), sequence: 0, running: false };
  const REDDIT_POSTS_ERRORS = {
    not_signed_in: "Sign in to the ForkMesh browser extension first (click its toolbar icon).",
    reddit_accounts_empty: "No Reddit accounts on the watch list yet. Add yours under Watch list, or open your Reddit profile and press Update in the extension.",
    reddit_accounts_running: "The extension is already updating your posts.",
    reddit_organization_mismatch: "The extension is set to another organization: choose this one in its popup, then try again.",
    extension_unavailable: "The browser extension did not answer. Reload this page.",
    forbidden_sender: "Reload this page and try again.",
  };
  const redditSubreddit = (url) => /\/r\/([A-Za-z0-9_]{2,21})(?:\/|$)/.exec(String(url || ""))?.[1] || "";
  const redditPostById = (id) => (state.marketing.posts || []).find((item) => item.id === id);

  function redditPostList() {
    return (state.marketing.posts || []).filter((item) => item.channel === "reddit")
      .sort((a, b) => ((b.postedAt || b.createdAt) - (a.postedAt || a.createdAt)) || String(a.id).localeCompare(String(b.id)));
  }

  function renderRedditPosts() {
    const body = $(`[data-marketing-reddit-posts]`); if (!body) return;
    const posts = redditPostList();
    const shown = state.view.redditPostsAll ? posts : posts.slice(0, REDDIT_POSTS_SHOWN);
    const views = posts.reduce((sum, item) => sum + (Number.isFinite(item.views) ? item.views : 0), 0);
    $(`[data-marketing-reddit-posts-count]`).textContent = posts.length
      ? `${fmtFull(posts.length)} post${posts.length === 1 ? "" : "s"} · ${fmtFull(views)} view${views === 1 ? "" : "s"}` : "";
    $(`[data-marketing-reddit-posts-empty]`).hidden = posts.length > 0;
    $(`[data-marketing-reddit-posts-more]`).hidden = posts.length <= REDDIT_POSTS_SHOWN;
    $(`[data-marketing-reddit-posts-more-button]`).textContent = state.view.redditPostsAll ? "Show fewer" : `Show all ${fmtFull(posts.length)}`;
    const count = (value) => Number.isFinite(value) ? fmtFull(value) : "—";
    body.innerHTML = shown.map((item) => {
      const where = redditSubreddit(item.url);
      const meta = [where ? `r/${where}` : "", item.postedAt ? `posted ${utcDate(item.postedAt)}` : "",
        item.source === "extension" ? "from the browser extension" : `added by ${item.createdBy || "a teammate"}`].filter(Boolean).join(" · ");
      return `<tr>
        <td>${outLink(item.url, `<b>${escapeHtml(item.title || item.url)}</b><small>${escapeHtml(meta)}</small>`, `class="mk-content-title" aria-label="Open on Reddit"`)}${item.note ? `<small style="display:block;font-size:11.5px;color:var(--mk-muted)">${escapeHtml(item.note)}</small>` : ""}</td>
        <td class="r mk-num">${count(item.views)}</td><td class="r mk-num">${count(item.likes)}</td><td class="r mk-num">${count(item.replies)}</td>
        <td><small class="mk-hint">${item.checkedAt ? escapeHtml(relativeTimeLabel(item.checkedAt)) : "not yet"}</small></td>
        <td class="r"><button data-marketing-reddit-post-edit="${escapeHtml(item.id)}" type="button" class="mk-link">Edit</button></td>
      </tr>`;
    }).join("");
  }

  function redditPostsStatus(text, error = false) {
    const node = $(`[data-marketing-reddit-posts-status]`); if (!node) return;
    node.textContent = text || "";
    node.hidden = !text;
    node.style.color = error ? "var(--mk-bad)" : "";
  }

  function updateRedditPosts() {
    if (!redditPosts.available) {
      message("notice", "Install the ForkMesh browser extension (Extension page) and reload this page to update your Reddit posts from here.");
      return;
    }
    if (redditPosts.running) return;
    redditPosts.running = true;
    $(`[data-marketing-reddit-posts-update]`).disabled = true;
    redditPostsStatus("The extension is reading your accounts' Reddit profiles in a background tab…");
    const request = `reddit-posts-${++redditPosts.sequence}`;
    redditPosts.requests.add(request);
    window.postMessage({ source: "forkmesh-marketing-page", type: "marketing-reddit-posts-update", request, organization: state.marketing.organization }, location.origin);
  }

  function redditPostsDone(response) {
    redditPosts.running = false;
    const button = $(`[data-marketing-reddit-posts-update]`); if (button) button.disabled = false;
    if (!response.ok) { redditPostsStatus(REDDIT_POSTS_ERRORS[response.error] || response.message || "The extension could not update your posts.", true); return; }
    redditPostsStatus(response.text || "Your posts are updated.");
    void load();
  }

  function redditPostsProgress(check) {
    if (!check) return;
    const read = Number(check.read || 0), total = Number(check.total || 0);
    if (check.done) {
      redditPostsStatus(`Views read for ${fmtFull(read)} of ${fmtFull(total)} link${total === 1 ? "" : "s"}${check.missed ? `; Reddit showed no views for ${fmtFull(check.missed)}` : ""}.`);
      void load();
    } else if (total) redditPostsStatus(`Reading views: ${fmtFull(read + Number(check.missed || 0))} of ${fmtFull(total)} links…`);
  }

  function redditPostDay(ts) { return ts ? utcDate(ts) : ""; }

  function openRedditPost(item = null) {
    $(`[data-marketing-reddit-post-title]`).textContent = item ? "Edit post" : "Add post";
    $(`[data-marketing-reddit-post-id]`).value = item?.id || "";
    $(`[data-marketing-reddit-post-url]`).value = item?.url || "";
    $(`[data-marketing-reddit-post-name]`).value = item?.title || "";
    $(`[data-marketing-reddit-post-date]`).value = item ? redditPostDay(item.postedAt) : utcDate(Date.now());
    $(`[data-marketing-reddit-post-note]`).value = item?.note || "";
    $$(`[data-marketing-reddit-post-stat]`).forEach((input) => {
      const value = item?.[input.dataset.marketingRedditPostStat];
      input.value = Number.isFinite(value) ? String(value) : "";
    });
    $(`[data-marketing-reddit-post-delete]`).classList.toggle("hidden", !item);
    $(`[data-marketing-reddit-post-dialog]`).showModal();
  }

  async function saveRedditPost(event) {
    event.preventDefault();
    const id = $(`[data-marketing-reddit-post-id]`).value;
    const existing = id ? redditPostById(id) : null;
    const body = { url: $(`[data-marketing-reddit-post-url]`).value, channel: "reddit",
      title: $(`[data-marketing-reddit-post-name]`).value, note: $(`[data-marketing-reddit-post-note]`).value };
    const day = $(`[data-marketing-reddit-post-date]`).value;
    if (redditPostDay(existing?.postedAt) !== day) body.postedAt = day ? Math.min(Date.parse(`${day}T12:00:00Z`), Date.now()) : 0;
    const stats = {};
    $$(`[data-marketing-reddit-post-stat]`).forEach((input) => {
      if (input.value.trim() !== "") stats[input.dataset.marketingRedditPostStat] = Math.max(0, Math.round(Number(input.value)));
    });
    const changed = !existing || Object.entries(stats).some(([key, value]) => existing[key] !== value);
    if (Object.keys(stats).length && changed) Object.assign(body, stats);
    try { await request(orgPath(`/posts${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-marketing-reddit-post-dialog]`).close(); message("notice", "Post saved."); await load(); }
    catch (error) {
      const found = error.status === 409 ? redditPostById(error.payload?.postId) : null;
      if (found) { $(`[data-marketing-reddit-post-dialog]`).close(); message("notice", "That post is already recorded; here it is."); openRedditPost(found); }
      else message("error", `Could not save the post: ${error.message}`);
    }
  }

  async function deleteRedditPost() {
    const id = $(`[data-marketing-reddit-post-id]`).value; if (!id || !confirm("Delete this post from Marketing? It stays on Reddit.")) return;
    try { await request(orgPath(`/posts/${id}`), "DELETE", {}); $(`[data-marketing-reddit-post-dialog]`).close(); message("notice", "Post deleted."); await load(); }
    catch (error) { message("error", `Could not delete the post: ${error.message}`); }
  }

  function openRedditWatch() {
    const canSave = state.marketing.canManageSettings, watch = state.marketing.settings.reddit || {};
    $(`[data-marketing-reddit-watch-subreddits]`).value = (watch.subreddits || []).join("\n");
    $(`[data-marketing-reddit-watch-queries]`).value = (watch.queries || []).join("\n");
    $(`[data-marketing-reddit-watch-accounts]`).value = (watch.accounts || []).join("\n");
    $$(`[data-marketing-reddit-watch-form] textarea`).forEach((input) => { input.disabled = !canSave; });
    $(`[data-marketing-reddit-watch-save]`).disabled = !canSave;
    $(`[data-marketing-reddit-watch-access]`).textContent = canSave ? "" : "Only organization owners and admins can change the watch list.";
    $(`[data-marketing-reddit-watch-dialog]`).showModal();
  }
  async function saveRedditWatch(event) {
    event.preventDefault();
    const lines = (name) => $(`[data-marketing-reddit-watch-${name}]`).value.split("\n").map((line) => line.trim()).filter(Boolean);
    try { await request(orgPath("/settings"), "PUT", { reddit: { subreddits: lines("subreddits"), queries: lines("queries"), accounts: lines("accounts") } });
      $(`[data-marketing-reddit-watch-dialog]`).close(); message("notice", "Watch list saved."); await load(); }
    catch (error) { message("error", `Could not save the watch list: ${error.message}`); }
  }

  async function addChannel(id) {
    const catalog = state.marketing.channelCatalog.find((channel) => channel.id === id);
    if (!catalog || state.marketing.addingChannel || !state.marketing.canManageSettings ||
        (state.marketing.settings.channels || []).some((channel) => channel.id === id)) return;
    state.marketing.addingChannel = id; renderCatalog();
    const channels = [...(state.marketing.settings.channels || []), {
      id: catalog.id, enabled: true, displayName: catalog.name, profileUrl: "",
    }];
    try {
      await request(orgPath("/settings"), "PUT", { channels });
      message("notice", `${catalog.name} was added to the dashboard.`);
      await load(); state.marketing.addingChannel = ""; showPage("catalog");
    } catch (error) {
      state.marketing.addingChannel = ""; renderCatalog();
      message("error", `Could not add ${catalog.name}: ${error.message}`);
    }
  }

  async function deleteChannel(id) {
    const name = channelLabel(id);
    if (!state.marketing.canManageSettings || !confirm(`Delete ${name} from the dashboard? Its logged numbers are deleted too. Content, ads and posted links stay, and you can add the channel again from Add channels.`)) return;
    try {
      await request(orgPath(`/channels/${encodeURIComponent(id)}`), "DELETE");
      $(`[data-marketing-settings-dialog]`)?.close(); message("notice", `${name} was deleted.`); await load();
    } catch (error) { message("error", `Could not delete ${name}: ${error.message}`); }
  }

  function renderContent(colors = channelColors(seriesByChannel())) {
    const list = $(`[data-marketing-content-list]`); if (!list) return;
    const query = String($(`[data-marketing-search]`)?.value || "").trim().toLowerCase();
    const channel = $(`[data-marketing-channel-filter]`)?.value || "";
    const campaign = $(`[data-marketing-campaign-filter]`)?.value || "";
    const base = state.marketing.content.filter((item) => {
      if (channel && item.channel !== channel) return false;
      if (campaign && item.campaign !== campaign) return false;
      return !query || [item.title, item.campaign, item.owner, item.body, ...(item.tags || [])]
        .some((value) => String(value || "").toLowerCase().includes(query));
    });
    const counts = Object.fromEntries(STATUSES.map(([status]) => [status, 0]));
    for (const item of base) counts[item.status] = (counts[item.status] || 0) + 1;
    $(`[data-marketing-status-tabs]`).innerHTML = [["", "All", base.length], ...STATUSES.map(([status, label]) => [status, label, counts[status]])]
      .map(([status, label, count]) => `<button data-marketing-status-tab="${status}" type="button" role="tab" class="mk-tab" aria-selected="${state.view.status === status}">${label}<em>${count}</em></button>`).join("");
    const items = state.view.status ? base.filter((item) => item.status === state.view.status) : base;
    $(`[data-marketing-content-count]`).textContent = `${fmtFull(state.marketing.content.length)} items`;
    const now = Date.now();
    list.innerHTML = items.slice(0, state.view.contentLimit).map((item) => {
      let when;
      if (item.status === "published" && item.publishedAt) when = `<b class="mk-num">${escapeHtml(longDate(item.publishedAt))}</b><small>${escapeHtml(relativeTimeLabel(item.publishedAt))}</small>`;
      else if (item.scheduledAt) {
        const late = item.scheduledAt < now && ["queued", "scheduled"].includes(item.status);
        when = `<b class="mk-num">${escapeHtml(longDate(item.scheduledAt))}</b><small class="${late ? "mk-warn" : ""}">${late ? `Overdue · ${escapeHtml(relativeTimeLabel(item.scheduledAt))}` : escapeHtml(relativeLabel(item.scheduledAt))}</small>`;
      } else when = `<b style="color:var(--mk-muted)">Unscheduled</b><small>Updated ${escapeHtml(relativeTimeLabel(item.updatedAt))}</small>`;
      const owner = item.owner || item.createdBy;
      return `<tr>
        <td><button data-marketing-content-edit="${escapeHtml(item.id)}" type="button" class="mk-content-title"><b>${escapeHtml(item.title)}</b><small>${escapeHtml(item.body || "No copy yet")}</small></button></td>
        <td><span class="mk-swatch"><i style="background:${colorFor(colors, item.channel)}"></i>${escapeHtml(channelLabel(item.channel))}</span></td>
        <td><span class="fm-label mk-pill ${escapeHtml(item.status)}">${STATUS_ICONS[item.status] || ""}${escapeHtml((STATUSES.find(([status]) => status === item.status) || [, item.status])[1])}</span></td>
        <td>${item.campaign ? `<span class="mk-tag">${escapeHtml(item.campaign)}</span>` : `<span style="color:var(--mk-muted)">—</span>`}</td>
        <td>${owner ? `<span class="mk-owner" title="${escapeHtml(owner)}">${escapeHtml(initials(owner))}</span>` : `<span style="color:var(--mk-muted)">—</span>`}</td>
        <td class="mk-when">${when}</td>
        <td class="r">${item.url?.startsWith("https://") ? `<a href="${escapeHtml(item.url)}" target="_blank" rel="noopener noreferrer" class="mk-link">Open ↗</a>` : ""}${item.channel === "x" && item.status !== "published" && xState().connected && canManage() ? `<button data-marketing-x-compose="${escapeHtml(item.id)}" type="button" class="mk-link">Post</button>` : ""}<button data-marketing-content-edit="${escapeHtml(item.id)}" type="button" class="mk-link">Edit</button></td>
      </tr>`;
    }).join("");
    $(`[data-marketing-content-empty]`).hidden = items.length > 0;
    const remaining = items.length - state.view.contentLimit;
    $(`[data-marketing-content-more]`).hidden = remaining <= 0;
    $(`[data-marketing-content-more-button]`).textContent = `Show ${Math.min(50, Math.max(0, remaining))} more of ${fmtFull(Math.max(0, remaining))}`;
    $$(`[data-marketing-stage]`).forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.marketingStage === state.view.status)));
  }

  function renderWeek(colors) {
    const host = $(`[data-marketing-week]`); if (!host) return;
    const today = startOfDay(Date.now());
    $(`[data-marketing-week-range]`).textContent = `${shortDate(today)} – ${shortDate(today + 6 * DAY)}`;
    host.innerHTML = Array.from({ length: 7 }, (_, index) => {
      const day = today + index * DAY;
      const items = state.marketing.content.filter((item) => item.scheduledAt >= day && item.scheduledAt < day + DAY && item.status !== "archived")
        .sort((a, b) => a.scheduledAt - b.scheduledAt);
      const date = new Date(day);
      return `<div class="mk-day${index === 0 ? " today" : ""}"><div class="mk-day-date"><small>${escapeHtml(index === 0 ? "Today" : date.toLocaleDateString(undefined, { weekday: "short" }))}</small><b>${date.getDate()}</b></div><div class="mk-slots">${items.length
        ? items.map((item) => `<button data-marketing-content-edit="${escapeHtml(item.id)}" type="button" class="mk-slot" title="${escapeHtml(`${channelLabel(item.channel)} · ${item.status}`)}"><i style="background:${colorFor(colors, item.channel)}"></i><time>${escapeHtml(timeOnly(item.scheduledAt))}</time><span>${escapeHtml(item.title)}</span></button>`).join("")
        : `<span class="mk-slot-empty">Nothing scheduled</span>`}</div></div>`;
    }).join("");
  }

  const HEALTH = [["", "All"], ["attention", "Needs attention"], ["stale", "Stale"], ["setup", "Not set up"],
    ["good", "On track"], ["paused", "Paused"]];
  const HEALTH_LABELS = Object.fromEntries(HEALTH.filter(([key]) => key));
  const SOURCE_LABELS = { extension: "via the extension", "x-profile": "via the extension (X)", crm: "from CRM", zoho: "from CRM and Zoho Mail", refresh: "by a channel refresh",
    "app-store-connect": "from App Store Connect", "app-store": "from the App Store", "": "logged" };
  const CAPTURE_STATES = { opening: "Opening the page…", permission: "Allow access in the new tab…",
    reading: "Reading the page…", done: "Updated", error: "Update failed" };
  const CAPTURE_ERRORS = {
    not_signed_in: "Sign in to the ForkMesh browser extension first.",
    channel_url_required: "Add the channel's link first (Manage channels).",
    channel_not_configured: "Add this channel first.",
    capture_found_no_numbers: "No follower, post, view or review counts were on that page. Check the channel link points at the profile.",
    capture_sign_in_needed: "The site asked you to sign in. Sign in there and press Update again.",
    capture_host_mismatch: "The channel link is not on this channel's site.",
    capture_page_timeout: "The page took too long to load.",
    capture_tab_closed: "The tab was closed before it was read.",
    permission_denied: "Chrome did not grant access to that site.",
    marketing_not_installed: "Marketing is not installed for the extension's organization.",
    forbidden_sender: "Reload this page and try again.",
    extension_unavailable: "The browser extension did not answer. Reload this page.",
  };
  const capture = { available: Boolean(document.documentElement.dataset.forkmeshMarketingCapture),
    jobs: new Map(), requests: new Map(), queue: [], sequence: 0 };

  const CHANNEL_CAPTURE_ERRORS = {
    reddit: { capture_found_no_numbers: "Reddit showed no member or visitor counts. Point the channel link at your subreddit (https://www.reddit.com/r/yourcommunity/), open it once in this browser to pass Reddit's check, then press Update again." },
  };
  const captureError = (job) => CHANNEL_CAPTURE_ERRORS[job?.channel]?.[job?.error] || CAPTURE_ERRORS[job?.error] || job?.message || "Update failed.";
  function channelSettings(id) { return (state.marketing.settings.channels || []).find((item) => item.id === id) || {}; }
  function catalogEntry(id) { return (state.marketing.channelCatalog || []).find((item) => item.id === id) || {}; }

  function captureButton(channel, className = "mk-link") {
    const job = capture.jobs.get(channel);
    const busy = job && !["done", "error"].includes(job.state);
    return `<button data-marketing-channel-capture="${escapeHtml(channel)}" type="button" class="${className}" ${busy ? "disabled" : ""} title="${escapeHtml(capture.available ? "Open the channel's page and read its numbers with the ForkMesh browser extension" : "Needs the ForkMesh browser extension")}">${busy ? "Updating…" : "Update"}</button>`;
  }
  function captureNote(channel) {
    const job = capture.jobs.get(channel);
    if (!job) return "";
    const text = job.state === "error" ? captureError(job) : CAPTURE_STATES[job.state] || "";
    return text ? `<span class="mk-capture-state${job.state === "error" ? " error" : ""}">${escapeHtml(text)}</span>` : "";
  }
  function refreshCaptureUi() {
    if (state.view.page === "channels") renderChannelHealth();
    else render();
  }

  function startCapture(channel) {
    const settings = channelSettings(channel);
    const name = settings.displayName || channelLabel(channel);
    if (!capture.available) {
      message("notice", `Install the ForkMesh browser extension (Extension page) to update ${name} in one click. Until then, log its numbers by hand.`);
      openMetric(channel);
      return false;
    }
    if (!String(settings.profileUrl || "").startsWith("https://")) {
      message("error", `Add ${name}'s link first: the extension opens that page to read it.`);
      if (state.marketing.canManageSettings) openSettings();
      return false;
    }
    const request = `capture-${++capture.sequence}`;
    capture.requests.set(request, channel);
    capture.jobs.set(channel, { channel, state: "opening" });
    window.postMessage({ source: "forkmesh-marketing-page", type: "marketing-capture", request,
      organization: state.marketing.organization, channel }, location.origin);
    refreshCaptureUi();
    return true;
  }
  function nextQueued() {
    while (capture.queue.length) {
      const channel = capture.queue.shift();
      if (startCapture(channel)) return;
    }
  }
  function captureStale() {
    const rows = (state.marketing.summary.channels || []).filter((item) =>
      ["stale", "attention", "setup"].includes(item.health?.state)
      && String(channelSettings(item.channel).profileUrl || "").startsWith("https://")
      && !(item.health?.checks || []).find((check) => check.id === "fresh")?.ok);
    if (!rows.length) { message("notice", "Every channel with a link was updated in the last 7 days."); return; }
    if (!capture.available) { startCapture(rows[0].channel); return; }
    capture.queue = rows.map((item) => item.channel);
    message("notice", `Updating ${rows.length} channel${rows.length === 1 ? "" : "s"} one at a time…`);
    nextQueued();
  }
  function captureFinished(job) {
    const name = job.name || channelLabel(job.channel);
    if (job.state === "done") {
      const metrics = job.result?.metrics || {};
      const parts = [["followers", job.channel === "reddit" ? "members" : "followers"], ["reach", "weekly visitors"], ["engagements", "weekly contributions"],
        ["posts", "posts"], ["likes", "likes"], ["views", "views"], ["reviews", "reviews"],
        ["indexedPages", "pages on Google"], ["visits", "visits from Google in 30 days"]]
        .filter(([key]) => metrics[key]).map(([key, label]) => `${fmtFull(metrics[key])} ${label}`);
      if (metrics.rating) parts.push(`rated ${metrics.rating}`);
      message("notice", `${name} updated${parts.length ? `: ${parts.join(" · ")}` : ""}.`);
      void load();
    } else {
      message("error", `${name}: ${captureError(job)}`);
    }
    if (capture.queue.length) setTimeout(nextQueued, 600);
  }
  const xSetup = { available: Boolean(document.documentElement.dataset.forkmeshMarketingXSetup), requests: new Set(), sequence: 0 };
  const X_SETUP_ERRORS = {
    not_signed_in: "Sign in to the ForkMesh browser extension first (click its toolbar icon), then press Guide me again.",
    organization_admin_required: "Only an owner or admin of this organization can connect X.",
    forbidden_sender: "Reload this page and try again.",
    extension_unavailable: "The browser extension did not answer. Reload this page.",
  };
  const X_SETUP_KEYS = [["apiKey", "API key"], ["apiSecret", "API key secret"], ["accessToken", "access token"], ["accessSecret", "access token secret"]];
  function xGuideStatus(text, error = false) {
    const node = $(`[data-marketing-x-guide-status]`); if (!node) return;
    node.textContent = text; node.hidden = !text; node.style.color = error ? "var(--destructive, #dc2626)" : "";
  }
  function startXSetup() {
    if (!canManage()) { xGuideStatus("Only an owner or admin of this organization can connect X.", true); return; }
    if (!capture.available) { xGuideStatus("Install the ForkMesh browser extension from the Extension page (/dashboard/extension), sign in to it, then press Guide me again.", true); return; }
    if (!xSetup.available) { xGuideStatus("Update the ForkMesh browser extension to 2.12.0 or newer (Extension page), reload this page, then press Guide me again.", true); return; }
    const request = `x-setup-${++xSetup.sequence}`;
    xSetup.requests.add(request);
    xGuideStatus("Opening X's developer console in a new tab…");
    window.postMessage({ source: "forkmesh-marketing-page", type: "marketing-x-setup", request, organization: state.marketing.organization }, location.origin);
  }
  async function xSetupProgress(setup) {
    if (!setup || setup.organization !== state.marketing.organization) return;
    if (setup.state === "done") {
      xGuideStatus(`Connected as @${setup.username}.`);
      try { const payload = await request(orgPath("/x-connection")); state.marketing.settings.x = payload.x; }
      catch { await load(); }
      const dialog = $(`[data-marketing-x-dialog]`); if (dialog?.open) dialog.close();
      message("notice", `X connected as @${setup.username} through the browser extension.`); renderX();
      return;
    }
    if (setup.state === "stopped") { xGuideStatus("The X setup was stopped."); return; }
    if (setup.state === "checking") { xGuideStatus("The extension has all four keys: checking them with X…"); return; }
    if (setup.state === "error") { xGuideStatus(`Not connected yet: ${setup.detail || setup.message || setup.error}. The guide in X's tab says what to do next.`, true); return; }
    const have = X_SETUP_KEYS.filter(([key]) => setup.keys?.[key]).map(([, label]) => label);
    xGuideStatus(`The guide is open in X's developer console tab.${setup.permission ? " Read and write permission set." : ""}${have.length ? ` Read so far: ${have.join(", ")}.` : ""}`);
  }
  function onExtensionMessage(event) {
    if (event.source !== window || event.origin !== location.origin || event.data?.source !== "forkmesh-extension") return;
    const data = event.data;
    if (data.type === "marketing-ready") {
      if (data.xSetup) xSetup.available = true;
      if (data.redditPosts) redditPosts.available = true;
      if (data.redditWalk && !redditWalk.available) {
        redditWalk.available = true;
        askRedditWalk("marketing-reddit-walk-state");
      }
      if (!capture.available) { capture.available = true; refreshCaptureUi(); }
      return;
    }
    if (data.type === "marketing-reddit-walk-update") { redditWalkProgress(data.walk); return; }
    if (data.type === "marketing-reddit-posts-progress") { redditPostsProgress(data.check); return; }
    if (data.type === "marketing-reddit-posts-update") {
      if (redditPosts.requests.delete(data.request)) redditPostsDone(data.response || {});
      return;
    }
    if (String(data.type).startsWith("marketing-reddit-walk")) {
      if (!redditWalk.requests.delete(data.request)) return;
      const response = data.response || {};
      if (!response.ok) message("error", REDDIT_WALK_ERRORS[response.error] || response.message || "The walk could not start.");
      else redditWalkProgress(response.walk);
      return;
    }
    if (data.type === "marketing-x-setup") {
      if (!xSetup.requests.delete(data.request)) return;
      const response = data.response || {};
      if (!response.ok) xGuideStatus(X_SETUP_ERRORS[response.error] || response.message || "The extension could not start the X setup.", true);
      else void xSetupProgress(response.setup);
      return;
    }
    if (data.type === "marketing-x-setup-update") { void xSetupProgress(data.setup); return; }
    if (data.type === "marketing-capture") {
      const channel = capture.requests.get(data.request);
      if (!channel) return;
      capture.requests.delete(data.request);
      const response = data.response || {};
      if (!response.ok) {
        const job = { channel, state: "error", error: response.error, message: response.message };
        capture.jobs.set(channel, job); refreshCaptureUi(); captureFinished(job);
      } else if (response.job) {
        capture.jobs.set(channel, response.job); refreshCaptureUi();
      }
      return;
    }
    if (data.type === "marketing-capture-update" && data.job?.channel
        && data.job.organization === state.marketing.organization) {
      capture.jobs.set(data.job.channel, data.job);
      refreshCaptureUi();
      if (data.job.state === "done" || data.job.state === "error") captureFinished(data.job);
    }
  }

  function factCell(label, value, extra = "", title = "") {
    return `<div${title ? ` title="${escapeHtml(title)}"` : ""}><dt>${escapeHtml(label)}</dt><dd>${value}${extra}</dd></div>`;
  }
  function engagementExplained(health) {
    const basis = health.engagement;
    if (!basis) {
      return health.engagementRate === null || health.engagementRate === undefined
        ? "No engagement rate: this channel has reported no engagements, or nothing to divide them by."
        : "";
    }
    const stale = basis.readings > 1
      ? ` The same two numbers have come back since ${shortDate(basis.unchangedSince)} (${basis.readings} readings), so they may not be refreshing.`
      : "";
    return `Engagement rate = ${basis.formula}. ${fmtFull(basis.engagements)} ${basis.engagementsLabel} and ${fmtFull(basis.baseValue)} ${basis.baseLabel} come from the same snapshot, read ${relativeTimeLabel(basis.observedAt)} ${SOURCE_LABELS[basis.source] ?? `via ${basis.source}`}.${stale}`;
  }
  function renderChannelHealth() {
    const host = $(`[data-marketing-channel-groups]`); if (!host) return;
    const summary = state.marketing.summary || {};
    const counts = summary.health || {};
    $$(`[data-marketing-health-stat]`).forEach((node) => { node.textContent = fmtFull(counts[node.dataset.marketingHealthStat] || 0); });
    const rows = (summary.channels || []).map((item) => ({ item, settings: channelSettings(item.channel),
      catalog: catalogEntry(item.channel), health: item.health || {} }));
    const audience = rows.reduce((total, row) => total + (row.item.followers || 0), 0);
    const change = rows.reduce((total, row) => total + (row.health.followersChange || 0), 0);
    const tracked = rows.some((row) => row.health.followersChange !== null && row.health.followersChange !== undefined);
    $(`[data-marketing-health-audience]`).textContent = fmtCompact(audience);
    const delta = $(`[data-marketing-health-change]`);
    delta.className = `mk-delta mk-num ${tracked && change > 0 ? "up" : tracked && change < 0 ? "down" : ""}`;
    delta.textContent = tracked ? `${change > 0 ? "▲" : change < 0 ? "▼" : "•"} ${fmtCompact(Math.abs(change))}` : "—";
    $(`[data-marketing-extension-banner]`).hidden = capture.available;
    const filter = state.view.healthFilter;
    $(`[data-marketing-health-tabs]`).innerHTML = HEALTH.map(([key, label]) => {
      const total = key ? counts[key] || 0 : rows.length;
      return `<button data-marketing-health-tab="${key}" type="button" role="tab" class="mk-tab" aria-selected="${filter === key}">${escapeHtml(label)} <em>${fmtFull(total)}</em></button>`;
    }).join("");
    const categories = [...new Set(rows.map((row) => row.catalog.category).filter(Boolean))].sort((a, b) => a.localeCompare(b));
    const select = $(`[data-marketing-channels-category]`);
    const chosen = categories.includes(select.value) ? select.value : "";
    select.innerHTML = `<option value="">All categories</option>` + categories.map((name) => `<option value="${escapeHtml(name)}">${escapeHtml(name)}</option>`).join("");
    select.value = chosen;
    const query = ($(`[data-marketing-channels-search]`).value || "").trim().toLowerCase();
    const visible = rows.filter((row) => (!filter || row.health.state === filter)
      && (!chosen || row.catalog.category === chosen)
      && (!query || `${row.settings.displayName || ""} ${row.catalog.name || ""} ${row.catalog.category || ""}`.toLowerCase().includes(query)));
    const groups = new Map();
    for (const row of visible) {
      const key = row.catalog.category || "Other";
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(row);
    }
    const playbooks = state.marketing.channelPlaybooks || {};
    const colors = channelColors(seriesByChannel());
    $(`[data-marketing-channels-empty]`).hidden = visible.length > 0;
    host.innerHTML = [...groups.entries()].sort((a, b) => a[0].localeCompare(b[0])).map(([category, list]) => {
      const playbook = playbooks[category];
      const guide = playbook ? `<details class="mk-playbook"><summary>Playbook</summary><p><b>Track:</b> ${escapeHtml(playbook.kpis.join(" · "))}. <b>Cadence:</b> at least every ${playbook.cadenceDays} day${playbook.cadenceDays === 1 ? "" : "s"}.</p><ul>${playbook.tips.map((tip) => `<li>${escapeHtml(tip)}</li>`).join("")}</ul></details>` : "";
      return `<section class="mk-group"><div class="mk-group-head"><div><h3>${escapeHtml(category)} · ${list.length}</h3>${playbook ? `<p>${escapeHtml(playbook.goal)}</p>` : ""}</div>${guide}</div>
        <div class="mk-cards">${list.map((row) => channelCard(row, colors)).join("")}</div></section>`;
    }).join("");
  }
  function statsGuide(item, health) {
    const steps = (health.guide || []).map((step) => ({ ...step, done: step.done || (step.action === "extension" && capture.available) }));
    if (!steps.length || steps.every((step) => step.done)) return "";
    const name = channelLabel(item.channel);
    const actions = {
      link: state.marketing.canManageSettings ? `<button data-marketing-guide-link type="button" class="mk-link">Add link</button>` : "",
      extension: `<a href="/dashboard/extension" class="mk-link">Get the extension</a>`,
      capture: captureButton(item.channel),
      crm: `<a href="${escapeHtml(crmPath())}" class="mk-link">Open CRM</a>`,
      zoho: `<a href="${escapeHtml(crmPath())}" class="mk-link">Connect Zoho Mail in CRM</a>`,
      refresh: `<button data-marketing-channel-refresh="${escapeHtml(item.channel)}" type="button" class="mk-link">Refresh</button>`,
      "app-store": `<button data-marketing-page="app-store" type="button" class="mk-link">Open the App Store desk</button>`,
    };
    return `<ol class="mk-checks mk-guide" aria-label="${escapeHtml(`How to get ${name}'s numbers`)}"><li class="mk-guide-head"><b>${escapeHtml(`Get ${name}'s numbers`)}</b></li>${steps.map((step, index) => {
      const next = !step.done && steps.slice(0, index).every((before) => before.done);
      return `<li class="${step.done ? "ok" : "missing"}">${step.done ? CHECK_ICON : WARN_ICON}<span>${index + 1}. ${escapeHtml(step.label)}${next && actions[step.action] ? `<br>${actions[step.action]}` : ""}</span></li>`;
    }).join("")}</ol>`;
  }
  function openLink(settings) {
    const url = [settings.openUrl, settings.profileUrl].map((value) => String(value || "")).find((value) => value.startsWith("https://"));
    return url ? `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer" class="mk-link">Open ↗</a>` : "";
  }
  function channelDesk(item, health) {
    const now = Date.now(), channel = item.channel, cadence = Math.max(1, health.cadenceDays || 1);
    const due = health.lastPostAt ? health.lastPostAt + cadence * DAY : 0;
    const content = (state.marketing.content || []).filter((entry) => entry.channel === channel);
    const queued = content.filter((entry) => ["queued", "scheduled"].includes(entry.status) && entry.scheduledAt >= now)
      .sort((a, b) => a.scheduledAt - b.scheduledAt)[0];
    const drafts = content.filter((entry) => ["draft", "review"].includes(entry.status)).length;
    const live = (state.marketing.campaigns || []).filter((campaign) => ["active", "planned"].includes(campaign.status)
      && (campaign.channels || []).includes(channel));
    const waiting = live.filter((campaign) => (campaign.stats?.uncoveredChannels || []).includes(channel)).length;
    const ads = (state.marketing.ads || []).filter((ad) => ad.channel === channel && ad.status !== "archived");
    const facts = [
      factCell("Post due", due ? escapeHtml(relativeLabel(due)) : "now", "", `Aim: a post every ${cadence} day${cadence === 1 ? "" : "s"}`),
      factCell("Queued", queued ? escapeHtml(relativeLabel(queued.scheduledAt)) : "—", "", queued ? queued.title : `${drafts} draft${drafts === 1 ? "" : "s"}`),
      factCell("Campaigns", fmtFull(live.length), waiting ? ` <small class="down">${waiting} waiting</small>` : "", live.map((campaign) => campaign.name).join(", "))];
    if (ads.length) facts.push(factCell("Ads live", `${fmtFull(ads.filter((ad) => ad.status === "active").length)}/${fmtFull(ads.length)}`));
    if (channel === "app-store") return appStoreDesk(item);
    if (channel === "blog") return `<dl class="mk-facts mk-desk" aria-label="Website desk">${facts.join("")}</dl>
      <p class="mk-hint"><button data-marketing-page="website" type="button" class="mk-link">Open the Website channel</button></p>`;
    if (channel !== "reddit") return `<dl class="mk-facts mk-desk" aria-label="Desk">${facts.join("")}</dl>`;
    const desk = state.marketing.redditDesk || {}, summary = desk.summary || {};
    facts.push(factCell("Threads · 7D", fmtFull(summary.loggedThisWeek || 0), ` <small>${fmtFull(summary.repliedThisWeek || 0)} replied</small>`),
      factCell("Open", fmtFull(summary.open || 0), summary.monitored ? ` <small>${fmtFull(summary.monitored)} watched</small>` : ""),
      factCell("Rules on file", `${fmtFull(desk.rulesOnFile || 0)}/${fmtFull((desk.subreddits || []).length)}`));
    const places = ((desk.ideas || {}).subreddits || []).slice(0, 3).map((place) => `r/${escapeHtml(place.name)}`).join(" · ");
    return `<dl class="mk-facts mk-desk" aria-label="Reddit desk">${facts.join("")}</dl>
      <p class="mk-hint">${places ? `Post next: ${places}. ` : ""}<button data-marketing-page="reddit" type="button" class="mk-link">Open the Reddit desk</button></p>`;
  }
  function appStoreDesk(item) {
    const store = shownAppStore();
    if (!store) void loadAppStore();
    const month = (store?.windows || []).find((entry) => entry.days === 30) || {};
    const facts = [
      factCell("Downloads · 30D", store?.lastDay ? fmtFull(month.downloads || 0) : "—", month.comparable ? ` <small>${escapeHtml(changeDelta(month.downloads || 0, month.previous?.downloads || 0)[1])}</small>` : ""),
      factCell("Updates · 30D", store?.lastDay ? fmtFull(month.updates || 0) : "—"),
      factCell("Reported to", store?.lastDay ? escapeHtml(dayLabel(store.lastDay)) : "—", "", store?.publishedAt ? `Sent ${relativeTimeLabel(store.publishedAt)}` : "")];
    const days = (store?.days || []).slice(-30).map((day) => day.downloads || 0);
    const spark = days.length > 1 ? (() => { const max = Math.max(1, ...days);
      return `<svg viewBox="0 0 120 24" class="mk-app-store-spark" aria-label="Downloads per day, last 30 reported days"><path d="${days.map((value, index) => `${index ? "L" : "M"}${(index * 120 / (days.length - 1)).toFixed(1)},${(22 - value / max * 20).toFixed(1)}`).join("")}" fill="none" style="stroke:var(--mk-s1)" stroke-width="1.5"/></svg>`; })() : "";
    return `<dl class="mk-facts mk-desk" aria-label="App Store desk">${facts.join("")}</dl>
      <p class="mk-hint">${spark}<button data-marketing-page="app-store" type="button" class="mk-link">Open the App Store desk</button></p>`;
  }
  function channelCard({ item, settings, catalog, health }, colors) {
    const name = settings.displayName || catalog.name || item.channel;
    const snap = settings.capture || {};
    let where = snap.handle || snap.title || "";
    if (!where && String(settings.profileUrl || "").startsWith("https://")) {
      try { const url = new URL(settings.profileUrl); where = url.host + url.pathname.replace(/\/$/, ""); } catch { where = ""; }
    }
    const change = health.followersChange;
    const changeText = change === null || change === undefined ? ""
      : ` <small class="${change > 0 ? "up" : change < 0 ? "down" : ""}">${change > 0 ? "+" : change < 0 ? "−" : "±"}${fmtCompact(Math.abs(change))}</small>`;
    const reddit = item.channel === "reddit";
    const facts = [factCell(reddit ? "Members" : "Followers", item.followers ? fmtCompact(item.followers) : "—", changeText),
      factCell("Eng. rate", health.engagementRate ? `${health.engagementRate.toFixed(1)}%` : "—", "", engagementExplained(health)),
      factCell("Posts · 30D", fmtFull(health.postsLast30d || 0)),
      factCell("Last post", health.lastPostAt ? escapeHtml(relativeLabel(health.lastPostAt)) : "—")];
    if (item.rating) facts.push(factCell("Rating", `${item.rating.toFixed(1)}★`));
    if (reddit) facts.push(factCell("Weekly visitors", item.reach ? fmtCompact(item.reach) : "—"), factCell("Weekly contrib.", item.engagements ? fmtCompact(item.engagements) : "—"));
    for (const [key, label] of [["downloads", "Downloads · 30D"], ["ratings", "Ratings"], ["stars", "Stars"], ["forks", "Forks"], ["outbound", "Emails sent · 30D"], ["closedDeals", "Closed deals"], ["closedValue", "Won value"], ["replies", "Replies · 30D"], ["visits", "Visits · 30D"], ["indexedPages", "Pages on Google"], ["reviews", "Reviews"], ["views", "Views"], ["likes", "Likes"], ["following", "Following"], ["posts", "Posts"]]) {
      if (item[key] && facts.length < 9) facts.push(factCell(label, fmtCompact(item[key])));
    }
    const checks = (health.checks || []).map((check) => `<li class="${check.ok ? "ok" : "missing"}">${check.ok ? CHECK_ICON : WARN_ICON}<span>${escapeHtml(check.label)}</span></li>`).join("");
    const recent = (snap.recent || []).slice(0, 3).map((post) => `<li>${post.url ? `<a href="${escapeHtml(post.url)}" target="_blank" rel="noopener noreferrer">${escapeHtml(post.title || post.url)}</a>` : `<span>${escapeHtml(post.title)}</span>`}${post.at ? `<time>${escapeHtml(shortDate(post.at))}</time>` : ""}</li>`).join("");
    const updated = health.observedAt
      ? `Updated ${escapeHtml(relativeTimeLabel(health.observedAt))} ${escapeHtml(SOURCE_LABELS[health.source] ?? `via ${health.source}`)}`
      : "Never updated";
    const open = openLink(settings);
    return `<article class="mk-card${health.state === "paused" ? " paused" : ""}" data-marketing-channel-card="${escapeHtml(item.channel)}">
      <div class="mk-card-head"><span class="mk-glyph" style="background:${colorFor(colors, item.channel)}">${escapeHtml(initials(name))}</span><div><b>${escapeHtml(name)}</b><small title="${escapeHtml(snap.description || where)}">${escapeHtml(where || "No link yet")}</small></div><span class="fm-label mk-health ${escapeHtml(health.state || "")}">${escapeHtml(HEALTH_LABELS[health.state] || "—")}</span></div>
      <dl class="mk-facts">${facts.join("")}</dl>
      ${channelDesk(item, health)}
      ${checks ? `<ul class="mk-checks">${checks}</ul>` : ""}
      ${statsGuide(item, health)}
      ${recent ? `<ul class="mk-recent" aria-label="Recent posts">${recent}</ul>` : ""}
      ${item.channel === "email" ? emailSuggestionsList() : ""}
      ${item.channel === "discord" ? discordPostsList() : ""}
      ${item.channel === "x" ? readyPostBlock() : ""}
      <div class="mk-card-foot"><span>${updated}</span><div>${captureNote(item.channel)}${open}<button data-marketing-channel-log="${escapeHtml(item.channel)}" type="button" class="mk-link">Log</button><button data-marketing-channel-context="${escapeHtml(item.channel)}" type="button" class="mk-link" title="${escapeHtml(`Copy ${name}'s numbers, posts and a prompt to paste into an AI chat. It suggests how to grow the channel and checks its earlier advice against the numbers each time you copy again.`)}">AI context</button>${state.marketing.canManageSettings ? `<button data-marketing-channel-delete="${escapeHtml(item.channel)}" type="button" class="mk-link">Delete</button>` : ""}${captureButton(item.channel, "fm-btn fm-btn-sm")}</div></div>
    </article>`;
  }

  const EMAIL_KINDS = { reply: "Reply", follow_up: "Follow up", intro: "Introduce" };
  function emailSuggestionsList() {
    if (state.marketing.emailSuggestions?.org !== state.marketing.organization) void loadEmailSuggestions();
    const data = state.marketing.emailSuggestions;
    if (data.loading) return `<p class="mk-hint">Looking through CRM for people to email…</p>`;
    if (data.error) return `<p class="mk-hint">No suggested emails: ${escapeHtml(REFRESH_ERRORS[data.error] || data.error)}</p>`;
    const items = data.suggestions || [];
    if (!items.length) return `<p class="mk-hint">Nobody needs an email right now.</p>`;
    const mailbox = data.provider === "gmail" ? "Gmail" : "Zoho Mail";
    return `<div class="mk-mail" data-marketing-email-suggestions><b>Suggested emails · ${fmtFull(items.length)}${data.total > items.length ? ` of ${fmtFull(data.total)}` : ""}</b>
      <ul>${items.map((item) => `<li><span><b>${escapeHtml(item.name || item.email)}</b><small>${escapeHtml([EMAIL_KINDS[item.kind], item.reason].filter(Boolean).join(" · "))}</small><small title="${escapeHtml(item.body)}">${escapeHtml(item.subject)}</small></span><a href="${escapeHtml(item.composeUrl)}" target="_blank" rel="noopener noreferrer" class="fm-btn fm-btn-sm" title="${escapeHtml(`Open a ${mailbox} message to ${item.email} with the subject and text filled in`)}">Open in ${mailbox} ↗</a></li>`).join("")}</ul>
      <small class="mk-hint">Fill in any [bracketed] parts before you send.</small></div>`;
  }
  async function loadEmailSuggestions() {
    const org = state.marketing.organization;
    if (state.marketing.emailSuggestions?.org === org && state.marketing.emailSuggestions.loading) return;
    state.marketing.emailSuggestions = { org, loading: true };
    try { const payload = await request(orgPath("/email-suggestions"));
      state.marketing.emailSuggestions = { org, ...payload.email }; }
    catch (error) { state.marketing.emailSuggestions = { org, error: error.message }; }
    if (org === state.marketing.organization && state.view.page === "channels") renderChannelHealth();
  }

  const DISCORD_POSTS_FRESH = 60000;
  function discordPostsList() {
    void loadDiscordPosts();
    const data = state.marketing.discordPosts;
    if (data.loading) return `<p class="mk-hint">Reading the Discord channels…</p>`;
    if (data.error) return `<p class="mk-hint">No Discord posts: ${escapeHtml(REFRESH_ERRORS[data.error] || data.error)}</p>`;
    const posts = data.posts || [];
    const channels = (data.channels || []).map((name) => `#${name}`).join(", ");
    if (!posts.length) return `<p class="mk-hint">${escapeHtml(`Nothing was posted in ${channels} in the last ${data.days} days.`)}</p>`;
    return `<div class="mk-mail mk-discord" data-marketing-discord-posts><b>${escapeHtml(`Posts in the last ${data.days} days · ${fmtFull(posts.length)}`)}<small class="mk-hint"> read ${escapeHtml(relativeTimeLabel(data.readAt))}</small></b>
      <ul>${posts.map((post) => `<li><span><small>${escapeHtml([`#${post.channel}`, post.author + (post.bot ? " (bot)" : ""), shortDate(post.postedAt)].join(" · "))}</small><span title="${escapeHtml(post.content)}">${escapeHtml(post.content || "(no text)")}${post.edited ? " <small>(edited)</small>" : ""}</span></span>${post.url ? `<a href="${escapeHtml(post.url)}" target="_blank" rel="noopener noreferrer" class="mk-link">Open ↗</a>` : ""}</li>`).join("")}</ul>
      ${data.contentMissing ? `<small class="mk-hint">The bot cannot read message text: turn on the Message Content intent for the bot in the Discord developer portal.</small>` : ""}
      ${data.truncated ? `<small class="mk-hint">Only the latest 50 posts of each channel are read.</small>` : ""}</div>`;
  }
  async function loadDiscordPosts(force) {
    const org = state.marketing.organization;
    const held = state.marketing.discordPosts?.org === org ? state.marketing.discordPosts : null;
    if (held && (held.loading || (!force && Date.now() - (held.readAt || 0) < DISCORD_POSTS_FRESH))) return;
    state.marketing.discordPosts = { org, loading: true };
    try { const payload = await request(orgPath("/discord-posts"));
      state.marketing.discordPosts = { org, readAt: Date.now(), ...payload.discord }; }
    catch (error) { state.marketing.discordPosts = { org, readAt: Date.now(), error: error.message }; }
    if (org === state.marketing.organization && state.view.page === "channels") renderChannelHealth();
  }

  const APP_STORE_METRICS = { downloads: "Downloads", redownloads: "Redownloads", updates: "Updates" };
  const APP_STORE_RANGES = { 30: "30D", 90: "90D", 365: "1Y" };
  const regionNames = typeof Intl.DisplayNames === "function" ? new Intl.DisplayNames(undefined, { type: "region" }) : null;
  function countryName(code) { try { return regionNames?.of(code) || code; } catch { return code; } }
  function dayLabel(day, options = {}) {
    return day ? new Date(`${day}T00:00:00Z`).toLocaleDateString(undefined, { month: "short", day: "numeric", timeZone: "UTC", ...options }) : "";
  }
  function shiftDay(day, delta) {
    const date = new Date(`${day}T00:00:00Z`); date.setUTCDate(date.getUTCDate() + delta); return date.toISOString().slice(0, 10);
  }
  function starText(rating) {
    const whole = Math.max(0, Math.min(5, Math.round(rating || 0)));
    return "★".repeat(whole) + "☆".repeat(5 - whole);
  }
  function shownAppStore() {
    const store = state.marketing.appStore;
    return store && store.org === state.marketing.organization ? store : null;
  }
  async function loadAppStore() {
    const org = state.marketing.organization;
    if (!org || !state.marketing.installed || state.marketing.appStore?.loading === org) return;
    state.marketing.appStore = { ...(shownAppStore() || { org }), loading: org };
    try {
      const payload = await request(orgPath("/app-store"));
      if (org !== state.marketing.organization) return;
      state.marketing.appStore = { org, ...payload.appStore, canPublish: Boolean(payload.canPublish) };
    } catch (error) {
      if (org !== state.marketing.organization) return;
      state.marketing.appStore = { org, error: error.message };
    }
    if (state.view.page === "app-store") renderAppStore();
    if (state.view.page === "channels") renderChannelHealth();
  }
  function appStoreBuckets(store) {
    const range = state.view.appStoreRange, width = range > 90 ? 7 : 1;
    const span = Math.floor(range / width) * width;
    const known = new Map((store.days || []).map((day) => [day.day, day]));
    const buckets = [];
    for (let end = 0; end < span; end += width) {
      const to = shiftDay(store.lastDay, -end), from = shiftDay(store.lastDay, -(end + width - 1));
      const bucket = { from, to, seen: false, downloads: 0, redownloads: 0, updates: 0 };
      for (let day = from; day <= to; day = shiftDay(day, 1)) {
        const entry = known.get(day); if (!entry) continue;
        bucket.seen = true;
        for (const key of Object.keys(APP_STORE_METRICS)) bucket[key] += entry[key] || 0;
      }
      buckets.unshift(bucket);
    }
    return buckets;
  }
  function renderAppStoreChart(store) {
    const host = $(`[data-marketing-app-store-chart]`); if (!host) return;
    const metric = state.view.appStoreMetric, weekly = state.view.appStoreRange > 90;
    $(`[data-marketing-app-store-chart-title]`).textContent = `${APP_STORE_METRICS[metric]} per ${weekly ? "week" : "day"}`;
    if (!store.lastDay) {
      host.innerHTML = blankslate("chart", "No downloads yet", "App Store Connect's daily numbers appear here once the App Store report sends them.");
      return;
    }
    const buckets = appStoreBuckets(store);
    const W = 760, H = 240, L = 44, R = 10, T = 10, B = 26;
    const max = Math.max(0, ...buckets.map((bucket) => bucket[metric]));
    const step = niceStep(max), top = Math.max(step, Math.ceil(max / step) * step);
    const slot = (W - L - R) / buckets.length, barWidth = Math.max(1, slot * 0.72);
    const y = (value) => T + (1 - value / top) * (H - T - B);
    let svg = "";
    for (let value = 0; value <= top + step / 2; value += step) {
      svg += `<line x1="${L}" x2="${W - R}" y1="${y(value)}" y2="${y(value)}" style="stroke:var(${value ? "--mk-line-soft" : "--mk-line"})" stroke-width="1"/>`
        + `<text x="${L - 8}" y="${y(value) + 3.5}" text-anchor="end">${escapeHtml(fmtCompact(value))}</text>`;
    }
    buckets.forEach((bucket, index) => {
      if (!bucket.seen || !bucket[metric]) return;
      const height = Math.max(1, H - B - y(bucket[metric]));
      svg += `<rect class="mk-day-bar" data-marketing-app-store-bar="${index}" x="${(L + index * slot + (slot - barWidth) / 2).toFixed(1)}" y="${(H - B - height).toFixed(1)}" width="${barWidth.toFixed(1)}" height="${height.toFixed(1)}" rx="1.5"/>`;
    });
    for (let tick = 0; tick < 5; tick += 1) {
      const index = Math.round((buckets.length - 1) * tick / 4);
      svg += `<text x="${L + (index + 0.5) * slot}" y="${H - 8}" text-anchor="${tick === 0 ? "start" : tick === 4 ? "end" : "middle"}">${escapeHtml(dayLabel(buckets[index].to))}</text>`;
    }
    svg += `<rect data-marketing-app-store-hit x="${L}" y="${T}" width="${W - L - R}" height="${H - T - B}" fill="transparent"/>`;
    const label = `${APP_STORE_METRICS[metric]} per ${weekly ? "week" : "day"}, ${dayLabel(buckets[0].from, { year: "numeric" })} to ${dayLabel(store.lastDay, { year: "numeric" })}`;
    host.innerHTML = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${escapeHtml(label)}">${svg}</svg><div data-marketing-app-store-tip class="mk-tip" hidden></div>`;
    const chart = host.querySelector("svg"), tip = host.querySelector("[data-marketing-app-store-tip]");
    const hit = host.querySelector("[data-marketing-app-store-hit]");
    let active = null;
    hit.addEventListener("pointermove", (event) => {
      const box = chart.getBoundingClientRect();
      const index = Math.max(0, Math.min(buckets.length - 1, Math.floor(((event.clientX - box.left) * W / box.width - L) / slot)));
      const bucket = buckets[index];
      active?.classList.remove("active");
      active = host.querySelector(`[data-marketing-app-store-bar="${index}"]`); active?.classList.add("active");
      const when = weekly ? `${dayLabel(bucket.from)} – ${dayLabel(bucket.to, { year: "numeric" })}` : dayLabel(bucket.to, { weekday: "short", year: "numeric" });
      tip.innerHTML = `<b>${escapeHtml(when)}</b>` + (bucket.seen
        ? Object.entries(APP_STORE_METRICS).map(([key, name]) => `<div><span>${escapeHtml(name)}</span><span class="mk-num">${escapeHtml(fmtFull(bucket[key]))}</span></div>`).join("")
        : `<div><span>No report sent for this ${weekly ? "week" : "day"}</span></div>`);
      tip.hidden = false;
      const px = (L + (index + 0.5) * slot) * box.width / W, tipWidth = tip.offsetWidth;
      tip.style.left = `${px + 12 + tipWidth > box.width ? px - 12 - tipWidth : px + 12}px`;
    });
    hit.addEventListener("pointerleave", () => { tip.hidden = true; active?.classList.remove("active"); active = null; });
  }
  function appStoreShareRows(rows, total, name) {
    if (!rows.length) return `<tr><td colspan="3" class="mk-hint">No downloads in this range.</td></tr>`;
    return rows.map((row) => {
      const share = total ? row.downloads / total * 100 : 0;
      return `<tr><td>${escapeHtml(name(row))}</td><td><div class="mk-share"><span class="mk-track"><span class="mk-bar" style="width:${Math.max(share ? 2 : 0, share)}%"></span></span><span class="mk-num mk-hint">${share.toFixed(1)}%</span></div></td><td class="r mk-num">${escapeHtml(fmtFull(row.downloads))}</td></tr>`;
    }).join("");
  }
  function appStoreNotes(store) {
    if (!store) return "Reading the App Store numbers…";
    if (store.error) return `Could not read the App Store numbers: ${store.error}`;
    const notes = [];
    if (!store.lastDay) notes.push("No downloads yet. Run native/ios/tools/asc_store_report.py --publish on the computer that holds the App Store Connect key; it sends each day's downloads here.");
    else if (!store.fresh) notes.push(`App Store Connect last sent numbers ${relativeTimeLabel(store.publishedAt)}. Run the App Store report with --publish again to catch up.`);
    if (!store.lookup?.lookedUpAt) notes.push("Press Refresh on the App Store channel to read the app's rating and store listing.");
    return notes.join(" ");
  }
  function renderAppStore() {
    const store = shownAppStore();
    if (!store || (!store.loading && !store.error && !("lastDay" in store))) void loadAppStore();
    const data = store || {}, lookup = data.lookup || {}, app = data.app || {};
    const range = state.view.appStoreRange, rangeLabel = APP_STORE_RANGES[range] || `${range}D`;
    $$(`[data-marketing-app-store-range]`).forEach((button) => button.setAttribute("aria-pressed", String(Number(button.dataset.marketingAppStoreRange) === range)));
    $$(`[data-marketing-app-store-metric]`).forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.marketingAppStoreMetric === state.view.appStoreMetric)));
    const title = $(`[data-marketing-app-store-title]`); if (!title) return;
    title.textContent = lookup.name || app.name || "Apple App Store";
    $(`[data-marketing-app-store-sent]`).textContent = data.publishedAt
      ? `Sent from App Store Connect ${relativeTimeLabel(data.publishedAt)}${data.publishedBy ? ` by ${data.publishedBy}` : ""} · reported to ${dayLabel(data.lastDay, { year: "numeric" })}`
      : "Downloads from App Store Connect, rating from the App Store";
    const notes = appStoreNotes(store && ("lastDay" in store || store.error) ? store : null);
    $(`[data-marketing-app-store-setup]`).hidden = !notes;
    $(`[data-marketing-app-store-setup-text]`).textContent = notes;
    const shown = (data.windows || []).find((item) => item.days === range) || {};
    for (const [key, name] of Object.entries(APP_STORE_METRICS)) {
      $(`[data-marketing-app-store-label="${key}"]`).textContent = `${name} · ${rangeLabel}`;
      $(`[data-marketing-app-store-stat="${key}"]`).textContent = data.lastDay ? fmtFull(shown[key] || 0) : "—";
      const delta = data.lastDay && shown.comparable ? changeDelta(shown[key] || 0, shown.previous?.[key] || 0) : ["", "—"];
      const node = $(`[data-marketing-app-store-delta="${key}"]`);
      node.className = `mk-delta mk-num ${delta[0]}`; node.textContent = delta[1];
      node.title = shown.comparable ? `Against the ${range} days before: ${fmtFull(shown.previous?.[key] || 0)}` : "Not enough history to compare yet";
    }
    $(`[data-marketing-app-store-note="downloads"]`).textContent = shown.from ? `${dayLabel(shown.from)} – ${dayLabel(shown.to)}` : "";
    $(`[data-marketing-app-store-stat="rating"]`).textContent = lookup.rating ? `${lookup.rating.toFixed(1)}★` : "—";
    $(`[data-marketing-app-store-note="rating"]`).textContent = lookup.ratings
      ? `${fmtFull(lookup.ratings)} rating${lookup.ratings === 1 ? "" : "s"}${lookup.country ? ` · ${lookup.country} store` : ""}`
      : lookup.lookedUpAt ? "No ratings yet" : "Refresh to read";
    $(`[data-marketing-app-store-stat="total"]`).textContent = data.lastDay ? fmtFull(data.total?.downloads || 0) : "—";
    $(`[data-marketing-app-store-note="total"]`).textContent = data.firstDay ? `since ${dayLabel(data.firstDay, { year: "numeric" })}` : "";
    $(`[data-marketing-app-store-stat="best"]`).textContent = data.bestDay ? fmtFull(data.bestDay.downloads) : "—";
    $(`[data-marketing-app-store-note="best"]`).textContent = data.bestDay ? dayLabel(data.bestDay.day, { year: "numeric" }) : "";
    renderAppStoreChart(data);
    $(`[data-marketing-app-store-countries-hint]`).textContent = shown.territoryCount
      ? `${fmtFull(shown.territoryCount)} countr${shown.territoryCount === 1 ? "y" : "ies"} · ${rangeLabel}` : "";
    $(`[data-marketing-app-store-countries]`).innerHTML = appStoreShareRows(shown.territories || [], shown.downloads || 0, (row) => countryName(row.code));
    $(`[data-marketing-app-store-devices]`).innerHTML = appStoreShareRows(shown.devices || [], shown.downloads || 0, (row) => row.name);
    const reviews = data.reviews || [];
    $(`[data-marketing-app-store-reviews-hint]`).textContent = reviews.length ? `${reviews.length} most recent, from App Store Connect` : "";
    $(`[data-marketing-app-store-reviews]`).innerHTML = reviews.length ? reviews.map((review) => `<li><span><span class="mk-stars" aria-label="${review.rating} of 5 stars">${starText(review.rating)}</span> <b>${escapeHtml(review.title || "")}</b></span>${review.body ? `<span>${escapeHtml(review.body)}</span>` : ""}<small>${escapeHtml([review.territory, review.createdAt ? new Date(review.createdAt).toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" }) : ""].filter(Boolean).join(" · "))}</small></li>`).join("")
      : `<li class="mk-hint">No reviews yet.</li>`;
    const url = [lookup.url, channelSettings("app-store").profileUrl].find((value) => String(value || "").startsWith("https://"));
    $(`[data-marketing-app-store-open]`).innerHTML = url ? `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer" class="mk-link">Open in the App Store ↗</a>` : "";
    const yearDate = (ts) => ts ? new Date(ts).toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" }) : "—";
    $(`[data-marketing-app-store-listing]`).innerHTML = lookup.lookedUpAt ? [
      factCell("Version", escapeHtml(lookup.version || "—")),
      factCell("Released", escapeHtml(yearDate(lookup.releasedAt))),
      factCell("This version", lookup.versionRating ? `${lookup.versionRating.toFixed(1)}★` : "—", lookup.versionRatings ? ` <small>${escapeHtml(fmtFull(lookup.versionRatings))}</small>` : ""),
      factCell("Genre", escapeHtml(lookup.genre || "—")),
      factCell("Price", escapeHtml(lookup.price || "—")),
      factCell("Minimum iOS", escapeHtml(lookup.minimumOs || "—")),
      factCell("First released", escapeHtml(yearDate(lookup.firstReleasedAt))),
      factCell("Seller", escapeHtml(lookup.seller || "—")),
      factCell("Read", escapeHtml(relativeTimeLabel(lookup.lookedUpAt))),
    ].join("") : `<p class="mk-hint">Press Refresh on the App Store channel to read the store listing.</p>`;
    const versions = data.versions || [];
    $(`[data-marketing-app-store-versions]`).innerHTML = versions.length ? versions.map((row) => {
      const tone = row.status?.tone === "good" ? "up" : row.status?.tone === "action" ? "down" : "";
      return `<tr><td class="mk-num">${escapeHtml(row.version)}${row.build ? ` <span class="mk-hint">(${escapeHtml(row.build)})</span>` : ""}</td><td><span class="mk-chg ${tone}">${escapeHtml(row.status?.label || "")}</span></td><td class="r mk-hint">${escapeHtml(row.createdAt ? yearDate(row.createdAt) : "—")}</td></tr>`;
    }).join("") : `<tr><td colspan="3" class="mk-hint">Versions arrive with the App Store report.</td></tr>`;
  }

  const READY_STATUS = { posted: "Posted", skipped: "Skipped", replaced: "Replaced" };
  const READY_IMAGE = /^data:(image\/(png|jpeg|webp));base64,([A-Za-z0-9+/]+=*)$/;
  function shownReadyPost() { return state.marketing.readyPost?.post || state.marketing.readyPosts?.x?.post || null; }
  function canCloseReadyPost() { return ["owner", "admin", "maintainer", "member"].includes(selectedOrg()?.role || ""); }
  function readyPostBlock() {
    const org = state.marketing.organization, quick = state.marketing.readyPosts?.x || null;
    const current = state.marketing.readyPost;
    if (current?.org !== org || (!current.loading && !current.error && quick && (quick.post?.id || "") !== (current.post?.id || ""))) void loadReadyPost();
    const data = state.marketing.readyPost;
    const shown = data.loading || data.error ? quick : data;
    const failed = data.error ? `<small class="mk-hint">${escapeHtml(`Could not load the ready post: ${data.error}`)}</small>` : "";
    if (!shown) return `<div class="mk-mail mk-ready" data-marketing-ready-post><b>Ready to post</b>${failed || `<p class="mk-hint">Loading the ready post…</p>`}</div>`;
    const post = shown.post;
    if (!post) {
      const last = (shown.history || [])[0];
      const lastLine = last ? `<li><span><small>${escapeHtml(["Last: " + (READY_STATUS[last.status] || last.status), relativeTimeLabel(last.closedAt || last.createdAt), last.closedBy].filter(Boolean).join(" · "))}</small><span title="${escapeHtml(last.text)}">${escapeHtml(last.text)}</span></span>${String(last.url || "").startsWith("https://") ? `<a href="${escapeHtml(last.url)}" target="_blank" rel="noopener noreferrer" class="mk-link">Open ↗</a>` : ""}</li>` : "";
      return `<div class="mk-mail mk-ready" data-marketing-ready-post><b>Ready to post</b>
        <p class="mk-hint">No post ready yet. Herald (the X special agent) writes the next one.</p>
        ${lastLine ? `<ul>${lastLine}</ul>` : ""}${failed}</div>`;
    }
    const image = READY_IMAGE.test(post.imageDataUrl || "") ? post.imageDataUrl : "";
    const extension = image ? READY_IMAGE.exec(image)[2].replace("jpeg", "jpg") : "";
    const length = Number(post.length || xLength(post.text));
    const by = post.model || post.author;
    const picture = image
      ? `<img src="${escapeHtml(image)}" alt="${escapeHtml(post.imageAlt || "")}">`
      : post.hasImage && !failed ? `<p class="mk-hint">Loading the image…</p>` : "";
    const actions = [`<button data-marketing-ready-copy type="button" class="fm-btn fm-btn-sm">Copy text</button>`,
      image ? `<a href="${escapeHtml(image)}" download="${escapeHtml(`x-post-${post.id.slice(0, 8)}.${extension}`)}" class="fm-btn fm-btn-sm">Download image</a>` : "",
      String(post.intentUrl || "").startsWith("https://") ? `<button data-marketing-ready-share type="button" class="fm-btn fm-btn-sm" title="Share the image and text to the X app, or open the X composer with the text filled in">Post on X ↗</button>` : "",
      canCloseReadyPost() ? `<button data-marketing-ready-close="posted" type="button" class="fm-btn fm-btn-sm">Mark posted</button><button data-marketing-ready-close="skipped" type="button" class="mk-link">Skip</button>` : ""];
    return `<div class="mk-mail mk-ready" data-marketing-ready-post><b>Ready to post</b>
      ${picture}
      <p class="mk-ready-text">${escapeHtml(post.text)}</p>
      <small class="mk-ready-meta"><span class="mk-num${length > X_LIMIT ? " over" : ""}">${escapeHtml(`${length}/${X_LIMIT}`)}</span>${post.createdAt ? escapeHtml(` · Written ${relativeTimeLabel(post.createdAt)}${by ? ` by ${by}` : ""}`) : ""}</small>
      ${post.why ? `<small class="mk-hint">${escapeHtml(post.why)}</small>` : ""}
      <div class="mk-ready-actions">${actions.join("")}</div>${failed}</div>`;
  }
  function setReadyPost(org, readyPost) {
    state.marketing.readyPost = { org, ...readyPost };
    if (org !== state.marketing.organization) return;
    state.marketing.readyPosts = { ...state.marketing.readyPosts, x: readyPost };
    if (state.view.page === "channels") renderChannelHealth();
  }
  async function loadReadyPost() {
    const org = state.marketing.organization;
    if (state.marketing.readyPost?.org === org && state.marketing.readyPost.loading) return;
    state.marketing.readyPost = { org, loading: true };
    try { const payload = await request(orgPath("/ready-post?channel=x")); setReadyPost(org, payload.readyPost); }
    catch (error) { state.marketing.readyPost = { org, error: error.message };
      if (org === state.marketing.organization && state.view.page === "channels") renderChannelHealth(); }
  }
  function readyPostFile(post) {
    const match = READY_IMAGE.exec(post.imageDataUrl || ""); if (!match) return null;
    const bytes = Uint8Array.from(atob(match[3]), (char) => char.charCodeAt(0));
    return new File([bytes], `x-post-${post.id.slice(0, 8)}.${match[2].replace("jpeg", "jpg")}`, { type: match[1] });
  }
  function shareReadyPost() {
    const post = shownReadyPost(); if (!post) return;
    const file = readyPostFile(post);
    if (file && window.matchMedia("(pointer: coarse)").matches && navigator.canShare && navigator.canShare({ files: [file] })) {
      navigator.share({ files: [file], text: post.text }).catch((error) => {
        if (error.name !== "AbortError") message("error", `Could not open the share sheet: ${error.message}. Copy the text and download the image instead.`);
      });
      return;
    }
    window.open(post.intentUrl, "_blank", "noopener,noreferrer");
  }
  async function closeReadyPost(status, button) {
    const org = state.marketing.organization, post = shownReadyPost(); if (!post) return;
    button.disabled = true;
    try { const payload = await request(orgPath("/ready-post"), "PATCH", { channel: "x", id: post.id, status });
      setReadyPost(org, payload.readyPost);
      message("notice", status === "posted" ? "Marked as posted. Herald is writing the next post." : "Skipped. Herald is writing the next post."); }
    catch (error) {
      if (error.status === 409 && error.payload?.readyPost) { setReadyPost(org, error.payload.readyPost); message("error", "The ready post changed while you were looking; this is the new one."); }
      else { button.disabled = false; message("error", `Could not update the ready post: ${error.message}`); }
    }
  }

  const BRAND = { x: "#14171a", youtube: "#e62117", reddit: "#ff4500", linkedin: "#0a66c2", facebook: "#1877f2",
    instagram: "#d62976", mastodon: "#6364ff", discord: "#5865f2", github: "#24292f", bluesky: "#1185fe",
    tiktok: "#111111", threads: "#111111", hackernews: "#ff6600", producthunt: "#da552f", telegram: "#229ed9",
    whatsapp: "#25d366", pinterest: "#e60023", twitch: "#9146ff", medium: "#111111", substack: "#ff6719",
    devto: "#0a0a0a", stackoverflow: "#f48024", snapchat: "#e6c800", spotify: "#1db954" };
  const CAMPAIGN_STATUSES = [["idea", "Idea"], ["planned", "Planned"], ["active", "Active"], ["paused", "Paused"],
    ["completed", "Completed"], ["archived", "Archived"]];
  const CAMPAIGN_OBJECTIVES = [["awareness", "Awareness"], ["traffic", "Traffic"], ["signups", "Signups"],
    ["activation", "Activation"], ["engagement", "Engagement"], ["launch", "Launch"], ["community", "Community"],
    ["sales", "Sales"]];
  const CAMPAIGN_TABS = [["open", "Open"], ["completed", "Done"], ["all", "All"]];
  const IDEA_ICONS = { content: STATUS_ICONS.draft, task: CHECK_ICON,
    keyword: svgIcon('<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>'),
    campaign: svgIcon('<path d="M3 11v2a1 1 0 0 0 1 1h2l5 4V6L6 10H4a1 1 0 0 0-1 1Z"/><path d="M15 9a3 3 0 0 1 0 6M18 6a7 7 0 0 1 0 12"/>') };
  const X_LIMIT = 280;

  const glyphColor = (channel) => BRAND[channel] || "var(--mk-s1)";
  const fmtDuration = (ms) => { const seconds = Math.round(Number(ms || 0) / 1000); return `${Math.floor(seconds / 60)}m ${String(seconds % 60).padStart(2, "0")}s`; };
  const campaignById = (id) => (state.marketing.campaigns || []).find((item) => item.id === id);
  const keywordById = (id) => (state.marketing.keywords || []).find((item) => item.id === id);
  const xState = () => state.marketing.settings.x || {};
  const trafficReady = () => state.marketing.traffic?.status === "ok";

  function kpi(key, value, delta = null, note = null) {
    const node = $(`[data-marketing-kpi="${key}"]`); if (node) node.textContent = value;
    const deltaNode = $(`[data-marketing-kpi-delta="${key}"]`);
    if (deltaNode && delta) { deltaNode.className = `mk-delta mk-num ${delta[0]}`; deltaNode.textContent = delta[1]; }
    const noteNode = $(`[data-marketing-kpi-note="${key}"]`); if (noteNode && note !== null) noteNode.textContent = note;
  }
  function changeDelta(current, previous) {
    if (missingValue(current)) return ["", "—"];
    if (!previous) return ["", current ? "New" : "—"];
    const change = (current - previous) / previous * 100;
    return [change > 0.05 ? "up" : change < -0.05 ? "down" : "", `${change > 0.05 ? "▲" : change < -0.05 ? "▼" : "•"} ${Math.abs(change).toFixed(1)}%`];
  }
  function missingValue(value) { return value === null || value === undefined; }

  async function loadTraffic() {
    if (!state.marketing.installed) return;
    const to = Date.now(), from = to - state.view.rangeDays * DAY;
    try { const payload = await request(`${orgPath("/performance")}?${new URLSearchParams({ from: String(from), to: String(to) })}`);
      state.marketing.traffic = payload.performance || null; }
    catch (error) { state.marketing.traffic = { status: "error", insights: [{ title: `Could not read Analytics: ${error.message}`, detail: "" }] }; }
    renderTrafficKpis(); renderTraffic(); renderLanding(); renderKeywords(); renderCampaigns();
  }

  function renderTrafficKpis() {
    const perf = state.marketing.traffic;
    $(`[data-marketing-kpi-label="visits"]`).textContent = `Visits · ${state.view.rangeDays}D`;
    if (!perf || perf.status !== "ok") {
      kpi("visits", "—", ["", "—"], perf ? "Analytics not set up" : "loading…");
      kpi("conversions", "—", ["", "—"], perf ? "" : "");
      return;
    }
    const totals = perf.totals || {}, previous = perf.previous || {}, goals = (perf.goals || []).length;
    kpi("visits", fmtCompact(totals.sessions), changeDelta(totals.sessions, previous.sessions), `${fmtCompact(totals.visitors)} visitors`);
    kpi("conversions", goals ? fmtFull(totals.conversions) : "—", goals ? changeDelta(totals.conversions, previous.conversions) : ["", "—"],
      goals ? `${fmtShare(totals.conversionRate)} of visits` : "Set a goal on Website");
  }

  function renderTraffic() {
    const host = $(`[data-marketing-traffic]`); if (!host) return;
    const perf = state.marketing.traffic, setup = $(`[data-marketing-traffic-setup]`), total = $(`[data-marketing-traffic-total]`);
    if (!perf || perf.status !== "ok") {
      host.innerHTML = ""; total.innerHTML = "";
      setup.hidden = !perf;
      setup.innerHTML = perf ? `${INFO_ICON}<div class="fm-flash-body">${(perf.insights || []).map((item) => `<span class="fm-flash-title">${escapeHtml(item.title)}</span>${item.detail ? `<p>${escapeHtml(item.detail)}</p>` : ""}`).join("")}</div><div class="fm-flash-actions"><a href="/dashboard/analytics" class="fm-btn fm-btn-sm">Open Analytics</a></div>` : "";
      $(`[data-marketing-traffic-hint]`).textContent = perf ? "" : "Loading Analytics…";
      return;
    }
    setup.hidden = true;
    const rows = perf.channels || [], goals = (perf.goals || []).length, totals = perf.totals || {};
    const sum = rows.reduce((value, row) => value + row.sessions, 0) || 1;
    const previousSum = rows.reduce((value, row) => value + row.previousSessions, 0) || 1;
    $(`[data-marketing-traffic-hint]`).textContent = `${perf.project?.name || "Analytics"} · ${shortDate(perf.from)} – ${shortDate(perf.to)}`;
    const SWATCH = { organic: "--mk-s1", paid: "--mk-s2", social: "--mk-s3", referral: "--mk-s4" };
    host.innerHTML = rows.length ? rows.map((row) => {
      const share = row.sessions / sum * 100, before = row.previousSessions / previousSum * 100;
      const change = row.sessions - row.previousSessions;
      const sources = (row.topSources || []).map((item) => `${item.value} ${fmtFull(item.count)}`).join(" · ");
      return `<tr title="${escapeHtml(sources)}">
        <td><span class="mk-dot" style="background:var(${SWATCH[row.channel] || "--mk-muted"})"></span>${escapeHtml(row.label)}</td>
        <td><div class="mk-share"><span class="mk-track"><span class="mk-bar" style="width:${Math.max(share ? 2 : 0, share)}%;background:var(${SWATCH[row.channel] || "--mk-muted"})"></span>${row.previousSessions ? `<span class="mk-prev" style="left:${Math.min(99, before)}%" title="Previous period ${before.toFixed(1)}%"></span>` : ""}</span><span class="mk-num mk-hint">${share.toFixed(1)}%</span></div></td>
        <td class="r mk-num">${fmtFull(row.sessions)}</td>
        <td class="r mk-num"><span class="mk-chg ${change > 0 ? "up" : change < 0 ? "down" : ""}">${row.previousSessions ? `${change > 0 ? "+" : ""}${fmtFull(change)}` : "new"}</span></td>
        <td class="r mk-num">${goals ? fmtShare(row.conversionRate) : "—"}</td>
        <td class="r mk-num">${fmtDuration(row.averageSessionMs)}</td>
        <td class="r mk-num">${fmtShare(row.bounceRate)}</td>
        <td class="r mk-num">${Number(row.pagesPerSession || 0).toFixed(2)}</td></tr>`;
    }).join("") : `<tr><td colspan="8" class="mk-hint">No visits in this window yet.</td></tr>`;
    const pages = totals.sessions ? totals.pageviews / totals.sessions : 0;
    total.innerHTML = rows.length ? `<tr><td>Total</td><td class="mk-hint">${fmtShare(totals.taggedShare)} tagged</td><td class="r mk-num">${fmtFull(totals.sessions)}</td><td class="r mk-num">${(() => { const change = totals.sessions - (perf.previous?.sessions || 0); return perf.previous?.sessions ? `${change > 0 ? "+" : ""}${fmtFull(change)}` : "—"; })()}</td><td class="r mk-num">${goals ? fmtShare(totals.conversionRate) : "—"}</td><td class="r mk-num">${fmtDuration(totals.averageSessionMs)}</td><td class="r mk-num">${fmtShare(totals.bounceRate)}</td><td class="r mk-num">${pages.toFixed(2)}</td></tr>` : "";
  }

  function renderLanding() {
    const host = $(`[data-marketing-landing]`); if (!host) return;
    const perf = state.marketing.traffic;
    if (!trafficReady()) { host.innerHTML = `<p class="mk-hint">${perf ? "Landing pages appear once Analytics records visits." : "Loading…"}</p>`; return; }
    const rows = (perf.landing || []).slice(0, 8), max = Math.max(1, ...rows.map((row) => row.sessions));
    host.innerHTML = rows.length ? rows.map((row) => `<div class="mk-bullet" title="${escapeHtml(`${row.landing}: ${fmtFull(row.sessions)} visits, ${fmtShare(row.bounceRate)} bounce`)}"><span>${escapeHtml(row.landing)}</span><span class="mk-track"><span class="mk-bar" style="width:${row.sessions / max * 100}%"></span></span><b>${fmtCompact(row.sessions)}</b></div>`).join("")
      : `<p class="mk-hint">No landing pages in this window.</p>`;
  }

  function renderReferrers() {
    const host = $(`[data-marketing-referrers]`), panel = $(`[data-marketing-referrers-panel]`); if (!host || !panel) return;
    const data = state.marketing.referringSites;
    panel.hidden = data?.status === "other_site";
    const hint = $(`[data-marketing-referrers-hint]`);
    if (!data || data.status !== "ok") {
      hint.textContent = "";
      host.innerHTML = `<p class="mk-hint">${data ? "Referring websites could not be read just now." : "Loading…"}</p>`;
      return;
    }
    hint.textContent = `${fmtFull(data.visits)} visits · ${fmtFull(data.sites)} sites · all time`;
    const rows = data.board || [], max = Math.max(1, ...rows.map((row) => row.visits));
    host.innerHTML = rows.length ? rows.map((row) => {
      const label = row.url ? `<a href="${escapeHtml(row.url)}" target="_blank" rel="noopener noreferrer">${escapeHtml(row.host)}</a>` : escapeHtml(row.host);
      return `<div class="mk-bullet" title="${escapeHtml(`${row.host}: ${fmtFull(row.visits)} visits, last ${row.lastAt ? relativeTimeLabel(row.lastAt) : "unknown"}`)}"><span>${label}</span><span class="mk-track"><span class="mk-bar" style="width:${row.visits / max * 100}%"></span></span><b>${fmtCompact(row.visits)}</b></div>`;
    }).join("") + `<p class="mk-hint">Page loads that another website linked to ${escapeHtml(data.host)}, from the browser's Referer header.</p>`
      : `<p class="mk-hint">No other website has sent a visitor to ${escapeHtml(data.host)} yet.</p>`;
  }

  function renderKeywords() {
    const host = $(`[data-marketing-keywords]`); if (!host) return;
    const view = state.view.keywordView, empty = $(`[data-marketing-keywords-empty]`);
    $$(`[data-marketing-keyword-view]`).forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.marketingKeywordView === view)));
    const measured = trafficReady() ? state.marketing.traffic.keywords || [] : [];
    const visitsFor = new Map(measured.map((row) => [row.term, row]));
    if (view === "measured") {
      $(`[data-marketing-keywords-head]`).innerHTML = `<tr><th>Term</th><th class="r">Visits</th><th class="r">Bounce</th><th class="r"><span class="sr-only">Track</span></th></tr>`;
      const tracked = new Set((state.marketing.keywords || []).map((item) => item.term));
      host.innerHTML = measured.map((row) => `<tr><td title="${escapeHtml((row.topSources || []).map((item) => item.value).join(", "))}">${escapeHtml(row.term)}</td><td class="r mk-num">${fmtFull(row.sessions)}</td><td class="r mk-num">${fmtShare(row.bounceRate)}</td><td class="r">${tracked.has(row.term) || row.term === "(not provided)" ? "" : `<button data-marketing-keyword-track="${escapeHtml(row.term)}" type="button" class="mk-link">Track</button>`}</td></tr>`).join("");
      empty.hidden = measured.length > 0;
      empty.innerHTML = blankslateBody("search", "No search terms", trafficReady() ? "No visits carried a search term (utm_term) in this window. Add {keyword} or utm_term to paid search and newsletter links." : "Search terms come from Analytics once it records visits.");
      return;
    }
    $(`[data-marketing-keywords-head]`).innerHTML = `<tr><th>Keyword</th><th class="r">Pos.</th><th class="r">Impr.</th><th class="r">Clicks</th><th class="r">Visits</th></tr>`;
    const items = state.marketing.keywords || [];
    host.innerHTML = items.map((item) => {
      const change = item.positionChange;
      const arrow = missingValue(change) || !change ? "" : `<small class="mk-chg ${change > 0 ? "up" : "down"}">${change > 0 ? "▲" : "▼"}${Math.abs(change)}</small>`;
      const visits = visitsFor.get(item.term)?.sessions;
      const stars = item.priority ? `<span class="mk-warn" title="Priority">${"★".repeat(item.priority)}</span> ` : "";
      return `<tr><td><button data-marketing-keyword-edit="${escapeHtml(item.id)}" type="button" class="mk-content-title" title="${escapeHtml(item.observedAt ? `Read ${shortDate(item.observedAt)}${item.source ? ` from ${item.source}` : ""}` : "No reading yet")}"><b>${stars}${escapeHtml(item.term)}</b>${item.campaign ? `<small>${escapeHtml(item.campaign)}</small>` : ""}</button></td>
        <td class="r mk-num">${missingValue(item.position) ? "—" : item.position.toFixed(1)} ${arrow}</td>
        <td class="r mk-num">${missingValue(item.impressions) ? "—" : fmtCompact(item.impressions)}</td>
        <td class="r mk-num" title="${missingValue(item.ctr) ? "" : `CTR ${fmtShare(item.ctr)}`}">${missingValue(item.clicks) ? "—" : fmtCompact(item.clicks)}</td>
        <td class="r mk-num">${missingValue(visits) ? "—" : fmtCompact(visits)}</td></tr>`;
    }).join("");
    empty.hidden = items.length > 0;
    empty.innerHTML = blankslateBody("search", "No keywords tracked", "Track the searches you want to win.", `<button data-marketing-keyword-import type="button" class="fm-btn fm-btn-sm">Import from Search Console</button>`);
  }

  function renderChannelGrid() {
    const host = $(`[data-marketing-channel-grid]`); if (!host) return;
    const rows = (state.marketing.summary.channels || []).map((item) => ({ item, settings: channelSettings(item.channel), health: item.health || {} }));
    rows.sort((a, b) => (a.health.state === "paused") - (b.health.state === "paused")
      || (b.item.stars || b.item.followers || b.item.views || b.item.visits || b.item.reach || 0) - (a.item.stars || a.item.followers || a.item.views || a.item.visits || a.item.reach || 0));
    const counts = state.marketing.summary.health || {};
    $(`[data-marketing-channel-hint]`).textContent = `${rows.filter((row) => row.settings.enabled !== false).length} active · ${fmtFull(counts.good || 0)} on track · ${fmtFull((counts.attention || 0) + (counts.stale || 0))} need a look`;
    const series = seriesByChannel();
    host.innerHTML = rows.map(({ item, settings, health }) => {
      const name = settings.displayName || channelLabel(item.channel);
      const list = series.get(item.channel) || [], latest = list[list.length - 1];
      const social = latest?.source === "x-profile"
        ? [["posts", "posts"], ["following", "following"], ["mentions", "mentions"]]
          .filter(([key]) => latest[key]).map(([key, label]) => `<span><b>${fmtCompact(latest[key])}</b> ${label}</span>`).join("") : "";
      const email = item.channel === "email";
      const stars = item.channel === "github" && item.stars > 0;
      const [value, label] = email ? [item.outbound || 0, "Emails sent · 30D"]
        : stars ? [item.stars, "Stars"]
        : item.followers ? [item.followers, item.channel === "youtube" ? "Subscribers" : "Followers"]
        : item.views ? [item.views, "Views"] : item.visits ? [item.visits, "Visits · 30D"]
          : item.indexedPages ? [item.indexedPages, "Pages on Google"] : item.reach ? [item.reach, "Reach"]
          : [health.postsLast30d || 0, "Posts · 30D"];
      const change = stars ? health.starsChange : health.followersChange;
      const changeText = missingValue(change) || !change ? "" : `<small class="${change > 0 ? "up" : "down"}">${change > 0 ? "+" : "−"}${fmtCompact(Math.abs(change))}</small>`;
      const facts = [
        health.engagementRate ? `<span title="${escapeHtml(engagementExplained(health))}"><b>${health.engagementRate.toFixed(1)}%</b> eng.</span>` : "",
        email ? `<span><b>${fmtFull(item.closedDeals || 0)}</b> closed deals</span>` : `<span><b>${fmtFull(health.postsLast30d || 0)}</b> posts/30D</span>`,
        email && item.closedValue ? `<span><b>${fmtCompact(item.closedValue)}</b> won</span>` : "",
        email && item.replies ? `<span><b>${fmtCompact(item.replies)}</b> replies</span>` : "",
        stars && item.followers ? `<span><b>${fmtCompact(item.followers)}</b> followers</span>` : "",
        item.forks ? `<span><b>${fmtCompact(item.forks)}</b> forks</span>` : "",
        item.engagements ? `<span><b>${fmtCompact(item.engagements)}</b> engaged</span>` : "",
        item.indexedPages && item.visits ? `<span><b>${fmtCompact(item.indexedPages)}</b> pages on Google</span>` : "",
        item.rating ? `<span><b>${item.rating.toFixed(1)}★</b></span>` : "",
        health.lastPostAt ? `<span>last ${escapeHtml(relativeLabel(health.lastPostAt))}</span>` : "",
        social,
      ].filter(Boolean).join("");
      const failing = (health.checks || []).filter((check) => !check.ok).map((check) => check.label).join("; ");
      const open = openLink(settings);
      return `<article class="mk-tile${health.state === "paused" ? " paused" : ""}" data-marketing-channel-card="${escapeHtml(item.channel)}">
        <span class="mk-state ${escapeHtml(health.state || "")}" title="${escapeHtml(`${HEALTH_LABELS[health.state] || "—"}${failing ? `: ${failing}` : ""}`)}"></span>
        <span class="mk-glyph" style="background:${glyphColor(item.channel)}">${escapeHtml(initials(name))}</span>
        <span class="mk-tile-value" title="${escapeHtml(fmtFull(value))}">${fmtCompact(value)}${changeText}</span>
        <span class="mk-tile-label"><b>${escapeHtml(name)}</b>· ${escapeHtml(label)}</span>
        <span class="mk-tile-facts">${facts}</span>
        <span class="mk-tile-actions">${open}${email ? `<button data-marketing-channel-refresh="email" type="button" class="mk-link">Refresh</button><a href="${escapeHtml(crmPath())}" class="mk-link">CRM</a>` : ""}<button data-marketing-channel-log="${escapeHtml(item.channel)}" type="button" class="mk-link">Log</button>${captureButton(item.channel)}${captureNote(item.channel)}</span>
      </article>`;
    }).join("") + `<button data-marketing-catalog-open type="button" class="mk-tile-add"><svg class="mk-icon" viewBox="0 0 24 24" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>Add a channel<small class="mk-hint">${fmtFull((state.marketing.channelCatalog || []).length || 100)} in the catalog</small></button>`;
  }

  function campaignResults(campaign) {
    if (!trafficReady() || !campaign.slug) return null;
    const rows = (state.marketing.traffic.campaigns || []).filter((row) => row.campaign === campaign.slug);
    const goals = (state.marketing.traffic.goals || []).length;
    return { sessions: rows.reduce((sum, row) => sum + row.sessions, 0),
      previous: rows.reduce((sum, row) => sum + row.previousSessions, 0),
      conversions: goals ? rows.reduce((sum, row) => sum + (row.conversions || 0), 0) : null };
  }
  function bullet(label, value, target, format = fmtFull) {
    if (missingValue(target) && missingValue(value)) return "";
    const ratio = target ? Math.min(1, (value || 0) / target) : 0;
    return `<div class="mk-bullet"><span>${escapeHtml(label)}</span><span class="mk-track">${target ? `<span class="mk-bar${ratio >= 1 ? " met" : ""}" style="width:${ratio * 100}%"></span><span class="mk-target" title="Target ${escapeHtml(format(target))}"></span>` : ""}</span><b>${missingValue(value) ? "—" : escapeHtml(format(value))}${target ? `<span class="mk-hint"> / ${escapeHtml(format(target))}</span>` : ""}</b></div>`;
  }
  function campaignDates(item) {
    if (!item.startsAt && !item.endsAt) return "No dates";
    const range = `${item.startsAt ? shortDate(item.startsAt) : "…"} – ${item.endsAt ? shortDate(item.endsAt) : "…"}`;
    const left = item.stats?.daysLeft;
    if (item.status === "active" && left) return `${range} · ${left} day${left === 1 ? "" : "s"} left`;
    if (item.startsAt > Date.now()) return `${range} · starts ${relativeLabel(item.startsAt)}`;
    return range;
  }
  function campaignCard(item) {
    const stats = item.stats || {}, results = campaignResults(item);
    const status = labelOf(CAMPAIGN_STATUSES, item.status);
    const spend = (stats.spend || []).map((entry) => fmtMoney(entry.cents, entry.currency)).join(" · ");
    const budget = item.budgetCents ? fmtMoney(item.budgetCents, item.currency) : "";
    const bullets = [
      bullet(`Visits · ${state.view.rangeDays}D`, results ? results.sessions : null, item.targetVisits),
      bullet("Conversions", results ? results.conversions : null, item.targetConversions),
      bullet("Published", stats.published || 0, stats.content || null),
      item.tasks?.length ? bullet("Checklist", stats.tasksDone || 0, stats.tasksTotal || null) : "",
    ].join("");
    const uncovered = new Set(stats.uncoveredChannels || []);
    const chips = (item.channels || []).map((channel) => `<span class="mk-chip${uncovered.has(channel) ? " missing" : ""}" title="${uncovered.has(channel) ? "No content planned on this channel yet" : "Content planned"}"><i style="background:${glyphColor(channel)}"></i>${escapeHtml(channelLabel(channel))}</span>`).join("");
    const openTasks = (item.tasks || []).map((task, index) => [task, index]).filter(([task]) => !task.done).slice(0, 3);
    const tasks = openTasks.length ? `<ul class="mk-tasks">${openTasks.map(([task, index]) => `<li><label><input data-marketing-campaign-task="${escapeHtml(item.id)}" data-index="${index}" type="checkbox" /><span>${escapeHtml(task.title)}</span></label></li>`).join("")}</ul>` : "";
    const next = stats.nextScheduledAt ? `<p>Next: <b>${escapeHtml(stats.nextTitle)}</b> · ${escapeHtml(relativeLabel(stats.nextScheduledAt))}</p>` : "";
    const line = [spend ? `Spent ${spend}` : "", budget ? `budget ${budget}` : "", stats.ads ? `${stats.ads} ad${stats.ads === 1 ? "" : "s"}` : "", stats.posts ? `${stats.posts} posted link${stats.posts === 1 ? "" : "s"} · ${fmtFull(stats.postEngagements || 0)} engagements` : ""].filter(Boolean).join(" · ");
    const progress = missingValue(stats.timeProgress) ? "" : `<div class="mk-timeline" title="${Math.round(stats.timeProgress * 100)}% of the flight"><span style="width:${stats.timeProgress * 100}%;background:${item.status === "active" ? "var(--mk-good)" : "var(--mk-muted)"}"></span></div>`;
    return `<article class="mk-campaign" data-marketing-campaign="${escapeHtml(item.id)}">
      <div class="mk-campaign-head"><div><b title="${escapeHtml(item.name)}">${escapeHtml(item.name)}</b><small>${escapeHtml(campaignDates(item))}${item.owner ? ` · ${escapeHtml(item.owner)}` : ""}</small></div><span class="fm-label mk-pill ${escapeHtml(item.status)}">${escapeHtml(status)}</span></div>
      ${progress}
      ${item.message ? `<p title="${escapeHtml(item.message)}">${escapeHtml(item.message)}</p>` : ""}
      <div class="mk-bullets">${bullets}</div>
      ${chips ? `<div class="mk-chips">${chips}</div>` : ""}
      ${next}${tasks}
      ${line ? `<p class="mk-num">${escapeHtml(line)}</p>` : ""}
      <div class="mk-campaign-foot">
        <button data-marketing-campaign-edit="${escapeHtml(item.id)}" type="button" class="mk-link">Edit</button>
        <button data-marketing-campaign-draft="${escapeHtml(item.id)}" type="button" class="mk-link">Draft post</button>
        ${state.marketing.assistantAvailable ? `<button data-marketing-campaign-ask="${escapeHtml(item.id)}" type="button" class="mk-link">Plan with agent</button>` : ""}
        <button data-marketing-campaign-handoff="${escapeHtml(item.id)}" type="button" class="mk-link">Hand off</button>
        <button data-marketing-campaign-brief="${escapeHtml(item.id)}" type="button" class="mk-link">Copy brief</button>
        ${stats.taggedLink ? `<button data-marketing-campaign-link="${escapeHtml(item.id)}" type="button" class="mk-link">Copy link</button>` : ""}
      </div>
    </article>`;
  }
  function renderCampaigns() {
    const host = $(`[data-marketing-campaigns]`); if (!host) return;
    const campaigns = state.marketing.campaigns || [], tab = state.view.campaignStatus;
    const counts = { open: campaigns.filter((item) => ["idea", "planned", "active", "paused"].includes(item.status)).length,
      completed: campaigns.filter((item) => item.status === "completed").length, all: campaigns.length };
    $(`[data-marketing-campaign-tabs]`).innerHTML = CAMPAIGN_TABS.map(([key, label]) => `<button data-marketing-campaign-tab="${key}" type="button" aria-pressed="${tab === key}">${label} ${counts[key]}</button>`).join("");
    const order = { active: 0, paused: 1, planned: 2, idea: 3, completed: 4, archived: 5 };
    const shown = campaigns.filter((item) => tab === "all" || (tab === "completed" ? item.status === "completed"
      : ["idea", "planned", "active", "paused"].includes(item.status)))
      .sort((a, b) => order[a.status] - order[b.status] || (a.endsAt || Infinity) - (b.endsAt || Infinity));
    const sources = new Set(campaigns.map((item) => item.source).filter(Boolean));
    const templates = (state.marketing.campaignTemplates || []).map((item, index) => ({ item, index })).filter(({ item }) => !sources.has(item.source));
    host.innerHTML = shown.map(campaignCard).join("") + templates.map(({ item, index }) => `<article class="mk-campaign ghost"><div class="mk-campaign-head"><div><b>${escapeHtml(item.name)}</b><small>${escapeHtml(item.label)} · ${escapeHtml(shortDate(item.startsAt))} – ${escapeHtml(shortDate(item.endsAt))}</small></div><span class="fm-label mk-pill">Template</span></div><p>${escapeHtml(item.message)}</p><p>${escapeHtml(item.offer)}</p><div class="mk-campaign-foot"><button data-marketing-campaign-template="${index}" type="button" class="fm-btn fm-btn-sm">${PLUS_ICON}Add this campaign</button><a href="${escapeHtml(item.source)}" target="_blank" rel="noopener noreferrer" class="mk-link">Open page ↗</a></div></article>`).join("")
      || blankslate("campaign", "No campaigns here", "Plan one audience, one message and a target.", `<button data-marketing-campaign-open type="button" class="fm-btn fm-btn-sm">New campaign</button>`, "grid-column:1/-1");
    const live = campaigns.filter((item) => item.status === "active");
    const ending = live.filter((item) => item.endsAt > Date.now()).sort((a, b) => a.endsAt - b.endsAt)[0];
    kpi("campaigns", fmtFull(live.length), null, ending ? `${ending.name} ends ${relativeLabel(ending.endsAt)}` : `${counts.open} open`);
    renderAssistantCampaigns();
  }

  function fillCampaignChannels(selected) {
    const configured = state.marketing.settings.channels || [];
    const ids = [...new Set([...configured.map((item) => item.id), ...selected])];
    $(`[data-marketing-campaign-channels]`).innerHTML = ids.map((id) => `<label><input type="checkbox" value="${escapeHtml(id)}" ${selected.includes(id) ? "checked" : ""} />${escapeHtml(channelLabel(id))}</label>`).join("");
  }
  function openCampaign(item = null, preset = null) {
    const source = item || preset || {};
    const field = (name) => $(`[data-marketing-campaign-field="${name}"]`);
    field("status").innerHTML = CAMPAIGN_STATUSES.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    field("objective").innerHTML = CAMPAIGN_OBJECTIVES.map(([id, label]) => `<option value="${id}">${label}</option>`).join("");
    $(`[data-marketing-campaign-id]`).value = item?.id || "";
    $(`[data-marketing-campaign-source]`).value = source.source || "";
    $(`[data-marketing-campaign-title]`).textContent = item ? "Edit campaign" : preset?.label ? `New campaign · ${preset.label}` : "New campaign";
    for (const key of ["name", "slug", "owner", "landingUrl", "audience", "message", "offer", "brief", "notes"]) field(key).value = source[key] || "";
    field("status").value = source.status || "planned";
    field("objective").value = source.objective || "awareness";
    field("startsAt").value = localDateTime(source.startsAt);
    field("endsAt").value = localDateTime(source.endsAt);
    field("hashtags").value = (source.hashtags || []).join(", ");
    field("keywords").value = (source.keywords || []).join(", ");
    field("budget").value = source.budgetCents ? (source.budgetCents / 100).toFixed(2) : "";
    field("currency").value = source.currency || "USD";
    for (const key of ["targetVisits", "targetConversions"]) field(key).value = missingValue(source[key]) ? "" : source[key];
    field("tasks").value = (source.tasks || []).map((task) => `${task.done ? "[x] " : ""}${task.title}`).join("\n");
    fillCampaignChannels(source.channels || []);
    $(`[data-marketing-campaign-delete]`).classList.toggle("hidden", !item);
    $(`[data-marketing-campaign-dialog]`).showModal();
  }
  function campaignPayload() {
    const field = (name) => $(`[data-marketing-campaign-field="${name}"]`);
    const time = (name) => field(name).value ? new Date(field(name).value).getTime() : 0;
    const count = (name) => field(name).value === "" ? null : Math.max(0, Math.round(Number(field(name).value)));
    return {
      name: field("name").value, slug: field("slug").value, status: field("status").value, objective: field("objective").value,
      owner: field("owner").value, landingUrl: field("landingUrl").value, audience: field("audience").value,
      message: field("message").value, offer: field("offer").value, brief: field("brief").value, notes: field("notes").value,
      startsAt: time("startsAt"), endsAt: time("endsAt"),
      hashtags: field("hashtags").value, keywords: field("keywords").value,
      budgetCents: Math.round(Number(field("budget").value || 0) * 100), currency: (field("currency").value || "USD").toUpperCase(),
      targetVisits: count("targetVisits"), targetConversions: count("targetConversions"),
      channels: $$(`[data-marketing-campaign-channels] input:checked`).map((input) => input.value),
      tasks: field("tasks").value.split("\n").map((line) => line.trim()).filter(Boolean)
        .map((line) => ({ done: /^\[x\]/i.test(line), title: line.replace(/^\[[x ]?\]\s*/i, "") })),
      source: $(`[data-marketing-campaign-source]`).value,
    };
  }
  function upsert(list, item) { const index = list.findIndex((entry) => entry.id === item.id); if (index >= 0) list[index] = item; else list.unshift(item); }
  async function saveCampaign(event) {
    event.preventDefault();
    const id = $(`[data-marketing-campaign-id]`).value;
    try { const payload = await request(orgPath(`/campaigns${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", campaignPayload());
      $(`[data-marketing-campaign-dialog]`).close(); message("notice", id ? "Campaign saved." : "Campaign created.");
      await load(); return payload; }
    catch (error) { message("error", `Could not save the campaign: ${error.message}`); }
  }
  async function deleteCampaign() {
    const id = $(`[data-marketing-campaign-id]`).value;
    if (!id || !confirm("Delete this campaign? Its content and ads stay.")) return;
    try { await request(orgPath(`/campaigns/${id}`), "DELETE"); $(`[data-marketing-campaign-dialog]`).close(); await load(); }
    catch (error) { message("error", `Could not delete the campaign: ${error.message}`); }
  }
  async function toggleCampaignTask(id, index, done) {
    const item = campaignById(id); if (!item) return;
    const tasks = item.tasks.map((task, position) => position === index ? { ...task, done } : task);
    try { const payload = await request(orgPath(`/campaigns/${id}`), "PATCH", { tasks });
      upsert(state.marketing.campaigns, payload.campaign); renderCampaigns(); }
    catch (error) { message("error", `Could not update the checklist: ${error.message}`); }
  }
  function draftForCampaign(item, channel = "", body = "", inDays = null, title = "") {
    openContent(null);
    $(`[data-marketing-content-campaign]`).value = item?.name || "";
    const target = channel || item?.channels?.[0] || "";
    if (target) { syncChannelControls(target); $(`[data-marketing-content-channel]`).value = target; }
    if (title) $(`[data-marketing-content-name]`).value = title;
    if (body) $(`[data-marketing-content-body]`).value = body;
    if (inDays !== null) { const when = new Date(startOfDay(Date.now()) + inDays * DAY); when.setHours(10); $(`[data-marketing-content-scheduled]`).value = localDateTime(when.getTime()); }
  }
  const cell = (value) => String(value || "—").replaceAll("|", "\\|").replaceAll("\n", " ");
  function campaignBrief(item) {
    const stats = item.stats || {}, results = campaignResults(item);
    const day = (ts) => ts ? new Date(ts).toISOString().slice(0, 10) : "not set";
    const targets = [item.targetVisits ? `${fmtFull(item.targetVisits)} visits` : "", item.targetConversions ? `${fmtFull(item.targetConversions)} conversions` : ""].filter(Boolean).join(", ");
    return [`# ${item.name}`, "",
      `**Owner:** ${item.owner || "not set"} · **Status:** ${item.status} · **Objective:** ${item.objective} · **Campaign tag:** ${item.slug || "none"}`,
      `**Dates:** ${day(item.startsAt)} to ${day(item.endsAt)}`, "",
      "## Brief", "", "| Field | Value |", "|---|---|",
      `| Audience | ${cell(item.audience)} |`, `| Message | ${cell(item.message)} |`, `| Offer | ${cell(item.offer)} |`,
      `| Channels | ${cell((item.channels || []).map(channelLabel).join(", "))} |`,
      `| Success measure | ${cell(targets)} |`, `| Landing page | ${cell(item.landingUrl)} |`,
      `| Hashtags | ${cell((item.hashtags || []).join(" "))} |`, `| Search keywords | ${cell((item.keywords || []).join(", "))} |`,
      `| Budget | ${cell(item.budgetCents ? fmtMoney(item.budgetCents, item.currency) : "")} |`, "",
      ...(stats.taggedLink ? ["## Tagged link", "", stats.taggedLink, ""] : []),
      "## Where it stands", "",
      `- Content: ${stats.content || 0} items, ${stats.published || 0} published${stats.nextTitle ? `; next “${stats.nextTitle}” on ${day(stats.nextScheduledAt)}` : "; nothing scheduled"}.`,
      `- Channels without content: ${(stats.uncoveredChannels || []).map(channelLabel).join(", ") || "none"}.`,
      `- Ads: ${stats.ads || 0}${(stats.spend || []).length ? `, spent ${(stats.spend || []).map((entry) => fmtMoney(entry.cents, entry.currency)).join(" + ")}` : ""}.`,
      results ? `- Analytics, last ${state.view.rangeDays} days: ${fmtFull(results.sessions)} visits${missingValue(results.conversions) ? "" : `, ${fmtFull(results.conversions)} conversions`}.` : "- Analytics: no measured visits yet.",
      "", "## Checklist", "", ...((item.tasks || []).length ? item.tasks.map((task) => `- [${task.done ? "x" : " "}] ${task.title}`) : ["- (empty)"]),
      ...(item.brief ? ["", "## Notes", "", item.brief] : [])].join("\n");
  }
  const CONTEXT_KEYS = [["followers", "Followers"], ["following", "Following"], ["reach", "Reach"], ["engagements", "Engagements"],
    ["views", "Views"], ["visits", "Visits"], ["posts", "Posts"], ["likes", "Likes"], ["replies", "Replies"], ["stars", "Stars"],
    ["forks", "Forks"], ["outbound", "Emails sent"], ["closedDeals", "Closed deals"], ["indexedPages", "Pages on Google"], ["reviews", "Reviews"]];
  const contextKey = (channel) => `forkmesh.marketing.aiContext.${state.marketing.organization}.${channel}`;
  function contextNumbers(item) {
    const values = Object.fromEntries(CONTEXT_KEYS.filter(([key]) => Number(item[key]) > 0).map(([key]) => [key, Number(item[key])]));
    return { ...values, postsLast30d: Number(item.health?.postsLast30d || 0) };
  }
  function channelContext(channel) {
    const item = (state.marketing.summary?.channels || []).find((entry) => entry.channel === channel) || { channel };
    const health = item.health || {}, settings = channelSettings(channel), catalog = catalogEntry(channel);
    const name = settings.displayName || catalog.name || channelLabel(channel), org = state.marketing.organization;
    const now = Date.now(), range = state.view.rangeDays, start = now - range * DAY, cadence = Math.max(1, health.cadenceDays || 1);
    const history = seriesByChannel().get(channel) || [], before = snapshotAt(history, start);
    const day = (ts) => ts ? new Date(ts).toISOString().slice(0, 10) : "never";
    const clip = (value, size) => { const flat = String(value || "").replace(/\s+/g, " ").trim(); return flat.length > size ? `${flat.slice(0, size - 1).trim()}…` : flat; };
    const count = (value) => typeof value === "number" && Number.isFinite(value) ? fmtFull(value) : "—";
    const shown = CONTEXT_KEYS.filter(([key]) => Number(item[key]) > 0 || history.some((snapshot) => Number(snapshot[key]) > 0));
    const main = shown[0]?.[1] || "Posts in the last 30 days";
    const lines = [`# Grow ${name} for ${org}`, "",
      `You are the marketing analyst for the ${name} channel of ${org}. Below is everything the ForkMesh Marketing dashboard knows about the channel on ${day(now)}. Work from these numbers, and say when they are too thin to support a claim.`,
      "", "## What to do", "",
      `1. In two sentences, say how the channel is doing against its goal and its aim of a post every ${cadence === 1 ? "day" : `${cadence} days`}.`,
      "2. Find what worked: match the posts and content below to the changes in the numbers after them.",
      `3. Give the three changes most likely to raise ${main} in the next 14 days. For each: what to do, the data behind it, the number to watch, and the result you expect by when.`,
      "4. Draft the next three posts for this channel, ready to publish.",
      "5. Learn from last time: if this chat already holds an earlier copy of this context, or \"Since the last copy\" below has numbers, check your earlier predictions against them, say which held and which did not, and change your advice to match.",
      "6. End with \"Notes for next time\": your predictions with the numbers to check, the data missing here that would change your advice, and a better version of this \"What to do\" list for the next copy.",
      "", "## Channel", "", "| Field | Value |", "|---|---|",
      `| Channel | ${cell(name)} |`, `| Category | ${cell(catalog.category)} |`,
      `| Link | ${cell([settings.openUrl, settings.profileUrl].find((value) => String(value || "").startsWith("https://")))} |`,
      `| About | ${cell(clip(settings.capture?.description, 400))} |`,
      `| Status | ${settings.enabled === false ? "Paused" : "Active"} |`, `| Health | ${cell(HEALTH_LABELS[health.state])} |`,
      `| Posting aim | every ${cadence} day${cadence === 1 ? "" : "s"} |`, `| Posts in the last 30 days | ${fmtFull(health.postsLast30d || 0)} |`,
      `| Last post | ${day(health.lastPostAt)} |`,
      `| Engagement rate | ${cell(typeof health.engagementRate === "number" ? `${health.engagementRate.toFixed(1)}% (${engagementExplained(health)})` : engagementExplained(health))} |`,
      `| Numbers last read | ${cell(health.observedAt ? `${day(health.observedAt)}, ${SOURCE_LABELS[health.source] ?? `via ${health.source}`}` : "never")} |`,
      "", "## Numbers", "",
      ...(shown.length ? [`| Metric | Now | ${range} days ago |`, "|---|---|---|",
        ...shown.map(([key, label]) => `| ${label} | ${count(item[key])} | ${before ? count(before[key]) : "—"} |`)] : ["No numbers recorded yet."]),
      "", "## Since the last copy", ""];
    let last = null;
    try { last = JSON.parse(localStorage.getItem(contextKey(channel)) || "null"); } catch { last = null; }
    if (!last?.at) lines.push("This is the first copy of this context, so there is nothing to compare yet.");
    else {
      const current = contextNumbers(item), then = last.values || {};
      lines.push(`Last copied on ${day(last.at)} (${relativeTimeLabel(last.at)}). Then → now:`);
      for (const [key, label] of [...CONTEXT_KEYS, ["postsLast30d", "Posts in the last 30 days"]]) {
        if (!(key in then) && !(key in current)) continue;
        const was = Number(then[key] || 0), is = Number(current[key] || 0);
        lines.push(`- ${label}: ${fmtFull(was)} → ${fmtFull(is)} (${is >= was ? "+" : "−"}${fmtFull(Math.abs(is - was))})`);
      }
    }
    const recent = history.filter((snapshot) => snapshot.observedAt > start).reverse().slice(0, 40);
    if (recent.length && shown.length) lines.push("", "## History, newest first", "",
      `| Date | ${shown.map(([, label]) => label).join(" | ")} | Source | Note |`, `|---|${shown.map(() => "---").join("|")}|---|---|`,
      ...recent.map((snapshot) => `| ${day(snapshot.observedAt)} | ${shown.map(([key]) => count(snapshot[key])).join(" | ")} | ${cell(SOURCE_LABELS[snapshot.source] ?? snapshot.source)} | ${cell(clip(snapshot.note, 160))} |`));
    const when = (entry) => entry.publishedAt || entry.scheduledAt || entry.updatedAt || 0;
    const content = (state.marketing.content || []).filter((entry) => entry.channel === channel && entry.status !== "archived")
      .sort((a, b) => when(b) - when(a)).slice(0, 25);
    if (content.length) lines.push("", "## Content for this channel, newest first", "", "| Date | Status | Title | Text |", "|---|---|---|---|",
      ...content.map((entry) => `| ${day(when(entry))} | ${cell(entry.status)} | ${cell(clip(entry.title, 120))} | ${cell(clip(entry.body, 280))} |`));
    const discord = state.marketing.discordPosts;
    if (channel === "discord" && discord?.org === org && Array.isArray(discord.posts)) {
      lines.push("", `## Discord posts in the last ${discord.days} days`, "",
        `${discord.posts.length} post${discord.posts.length === 1 ? "" : "s"} in ${(discord.channels || []).map((entry) => `#${entry}`).join(", ")}, read ${relativeTimeLabel(discord.readAt)}.`);
      if (discord.truncated) lines.push("Only the latest 50 posts of each channel were read.");
      if (discord.contentMissing) lines.push("The bot cannot read message text, so the posts below have no text.");
      lines.push("", ...discord.posts.slice(0, 60).map((post) => `- ${new Date(post.postedAt).toISOString().slice(0, 16).replace("T", " ")} · #${post.channel} · ${post.author}${post.bot ? " (bot)" : ""}: ${post.content ? clip(post.content, 300) : "(no text)"}`));
    }
    const ads = (state.marketing.ads || []).filter((ad) => ad.channel === channel);
    if (ads.length) lines.push("", "## Ads", "", ...ads.map((ad) => `- ${ad.name}: ${ad.status}, spent ${fmtMoney(ad.spendCents, ad.currency)}`));
    const campaigns = (state.marketing.campaigns || []).filter((campaign) => (campaign.channels || []).includes(channel) && campaign.status !== "archived");
    if (campaigns.length) lines.push("", "## Campaigns on this channel", "",
      ...campaigns.map((campaign) => `- ${campaign.name} (${campaign.status}, ${day(campaign.startsAt)} to ${day(campaign.endsAt)}): ${clip(campaign.message || campaign.objective, 240)}`));
    if ((health.checks || []).length) lines.push("", "## Health checks", "", ...health.checks.map((check) => `- [${check.ok ? "x" : " "}] ${check.label}`));
    const playbook = (state.marketing.channelPlaybooks || {})[catalog.category];
    if (playbook) lines.push("", `## Playbook for ${catalog.category} channels`, "", `- Goal: ${playbook.goal}`,
      ...((playbook.kpis || []).length ? [`- Track: ${playbook.kpis.join(", ")}`] : []), ...(playbook.tips || []).map((tip) => `- Tip: ${tip}`));
    return `${lines.join("\n")}\n`;
  }
  function copyChannelContext(channel) {
    const item = (state.marketing.summary?.channels || []).find((entry) => entry.channel === channel) || {};
    copyText(channelContext(channel), `Copied the AI context for ${channelLabel(channel)}. Paste it into Claude or another AI chat, and copy it again after you act on the advice so the AI can check what worked.`);
    try { localStorage.setItem(contextKey(channel), JSON.stringify({ at: Date.now(), values: contextNumbers(item) })); } catch {}
  }
  async function handOff(item = null) {
    const title = item ? `@Marketing: run the ${item.name} campaign` : "@Marketing: weekly marketing review";
    const details = item ? `${campaignBrief(item)}\n\nPlan the next deliverables for this campaign, draft the posts in the Marketing package's content pipeline (campaign "${item.name}"), and update the checklist. A person approves anything that posts or spends money.`
      : `Run the weekly marketing review for ${state.marketing.organization}: read the Marketing dashboard (channels, campaigns, keywords, content, ads) and Analytics, then plan the week.\n\nOpen campaigns:\n${(state.marketing.campaigns || []).filter((entry) => ["planned", "active"].includes(entry.status)).map((entry) => `- ${entry.name} (${entry.status}, ${campaignDates(entry)})`).join("\n") || "- none"}`;
    if (!confirm(`Put “${title}” on the organization's task board for the Marketing agent?`)) return;
    try { await request("/api/tasks", "POST", { title: title.slice(0, 200), details: details.slice(0, 12000), department: "marketing",
      assigneeKind: "agent", destination: "agent", repository: "forkmesh/forkmesh" });
      message("notice", "Task created for the Marketing agent. Follow it on the Tasks page."); }
    catch (error) { copyText(details, ""); message("error", `Could not create the task (${error.message}). The brief is on your clipboard to paste into a Marketing agent run.`); }
  }

  function renderSuggestions() {
    const host = $(`[data-marketing-suggestions]`); if (!host) return;
    const items = state.marketing.suggestions || [];
    const icon = { setup: STATUS_ICONS.draft, action: CLOCK_ICON, warning: WARN_ICON, win: CHECK_ICON, info: STATUS_ICONS.review };
    const verb = { campaign: "New", "campaign-edit": "Open", content: "Draft", channel: "View", keyword: "Add" };
    host.innerHTML = items.length ? items.map((item, index) => `<li data-level="${escapeHtml(item.level)}">${icon[item.level] || ""}<span>${escapeHtml(item.title)}${item.detail ? `<small>${escapeHtml(item.detail)}</small>` : ""}</span>${item.action?.type ? `<button data-marketing-suggestion="${index}" type="button" class="mk-link">${verb[item.action.type] || "Open"}</button>` : ""}</li>`).join("")
      : `<li data-level="win">${CHECK_ICON}<span>Nothing needs attention. Ask the agent for ideas.</span></li>`;
  }
  function runSuggestion(index) {
    runAction(state.marketing.suggestions?.[index]?.action || {});
  }
  function runAction(action = {}) {
    if (action.type === "campaign") openCampaign();
    else if (action.type === "campaign-edit") openCampaign(campaignById(action.id));
    else if (action.type === "content") draftForCampaign(state.marketing.campaigns.find((item) => item.name === action.campaign), action.channel);
    else if (action.type === "content-open") openContent(contentById(action.id));
    else if (action.type === "channel") showPage("channels");
    else if (action.type === "keyword") openKeyword();
    else if (action.type === "page") showPage(action.page);
    else if (action.type === "ad") openAd(adById(action.id));
    else if (action.type === "ad-new") openAdFor({ channel: action.channel || "" });
    else if (action.type === "experiment") openExperiment(experimentById(action.id));
    else if (action.type === "funnel-new") openFunnel();
    else if (action.type === "goals") { showPage("website"); openGoals(); }
    else if (action.type === "analytics") location.href = "/dashboard/analytics";
  }

  const ADVISOR_AREAS = { website: "Website", content: "Content", ads: "Ads", growth: "Growth",
    campaigns: "Campaigns", channels: "Channels", search: "Search", community: "Community" };
  const ADVISOR_VERBS = { campaign: "New", "campaign-edit": "Open", content: "Draft", "content-open": "Open",
    channel: "View", keyword: "Add", page: "Open", ad: "Open", "ad-new": "Plan ad", experiment: "Open",
    "funnel-new": "Log week", goals: "Goals", analytics: "Analytics" };
  const ADVISOR_DONE_DAYS = 7;
  function advisorDoneKey() { return `forkmesh.marketing.advisorDone.${state.marketing.organization}`; }
  function advisorDone() {
    try { const stored = JSON.parse(localStorage.getItem(advisorDoneKey()) || "{}");
      const fresh = Object.fromEntries(Object.entries(stored).filter(([, at]) => Date.now() - Number(at) < ADVISOR_DONE_DAYS * DAY));
      return fresh; }
    catch (_) { return {}; }
  }
  function markAdvisorDone(id) {
    const done = advisorDone();
    if (done[id]) delete done[id]; else done[id] = Date.now();
    try { localStorage.setItem(advisorDoneKey(), JSON.stringify(done)); } catch (_) {}
    renderAdvisor();
  }
  async function loadAdvisor() {
    if (!state.marketing.installed || state.view.advisorLoading) return;
    state.view.advisorLoading = true;
    try { const payload = await request(orgPath("/advisor"));
      state.marketing.advisor = payload.advisor || null; state.marketing.adPlan = payload.adPlan || null;
      renderAdvisor(); renderAdPlan(); }
    catch (error) { message("error", `Could not read the advisor: ${error.message}`); }
    finally { state.view.advisorLoading = false; }
  }
  function advisorItems() {
    const done = advisorDone(), area = state.view.advisorArea || "", priority = state.view.advisorPriority || "";
    return (state.marketing.advisor?.items || []).filter((item) => (!area || item.area === area)
      && (!priority || item.priority === priority) && (state.view.advisorShowDone || !done[item.id]));
  }
  function renderAdvisor() {
    const list = $(`[data-marketing-advisor-list]`); if (!list) return;
    const advisor = state.marketing.advisor, done = advisorDone();
    const all = advisor?.items || [];
    const counts = all.reduce((total, item) => ({ ...total, [item.area]: (total[item.area] || 0) + 1 }), {});
    $(`[data-marketing-advisor-areas]`).innerHTML = [["", "All", all.length], ...Object.keys(ADVISOR_AREAS).filter((key) => counts[key]).map((key) => [key, ADVISOR_AREAS[key], counts[key]])]
      .map(([key, label, count]) => `<button data-marketing-advisor-area="${key || "all"}" type="button" role="tab" class="mk-tab" aria-selected="${(state.view.advisorArea || "") === key}">${label}<em>${count}</em></button>`).join("");
    const items = advisorItems();
    const byPriority = advisor?.byPriority || {};
    $(`[data-marketing-advisor-count]`).textContent = advisor ? `${fmtFull(advisor.total || 0)} to-do${advisor.total === 1 ? "" : "s"} · ${fmtFull(byPriority.high || 0)} high` : "Reading…";
    list.innerHTML = items.map((item) => {
      const index = all.indexOf(item), verb = ADVISOR_VERBS[item.action?.type];
      return `<tr${done[item.id] ? ` style="opacity:.6"` : ""}><td><span class="fm-label mk-pill ${escapeHtml(item.priority)}">${escapeHtml(item.priority === "high" ? "High" : item.priority === "medium" ? "Medium" : "Low")}</span></td>
        <td>${escapeHtml(item.title)}${item.detail ? `<small>${escapeHtml(item.detail)}</small>` : ""}</td>
        <td>${escapeHtml(ADVISOR_AREAS[item.area] || item.area)}${item.channel ? `<small>${escapeHtml(channelLabel(item.channel))}</small>` : ""}</td>
        <td class="r" style="white-space:nowrap">${verb ? `<button data-marketing-advisor-action="${index}" type="button" class="fm-btn fm-btn-sm">${verb}</button> ` : ""}<button data-marketing-advisor-done="${escapeHtml(item.id)}" type="button" class="mk-link">${done[item.id] ? "Undo" : "Done"}</button></td></tr>`;
    }).join("");
    const empty = $(`[data-marketing-advisor-empty]`);
    empty.hidden = items.length > 0;
    empty.innerHTML = items.length ? "" : advisor ? blankslate("search", "Nothing to do", all.length ? "Every to-do in this view is marked done." : "Every channel, campaign, ad and experiment is on track.")
      : blankslate("chart", "Reading your marketing", "The advisor is checking channels, content, ads, experiments and Analytics.");
  }
  function renderAdPlan() {
    const host = $(`[data-marketing-plan]`); if (!host) return;
    const plan = state.marketing.adPlan;
    if (!plan) { host.innerHTML = `<li style="display:block">${blankslate("ads", "Ranking ad channels", "Reading visits, conversions and logged ads.")}</li>`; return; }
    host.innerHTML = plan.recommendations.slice(0, 6).map((item, index) => {
      const budget = item.suggestedDailyBudget;
      return `<li><span class="mk-score" title="Score out of 100">${fmtFull(item.score)}</span>
        <span><strong>${escapeHtml(item.name)}</strong><small>${escapeHtml(item.how)}</small>
          <ul>${item.why.map((line) => `<li style="display:list-item;padding:0;border:0">${escapeHtml(line)}</li>`).join("")}</ul>
          <small>Start at ${fmtFull(budget.low)}–${fmtFull(budget.high)} ${escapeHtml(budget.currency)} a day · measure: ${escapeHtml(item.measure)}${item.targeting?.length ? ` · target: ${escapeHtml(item.targeting.join(", "))}` : ""}</small></span>
        <span style="display:grid;gap:4px;justify-items:end"><button data-marketing-plan-ad="${index}" type="button" class="fm-btn fm-btn-sm">Plan ad</button>${item.link ? `<button data-marketing-plan-link="${index}" type="button" class="mk-link">Copy link</button>` : ""}${item.manager ? outLink(item.manager, "Ad console ↗", `class="mk-link"`) : ""}</span></li>`;
    }).join("");
    $(`[data-marketing-plan-note]`).textContent = plan.landingPage ? `Landing page: ${plan.landingPage}` : "Ranked from your visits, conversions and logged ads";
    const ready = plan.readiness || [];
    $(`[data-marketing-readiness-count]`).textContent = `${ready.filter((item) => item.done).length} of ${ready.length} ready`;
    $(`[data-marketing-readiness]`).innerHTML = ready.map((item, index) => `<li data-done="${item.done}">${item.done ? CHECK_ICON : CLOCK_ICON}<span style="flex:1">${escapeHtml(item.label)}<small style="display:block;font-size:11.5px;color:var(--mk-muted)">${escapeHtml(item.detail)}</small></span>${item.done ? "" : `<button data-marketing-readiness-action="${index}" type="button" class="mk-link">Fix</button>`}</li>`).join("");
  }
  function openAdFor(item) {
    openAd();
    const channel = $(`[data-marketing-ad-channel]`);
    if (item.channel && channel && [...channel.options].some((option) => option.value === item.channel)) channel.value = item.channel;
    if (item.link) $(`[data-marketing-ad-destination]`).value = item.link;
    if (item.name) $(`[data-marketing-ad-name]`).value = `${item.name} test`;
    if (item.targeting?.length) $(`[data-marketing-ad-audience]`).value = item.targeting.join(", ");
    describeAdPlatform();
  }
  function renderAssistantCampaigns() {
    const select = $(`[data-marketing-assistant-campaign]`); if (!select) return;
    const previous = select.value;
    const open = (state.marketing.campaigns || []).filter((item) => ["idea", "planned", "active", "paused"].includes(item.status));
    select.innerHTML = `<option value="">All marketing</option>` + open.map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(item.name)}</option>`).join("");
    select.value = open.some((item) => item.id === previous) ? previous : "";
    const form = $(`[data-marketing-assistant-form]`);
    if (form) form.hidden = !state.marketing.assistantAvailable;
  }
  async function runAssistant(event, campaignId = null) {
    event?.preventDefault();
    const select = $(`[data-marketing-assistant-campaign]`);
    if (campaignId !== null) select.value = campaignId;
    const button = $(`[data-marketing-assistant-run]`), out = $(`[data-marketing-assistant]`);
    button.disabled = true; button.textContent = "Thinking…";
    out.hidden = false; out.innerHTML = `<p class="mk-hint">The Marketing agent is reading your campaigns, channels and keywords…</p>`;
    try { const payload = await request(orgPath("/assistant"), "POST", { campaignId: select.value, question: $(`[data-marketing-assistant-question]`).value });
      state.marketing.assistant = payload; renderAssistant(); }
    catch (error) { out.innerHTML = `<div class="fm-flash ${error.status === 429 ? "fm-flash-warning" : "fm-flash-danger"}">${ALERT_ICON}<div class="fm-flash-body">${escapeHtml(error.status === 429 ? "The agent is busy; try again in a minute." : `The agent could not answer: ${error.message}`)}</div></div>`; }
    finally { button.disabled = false; button.textContent = "Suggest"; }
  }
  function renderAssistant() {
    const out = $(`[data-marketing-assistant]`), result = state.marketing.assistant; if (!out || !result) return;
    const campaign = campaignById(result.campaignId);
    const label = { content: "Add draft", task: "Add to checklist", keyword: "Track", campaign: "Create" };
    out.hidden = false;
    out.innerHTML = `${result.summary ? `<p>${escapeHtml(result.summary)}</p>` : ""}${(result.suggestions || []).map((idea, index) => {
      const disabled = idea.kind === "task" && !campaign;
      const meta = [idea.channel ? channelLabel(idea.channel) : "", idea.inDays ? `in ${idea.inDays} day${idea.inDays === 1 ? "" : "s"}` : idea.kind === "content" ? "today" : ""].filter(Boolean).join(" · ");
      return `<div class="mk-idea"><header><b>${IDEA_ICONS[idea.kind] || ""} ${escapeHtml(idea.title)}</b>${idea.applied ? `<span class="mk-hint">${CHECK_ICON}Added</span>` : `<button data-marketing-idea="${index}" type="button" class="mk-link" ${disabled ? 'disabled title="Pick a campaign first"' : ""}>${label[idea.kind]}</button>`}</header>${meta ? `<small>${escapeHtml(meta)}</small>` : ""}${idea.detail ? `<small>${escapeHtml(idea.detail)}</small>` : ""}${idea.copy ? `<blockquote>${escapeHtml(idea.copy)}</blockquote>` : ""}</div>`;
    }).join("")}<small class="mk-hint">Suggestions are drafts: nothing is posted or spent until a person does it.</small>`;
  }
  async function applyIdea(index) {
    const result = state.marketing.assistant, idea = result?.suggestions?.[index]; if (!idea) return;
    const campaign = campaignById(result.campaignId);
    try {
      if (idea.kind === "content") {
        const scheduled = new Date(startOfDay(Date.now()) + idea.inDays * DAY); scheduled.setHours(10);
        await request(orgPath("/content"), "POST", { title: idea.title, body: idea.copy || idea.detail, channel: idea.channel,
          campaign: campaign?.name || "", status: "draft", scheduledAt: idea.inDays ? scheduled.getTime() : 0, notes: "Suggested by the Marketing agent." });
      } else if (idea.kind === "task" && campaign) {
        const payload = await request(orgPath(`/campaigns/${campaign.id}`), "PATCH", { tasks: [...campaign.tasks, { title: idea.title, done: false }] });
        upsert(state.marketing.campaigns, payload.campaign);
      } else if (idea.kind === "keyword") {
        await request(orgPath("/keywords"), "POST", { term: idea.title, campaign: campaign?.name || "", notes: idea.detail });
      } else if (idea.kind === "campaign") { openCampaign(null, { name: idea.title, brief: idea.detail, status: "idea" }); return; }
      idea.applied = true; renderAssistant(); await load();
    } catch (error) { message("error", error.status === 409 ? "That keyword is already tracked." : `Could not add it: ${error.message}`); }
  }

  function renderAdsOverview() {
    const host = $(`[data-marketing-ads-overview]`); if (!host) return;
    const summary = state.marketing.adsSummary || {}, now = Date.now();
    const currency = (summary.currencies || [])[0];
    const ads = (state.marketing.ads || []).filter((ad) => ["active", "scheduled", "paused"].includes(ad.status))
      .sort((a, b) => (a.status !== "active") - (b.status !== "active") || b.spendCents - a.spendCents).slice(0, 4);
    if (!(state.marketing.ads || []).length) { host.innerHTML = `<p class="mk-hint">Plan paid placements on any channel and log what each platform reports. <button data-marketing-ad-open type="button" class="mk-link">New ad</button></p>`; return; }
    const totals = currency ? `<dl class="mk-ads-totals"><div><dt>Spend · ${escapeHtml(currency.currency)}</dt><dd>${escapeHtml(fmtMoney(currency.spendCents, currency.currency, true))}</dd></div><div><dt>Clicks · CTR</dt><dd>${fmtCompact(currency.clicks)} · ${Number(currency.ctr || 0).toFixed(1)}%</dd></div><div><dt>Conv. · ROAS</dt><dd>${fmtCompact(currency.conversions)} · ${currency.roas ? `${currency.roas}×` : "—"}</dd></div></dl>` : "";
    host.innerHTML = `${totals}<p class="mk-hint" style="margin:0 0 4px">${fmtFull(summary.active || 0)} live · ${fmtFull(summary.attention || 0)} need attention${(summary.currencies || []).length > 1 ? ` · ${summary.currencies.length} currencies` : ""}</p>${ads.map((ad) => {
      const pace = PACING[ad.pacing?.state];
      const total = ad.pacing?.totalBudgetCents || 0;
      const spent = total ? Math.min(100, ad.spendCents / total * 100) : 0, expected = total ? Math.min(100, (ad.pacing?.expectedCents || 0) / total * 100) : 0;
      return `<button data-marketing-ad-edit="${escapeHtml(ad.id)}" type="button" class="mk-ad-row"><b>${escapeHtml(ad.name)}</b><span class="fm-label mk-pill ${escapeHtml(ad.status)}">${escapeHtml(labelOf(AD_STATUSES, ad.status))}</span><small>${escapeHtml(channelLabel(ad.channel))} · ${escapeHtml(fmtMoney(ad.spendCents, ad.currency))}${total ? ` of ${escapeHtml(fmtMoney(total, ad.currency))}` : ""}${pace ? ` · ${pace[0]}` : ""}${ad.endsAt && ad.endsAt > now ? ` · ends ${escapeHtml(relativeLabel(ad.endsAt))}` : ""}</small>${total ? `<span class="mk-budget"><span class="mk-track"><span class="mk-bar${ad.spendCents > total ? " over" : ""}" style="width:${spent}%"></span>${expected ? `<span class="mk-expected" style="left:${expected}%"></span>` : ""}</span></span>` : ""}</button>`;
    }).join("") || `<p class="mk-hint">No live ads.</p>`}`;
  }

  function xLength(text) { return [...String(text || "").replace(/https?:\/\/\S+/g, "x".repeat(23))].length; }
  function renderX() {
    const x = xState(), manage = canManage();
    const status = $(`[data-marketing-x-status]`); if (!status) return;
    $(`[data-marketing-x-settings]`).textContent = x.connected ? "Connection" : manage ? "Connect" : "";
    $(`[data-marketing-x-settings]`).hidden = !manage;
    status.innerHTML = x.connected
      ? `Posting as <a href="https://x.com/${escapeHtml(x.username)}" target="_blank" rel="noopener noreferrer"><b>@${escapeHtml(x.username)}</b></a>${x.lastPostedAt ? ` · last post ${escapeHtml(relativeTimeLabel(x.lastPostedAt))}${x.lastPostUrl ? ` <a href="${escapeHtml(x.lastPostUrl)}" target="_blank" rel="noopener noreferrer" class="mk-link">View ↗</a>` : ""}` : ""}`
      : manage ? `Connect the organization's X account to post from here.` : `An owner or admin can connect X to post from here.`;
    const select = $(`[data-marketing-x-content]`), previous = select.value;
    const items = state.marketing.content.filter((item) => item.channel === "x" && !["published", "archived"].includes(item.status))
      .sort((a, b) => (a.scheduledAt || Infinity) - (b.scheduledAt || Infinity));
    select.innerHTML = `<option value="">Write a new post</option>` + items.map((item) => `<option value="${escapeHtml(item.id)}">${escapeHtml(item.title)}${item.scheduledAt ? ` · ${escapeHtml(shortDate(item.scheduledAt))}` : ""}</option>`).join("");
    select.value = items.some((item) => item.id === previous) ? previous : "";
    select.hidden = !items.length;
    const allowed = x.connected && manage;
    $(`[data-marketing-x-text]`).disabled = !allowed; $(`[data-marketing-x-post]`).disabled = !allowed;
    updateXCount();
  }
  function updateXCount() {
    const length = xLength($(`[data-marketing-x-text]`)?.value), node = $(`[data-marketing-x-count]`); if (!node) return;
    node.textContent = `${length} / ${X_LIMIT}`; node.classList.toggle("over", length > X_LIMIT);
  }
  function composeFromContent(id) {
    const item = contentById(id); if (!item) return;
    $(`[data-marketing-x-content]`).value = item.channel === "x" ? item.id : "";
    $(`[data-marketing-x-text]`).value = item.body || item.title;
    $(`[data-marketing-x-form]`).dataset.contentId = item.id;
    updateXCount(); $(`[data-marketing-x-text]`).focus();
    $(`[data-marketing-x-form]`).scrollIntoView({ behavior: "smooth", block: "center" });
  }
  async function postToX(event) {
    event.preventDefault();
    const form = $(`[data-marketing-x-form]`), text = $(`[data-marketing-x-text]`).value.trim(); if (!text) return;
    if (xLength(text) > X_LIMIT && !confirm(`This post is ${xLength(text)} characters. Only X Premium accounts can post more than ${X_LIMIT}. Post anyway?`)) return;
    if (!confirm(`Post this to X as @${xState().username}?\n\n${text}`)) return;
    const button = $(`[data-marketing-x-post]`); button.disabled = true; button.textContent = "Posting…";
    try { const payload = await request(orgPath("/x-post"), "POST", { text, contentId: $(`[data-marketing-x-content]`).value || form.dataset.contentId || "" });
      $(`[data-marketing-x-text]`).value = ""; delete form.dataset.contentId;
      message("notice", `Posted to X: ${payload.url}`); await load(); }
    catch (error) { message("error", `X did not accept the post: ${error.message}${error.payload?.detail ? ` (${error.payload.detail})` : ""}`); }
    finally { button.textContent = "Post to X"; renderX(); }
  }
  function openXSettings() {
    const x = xState();
    $$(`[data-marketing-x-key]`).forEach((input) => { input.value = ""; });
    const connected = $(`[data-marketing-x-connected]`);
    connected.hidden = !x.connected;
    connected.textContent = x.connected ? `Connected as @${x.username}${x.connectedBy ? ` by ${x.connectedBy}` : ""}${x.connectedAt ? ` on ${shortDate(x.connectedAt)}` : ""}. Enter new keys to replace them.` : "";
    $(`[data-marketing-x-disconnect]`).hidden = !x.connected;
    xGuideStatus("");
    $(`[data-marketing-x-dialog]`).showModal();
  }
  async function saveXConnection(event) {
    event.preventDefault();
    const body = Object.fromEntries($$(`[data-marketing-x-key]`).map((input) => [input.dataset.marketingXKey, input.value.trim()]));
    try { const payload = await request(orgPath("/x-connection"), "POST", body);
      state.marketing.settings.x = payload.x; $(`[data-marketing-x-dialog]`).close();
      message("notice", `X connected as @${payload.x.username}.`); renderX(); }
    catch (error) { message("error", `X refused these keys: ${error.message}${error.payload?.detail ? ` (${error.payload.detail})` : ""}. Check the app has Read and write permission and regenerate the access token after changing it.`); }
  }
  async function disconnectX() {
    if (!confirm("Remove the X keys from this organization?")) return;
    try { const payload = await request(orgPath("/x-connection"), "DELETE");
      state.marketing.settings.x = payload.x; $(`[data-marketing-x-dialog]`).close(); renderX(); }
    catch (error) { message("error", `Could not disconnect X: ${error.message}`); }
  }

  function openKeyword(item = null, term = "") {
    const field = (name) => $(`[data-marketing-keyword-field="${name}"]`);
    $(`[data-marketing-keyword-id]`).value = item?.id || "";
    $(`[data-marketing-keyword-title]`).textContent = item ? item.term : "Track a keyword";
    field("term").value = item?.term || term;
    for (const key of ["targetUrl", "intent", "campaign", "notes"]) field(key).value = item?.[key] || "";
    field("priority").value = String(item?.priority || 0);
    $$(`[data-marketing-keyword-snap]`).forEach((input) => { input.value = ""; });
    $(`[data-marketing-keyword-snap="at"]`).value = new Date().toISOString().slice(0, 10);
    $(`[data-marketing-keyword-delete]`).classList.toggle("hidden", !item);
    $(`[data-marketing-keyword-dialog]`).showModal();
  }
  async function saveKeyword(event) {
    event.preventDefault();
    const id = $(`[data-marketing-keyword-id]`).value;
    const field = (name) => $(`[data-marketing-keyword-field="${name}"]`).value;
    const snap = Object.fromEntries($$(`[data-marketing-keyword-snap]`).map((input) => [input.dataset.marketingKeywordSnap, input.value]));
    const body = { term: field("term"), targetUrl: field("targetUrl"), intent: field("intent"), campaign: field("campaign"),
      priority: Number(field("priority")), notes: field("notes") };
    if (snap.position || snap.impressions || snap.clicks) body.snapshot = { position: snap.position, impressions: snap.impressions, clicks: snap.clicks,
      at: snap.at ? Date.parse(`${snap.at}T12:00:00Z`) : Date.now(), source: "manual" };
    try { await request(orgPath(`/keywords${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-marketing-keyword-dialog]`).close(); await load(); }
    catch (error) { message("error", error.status === 409 ? "That keyword is already tracked." : `Could not save the keyword: ${error.message}`); }
  }
  async function deleteKeyword() {
    const id = $(`[data-marketing-keyword-id]`).value;
    if (!id || !confirm("Stop tracking this keyword and drop its history?")) return;
    try { await request(orgPath(`/keywords/${id}`), "DELETE"); $(`[data-marketing-keyword-dialog]`).close(); await load(); }
    catch (error) { message("error", `Could not delete the keyword: ${error.message}`); }
  }
  function csvCells(line, separator) {
    const cells = []; let current = "", quoted = false;
    for (let index = 0; index < line.length; index += 1) {
      const char = line[index];
      if (char === '"') { if (quoted && line[index + 1] === '"') { current += '"'; index += 1; } else quoted = !quoted; }
      else if (char === separator && !quoted) { cells.push(current.trim()); current = ""; }
      else current += char;
    }
    cells.push(current.trim()); return cells;
  }
  function parseKeywordImport(text) {
    const lines = String(text || "").split(/\r?\n/).map((line) => line.trim()).filter(Boolean);
    if (!lines.length) return [];
    const separator = lines[0].includes("\t") ? "\t" : lines[0].includes(";") && !lines[0].includes(",") ? ";" : ",";
    const head = csvCells(lines[0], separator).map((value) => value.toLowerCase());
    const find = (...names) => head.findIndex((value) => names.some((name) => value.includes(name)));
    const columns = { term: find("quer", "keyword", "search term", "term"), clicks: find("click"), impressions: find("impression"), position: find("position", "rank") };
    const hasHeader = columns.term >= 0 && (columns.clicks >= 0 || columns.impressions >= 0 || columns.position >= 0 || head.length === 1);
    const number = (value) => { const clean = String(value || "").replace(/[%\s]/g, "").replace(/,(?=\d{3}\b)/g, ""); return clean === "" ? null : Number(clean.replace(",", ".")); };
    return (hasHeader ? lines.slice(1) : lines).map((line) => {
      if (!hasHeader) return { term: line.split(separator)[0].replace(/^"|"$/g, "") };
      const cells = csvCells(line, separator);
      return { term: cells[columns.term], clicks: columns.clicks >= 0 ? number(cells[columns.clicks]) : null,
        impressions: columns.impressions >= 0 ? number(cells[columns.impressions]) : null,
        position: columns.position >= 0 ? number(cells[columns.position]) : null };
    }).filter((row) => row.term);
  }
  function previewKeywordImport() {
    const rows = parseKeywordImport($(`[data-marketing-keyword-import-text]`).value);
    const limit = Math.max(1, Number($(`[data-marketing-keyword-import-limit]`).value || 100));
    $(`[data-marketing-keyword-import-preview]`).textContent = rows.length ? `${rows.length} keyword${rows.length === 1 ? "" : "s"} found${rows.length > limit ? `; the top ${limit} by clicks and impressions will be imported` : ""}. First: “${rows[0].term}”.` : "";
  }
  function openKeywordImport() {
    $(`[data-marketing-keyword-import-text]`).value = "";
    $(`[data-marketing-keyword-import-date]`).value = new Date().toISOString().slice(0, 10);
    $(`[data-marketing-keyword-import-preview]`).textContent = "";
    $(`[data-marketing-keyword-import-dialog]`).showModal();
  }
  async function importKeywords(event) {
    event.preventDefault();
    const limit = Math.max(1, Math.min(500, Number($(`[data-marketing-keyword-import-limit]`).value || 100)));
    const rows = parseKeywordImport($(`[data-marketing-keyword-import-text]`).value)
      .sort((a, b) => (b.clicks || 0) - (a.clicks || 0) || (b.impressions || 0) - (a.impressions || 0)).slice(0, limit);
    if (!rows.length) return message("error", "No keywords found in that text.");
    const date = $(`[data-marketing-keyword-import-date]`).value;
    try { const payload = await request(orgPath("/keywords"), "POST", { rows, source: $(`[data-marketing-keyword-import-source]`).value,
      observedAt: date ? Date.parse(`${date}T12:00:00Z`) : Date.now() });
      $(`[data-marketing-keyword-import-dialog]`).close();
      message("notice", `Imported keywords: ${payload.created} new, ${payload.updated} updated${payload.skipped ? `, ${payload.skipped} skipped` : ""}.`); await load(); }
    catch (error) { message("error", `Could not import keywords: ${error.message}`); }
  }

  function render() {
    const series = seriesByChannel(), colors = channelColors(series);
    $$(`[data-marketing-range]`).forEach((button) => button.setAttribute("aria-pressed", String(Number(button.dataset.marketingRange) === state.view.rangeDays)));
    $$(`[data-marketing-chart-metric]`).forEach((button) => button.setAttribute("aria-pressed", String(button.dataset.marketingChartMetric === state.view.chartMetric)));
    syncChannelControls(); renderKpis(series); renderChart(series, colors); renderFlow();
    renderChannelGrid(); renderContent(colors); renderWeek(colors); renderCatalog();
    renderTrafficKpis(); renderTraffic(); renderLanding(); renderReferrers(); renderKeywords(); renderCampaigns();
    renderSuggestions(); renderAssistant(); renderAdsOverview(); renderX();
    renderAds(); renderGrowth(); renderReddit(); renderChannelHealth();
    if (state.view.page === "app-store") renderAppStore();
  }

  function localDateTime(timestamp) {
    if (!timestamp) return "";
    const date = new Date(timestamp); date.setMinutes(date.getMinutes() - date.getTimezoneOffset());
    return date.toISOString().slice(0, 16);
  }
  function openContent(item = null) {
    syncChannelControls(item?.channel || "");
    $(`[data-marketing-content-title]`).textContent = item ? "Edit content" : "New content";
    $(`[data-marketing-content-id]`).value = item?.id || "";
    for (const field of ["channel", "status", "campaign", "owner", "url", "body", "notes"])
      $(`[data-marketing-content-${field}]`).value = item?.[field] || (field === "channel" ? "blog" : field === "status" ? "draft" : "");
    $(`[data-marketing-content-name]`).value = item?.title || "";
    $(`[data-marketing-content-tags]`).value = (item?.tags || []).join(", ");
    $(`[data-marketing-content-scheduled]`).value = localDateTime(item?.scheduledAt);
    $(`[data-marketing-content-delete]`).classList.toggle("hidden", !item);
    $(`[data-marketing-content-dialog]`).showModal();
  }
  async function saveContent(event) {
    event.preventDefault(); const id = $(`[data-marketing-content-id]`).value;
    const body = { title: $(`[data-marketing-content-name]`).value,
      tags: $(`[data-marketing-content-tags]`).value.split(",").map((tag) => tag.trim()).filter(Boolean) };
    for (const field of ["channel", "status", "campaign", "owner", "url", "body", "notes"])
      body[field] = $(`[data-marketing-content-${field}]`).value;
    const scheduled = $(`[data-marketing-content-scheduled]`).value;
    body.scheduledAt = scheduled ? new Date(scheduled).getTime() : 0;
    // Keep the original publish date when an already-published item is edited;
    // the published KPI and cycle time are measured from it.
    const existing = id ? contentById(id) : null;
    if (body.status === "published" && !(existing?.status === "published" && existing.publishedAt)) body.publishedAt = Date.now();
    try { await request(orgPath(`/content${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-marketing-content-dialog]`).close(); message("notice", "Content saved."); await load(); }
    catch (error) { message("error", `Could not save content: ${error.message}`); }
  }
  async function deleteContent() {
    const id = $(`[data-marketing-content-id]`).value; if (!id || !confirm("Delete this content item?")) return;
    try { await request(orgPath(`/content/${id}`), "DELETE", {}); $(`[data-marketing-content-dialog]`).close(); message("notice", "Content deleted."); await load(); }
    catch (error) { message("error", `Could not delete content: ${error.message}`); }
  }

  const METRIC_FIELDS = ["reach", "followers", "impressions", "engagements", "clicks", "views", "visits", "indexedPages", "stars", "forks"];
  const metricInput = (key) => $(`[data-marketing-metric-${key.replace(/[A-Z]/g, (letter) => `-${letter.toLowerCase()}`)}]`);
  function describePreviousSnapshot() {
    const channel = $(`[data-marketing-metric-channel]`).value;
    const list = seriesByChannel().get(channel) || [];
    const latest = list[list.length - 1];
    $(`[data-marketing-metric-previous]`).textContent = latest
      ? `Last snapshot ${relativeTimeLabel(latest.observedAt)} by ${latest.recordedBy || "a teammate"}: reach ${fmtFull(latest.reach)} · followers ${fmtFull(latest.followers)} · engagements ${fmtFull(latest.engagements)}.`
      : "No snapshots logged for this channel yet.";
    for (const key of METRIC_FIELDS) {
      const input = metricInput(key);
      input.value = ""; input.placeholder = latest ? String(latest[key] || 0) : "0";
    }
  }
  function openMetric(channel = "") {
    syncChannelControls(channel);
    if (channel) $(`[data-marketing-metric-channel]`).value = channel;
    describePreviousSnapshot();
    $(`[data-marketing-metric-dialog]`).showModal();
  }
  async function saveMetric(event) {
    event.preventDefault(); const body = { channel: $(`[data-marketing-metric-channel]`).value };
    for (const key of METRIC_FIELDS)
      body[key] = Number(metricInput(key).value || 0);
    try { await request(orgPath("/metrics"), "POST", body); $(`[data-marketing-metric-dialog]`).close(); message("notice", "Metric snapshot saved."); await load(); }
    catch (error) { message("error", `Could not save metrics: ${error.message}`); }
  }

  async function refreshFeeds() {
    message("error", ""); message("notice", "Reading the configured blog RSS feed…");
    try {
      const payload = await request(orgPath("/refresh"), "POST", {});
      applyOverview(payload);
      render();
      const result = payload.refresh || {}; const feed = result.feed || {};
      message("notice", `Read ${Number(result.items || 0).toLocaleString()} posts from ${feed.title || "the blog feed"} · ${Number(result.imported || 0).toLocaleString()} newly imported.`);
    } catch (error) { message("error", `Could not read the blog feed: ${error.message}`); }
  }

  const REFRESH_ERRORS = { "crm not installed": "install the CRM package for this organization first", "crm unavailable": "the CRM package is not available here",
    "app store url unsupported": "add the app's App Store link, like https://apps.apple.com/app/id1234567890",
    "app store app not found": "the App Store has no app with that id in this country's store",
    "discord connector not connected": "connect Discord in the organization's settings and choose its channels",
    "discord connector not allowed": "update Marketing so it may read the Discord connector",
    "discord channel required": "choose the channels to read in the organization's Discord settings",
    "discord secret required": "the Discord connector has no bot token yet",
    "discord authorization required": "the Discord bot lost access; reconnect it in the Discord connector",
    "discord guild missing": "the Discord bot is no longer in the server; add it again",
    "discord rate limited": "Discord is rate limiting the bot; try again later" };
  async function refreshChannel(channel) {
    const name = channelLabel(channel);
    message("error", ""); message("notice", `Reading ${name}'s numbers…`);
    try {
      const payload = await request(orgPath("/refresh"), "POST", { channel });
      applyOverview(payload);
      if (channel === "email") state.marketing.emailSuggestions = null;
      if (channel === "discord") state.marketing.discordPosts = null;
      if (channel === "x") state.marketing.readyPost = null;
      if (channel === "app-store") void loadAppStore();
      render();
      const metrics = payload.refresh?.metrics || {};
      message("notice", channel === "email"
        ? `${name}: ${fmtFull(metrics.outbound || 0)} emails sent in 30 days · ${fmtFull(metrics.closedDeals || 0)} closed deals${metrics.closedValue ? ` worth ${fmtFull(metrics.closedValue)}` : ""}.`
        : channel === "github" ? `${name}: ${fmtFull(metrics.stars || 0)} stars · ${fmtFull(metrics.forks || 0)} forks · ${fmtFull(metrics.followers || 0)} followers.`
        : channel === "app-store" ? `${name}: ${fmtFull(metrics.ratings || 0)} ratings · ${fmtFull(metrics.downloads || 0)} downloads in 30 days.`
        : `${name} updated.`);
    } catch (error) { message("error", `Could not refresh ${name}: ${REFRESH_ERRORS[error.message] || error.message}`); }
  }

  function openSettings() {
    const canSave = state.marketing.canManageSettings;
    $(`[data-marketing-channel-settings]`).innerHTML = (state.marketing.settings.channels || []).map((channel) => `<div data-marketing-channel-setting="${escapeHtml(channel.id)}" class="grid gap-2 rounded-md border border-border bg-secondary p-3 sm:grid-cols-[auto_10rem_1fr_auto]"><label class="flex items-center gap-2 text-xs text-foreground"><input data-marketing-channel-enabled type="checkbox" ${channel.enabled ? "checked" : ""} /> Active</label><label class="grid gap-1 text-xs text-muted-foreground">Display name<input data-marketing-channel-name maxlength="80" value="${escapeHtml(channel.displayName)}" class="rounded border border-border bg-background px-2 py-1.5 text-foreground" /></label><label class="grid gap-1 text-xs text-muted-foreground">${channel.id === "blog" ? "Website or RSS URL" : "Channel URL (optional)"}<input data-marketing-channel-url type="url" maxlength="1000" value="${escapeHtml(channel.profileUrl || "")}" placeholder="${channel.id === "blog" ? "https://forkmesh.com/blog" : channel.id === "google-search" ? "https://www.google.com/search?q=site:forkmesh.com" : "https://…"}" class="rounded border border-border bg-background px-2 py-1.5 text-foreground" /></label>${canSave ? `<button data-marketing-channel-delete="${escapeHtml(channel.id)}" type="button" class="fm-btn fm-btn-sm fm-btn-danger self-end">Delete</button>` : ""}</div>`).join("");
    $(`[data-marketing-settings-access]`).textContent = canSave ? "Refresh discovers RSS from the Blog website and imports published posts. These settings contain no access tokens and never publish automatically." : "Only organization owners and admins can change channel settings.";
    const attribution = state.marketing.settings.attribution || {}, targets = state.marketing.settings.targets || {};
    $$(`[data-marketing-attribution-field]`).forEach((input) => {
      const value = attribution[input.dataset.marketingAttributionField];
      input.value = Array.isArray(value) ? value.join(", ") : value || ""; input.disabled = !canSave; });
    $$(`[data-marketing-target]`).forEach((input) => { input.value = targets[input.dataset.marketingTarget] || ""; input.disabled = !canSave; });
    $(`[data-marketing-settings-save]`).disabled = !canSave;
    $(`[data-marketing-plugin-zone]`).classList.toggle("hidden", !canSave);
    $(`[data-marketing-settings-dialog]`).showModal();
  }
  async function saveSettings(event) {
    event.preventDefault(); const channels = $$(`[data-marketing-channel-setting]`).map((row) => ({
      id: row.dataset.marketingChannelSetting,
      enabled: row.querySelector(`[data-marketing-channel-enabled]`).checked,
      displayName: row.querySelector(`[data-marketing-channel-name]`).value,
      profileUrl: row.querySelector(`[data-marketing-channel-url]`).value,
    }));
    const attribution = Object.fromEntries($$(`[data-marketing-attribution-field]`).map((input) => [input.dataset.marketingAttributionField, input.value]));
    const targets = Object.fromEntries($$(`[data-marketing-target]`).map((input) => [input.dataset.marketingTarget, Number(input.value || 0)]));
    try { await request(orgPath("/settings"), "PUT", { channels, attribution, targets }); $(`[data-marketing-settings-dialog]`).close(); message("notice", "Channel settings saved.");
      const blog = channels.find((channel) => channel.id === "blog");
      if (blog?.enabled && String(blog.profileUrl || "").trim()) await refreshFeeds(); else await load(); }
    catch (error) { message("error", `Could not save channels: ${error.message}`); }
  }

  function applyOverview(payload) {
    Object.assign(state.marketing, { content: payload.content || [], metrics: payload.metrics || [],
      settings: payload.settings || {}, summary: payload.summary || {},
      channelCatalog: payload.channelCatalog || state.marketing.channelCatalog || [],
      ads: payload.ads || [], adsSummary: payload.adsSummary || {},
      adPlatforms: payload.adPlatforms || [],
      experiments: payload.experiments || [], experimentsSummary: payload.experimentsSummary || {},
      funnel: payload.funnel || [], funnelReport: payload.funnelReport || {},
      redditThreads: payload.redditThreads || [], redditDesk: payload.redditDesk || {},
      posts: payload.posts || [],
      channelPlaybooks: payload.channelPlaybooks || state.marketing.channelPlaybooks || {},
      campaigns: payload.campaigns || [], campaignTemplates: payload.campaignTemplates || [],
      keywords: payload.keywords || [], suggestions: payload.suggestions || [],
      referringSites: payload.referringSites || null, readyPosts: payload.readyPosts || {},
      assistantAvailable: Boolean(payload.assistantAvailable),
      canManageSettings: Boolean(payload.canManageSettings) });
  }
  async function load() {
    renderInstall(); if (!state.marketing.installed) return;
    try { const payload = await request(orgPath());
      applyOverview(payload);
      state.marketing.advisor = null; state.marketing.adPlan = null;
      message("error", ""); render(); void loadTraffic();
      if (["advisor", "ads"].includes(state.view.page)) void loadAdvisor();
      if (state.view.page === "website") void loadPerformance();
      const blog = (state.marketing.settings.channels || []).find((channel) => channel.id === "blog");
      if (blog?.enabled && String(blog.profileUrl || "").trim() &&
          !blog.feed?.lastReadAt && !blog.feed?.lastError) await refreshFeeds(); }
    catch (error) { if (error.status === 409) { selectedOrg().installed = false; renderInstall(); }
      else message("error", `Marketing is unavailable: ${error.message}`); }
  }

  function closeMenu() { const menu = $(`[data-marketing-menu]`); if (menu) menu.open = false; }

  const PANEL_ORDER_KEY = "forkmesh.marketing.overviewPanels";
  function mergedPanelOrder(defaults, saved) {
    const order = saved.filter((key, index) => defaults.includes(key) && saved.indexOf(key) === index);
    defaults.forEach((key, index) => {
      if (!order.includes(key)) order.splice(index ? order.indexOf(defaults[index - 1]) + 1 : 0, 0, key);
    });
    return order;
  }

  function setupMovablePanels() {
    const grid = $(`[data-marketing-panels]`); if (!grid) return;
    const panelOf = (key) => grid.querySelector(`:scope > [data-marketing-panel="${CSS.escape(key)}"]`);
    const keys = () => [...grid.children].map((node) => node.dataset.marketingPanel).filter(Boolean);
    let saved = [];
    try { saved = JSON.parse(localStorage.getItem(PANEL_ORDER_KEY) || "[]"); } catch { saved = []; }
    if (Array.isArray(saved) && saved.length) mergedPanelOrder(keys(), saved.map(String)).forEach((key) => grid.append(panelOf(key)));
    let drag = null;
    const mark = (target) => {
      if (target === drag.target) return;
      drag.target?.classList.remove("mk-drop-target"); target?.classList.add("mk-drop-target"); drag.target = target;
    };
    const finish = (drop) => {
      if (!drag) return;
      const { panel, target, moving } = drag;
      mark(null); drag = null;
      panel.classList.remove("mk-dragging"); document.body.classList.remove("mk-panel-moving");
      if (!drop || !moving || !target) return;
      if (panel.compareDocumentPosition(target) & Node.DOCUMENT_POSITION_FOLLOWING) target.after(panel); else target.before(panel);
      try { localStorage.setItem(PANEL_ORDER_KEY, JSON.stringify(keys())); } catch {}
    };
    grid.addEventListener("pointerdown", (event) => {
      const head = event.target.closest(".mk-ph"), panel = head?.parentElement;
      if (event.button !== 0 || event.pointerType === "touch" || !panel?.dataset.marketingPanel || panel.parentElement !== grid) return;
      if (event.target.closest("button, a, input, select, textarea, label")) return;
      drag = { panel, x: event.clientX, y: event.clientY, moving: false, target: null };
      head.setPointerCapture(event.pointerId);
    });
    grid.addEventListener("pointermove", (event) => {
      if (!drag) return;
      if (!drag.moving) {
        if (Math.hypot(event.clientX - drag.x, event.clientY - drag.y) < 6) return;
        drag.moving = true; drag.panel.classList.add("mk-dragging"); document.body.classList.add("mk-panel-moving");
      }
      const over = document.elementFromPoint(event.clientX, event.clientY)?.closest("[data-marketing-panel]");
      mark(over && over.parentElement === grid && over !== drag.panel ? over : null);
      const edge = 48;
      if (event.clientY < edge) window.scrollBy(0, -16); else if (event.clientY > window.innerHeight - edge) window.scrollBy(0, 16);
    });
    grid.addEventListener("pointerup", () => finish(true));
    grid.addEventListener("pointercancel", () => finish(false));
    document.addEventListener("keydown", (event) => { if (event.key === "Escape") finish(false); });
  }

  async function init() {
    setupMovablePanels();
    window.addEventListener("message", onExtensionMessage);
    window.postMessage({ source: "forkmesh-marketing-page", type: "marketing-hello" }, location.origin);
    if (!state.session?.sessionToken) return message("error", "Sign in to use organization Marketing.");
    try { const payload = await request("/api/marketing"); state.marketing.organizations = payload.organizations || [];
      if (!state.marketing.organizations.length) return message("error", "Join or create an organization before using Marketing.");
      const select = $(`[data-marketing-organization]`); select.innerHTML = state.marketing.organizations.map((org) => `<option value="${escapeHtml(org.name)}">${escapeHtml(org.name)} · ${escapeHtml(org.role)}${org.installed ? "" : " · not installed"}</option>`).join("");
      const params = new URLSearchParams(location.search);
      const wanted = state.marketing.organizations.find((org) => org.name === params.get("org"));
      state.marketing.organization = (wanted || state.marketing.organizations[0]).name; select.value = state.marketing.organization; await load();
      const page = params.get("page") === "performance" ? "website" : params.get("page");
      if (["advisor", "website", "ads", "growth", "reddit", "channels", "app-store"].includes(page)) showPage(page);
      const wantedCapture = params.get("capture");
      const wantedXSetup = params.get("xsetup") === "1";
      if (wantedCapture || wantedXSetup || params.has("page")) {
        params.delete("capture"); params.delete("xsetup");
        history.replaceState(null, "", `${location.pathname}${params.toString() ? `?${params}` : ""}${location.hash}`);
      }
      if (wantedCapture && state.marketing.installed) {
        if (state.view.page === "dashboard") showPage("channels");
        setTimeout(() => {
          if ((state.marketing.summary.channels || []).some((item) => item.channel === wantedCapture)
              || channelSettings(wantedCapture).id) startCapture(wantedCapture);
          else message("error", "That channel is not set up for this organization.");
        }, capture.available ? 0 : 700);
      }
      if (wantedXSetup && state.marketing.installed) {
        openXSettings();
        setTimeout(startXSetup, xSetup.available ? 0 : 700);
      }
      select.addEventListener("change", () => { state.marketing.organization = select.value; state.view.status = ""; state.view.adStatus = ""; state.view.adChannel = ""; state.view.adCurrency = "";
        state.view.experimentStatus = ""; state.view.redditStatus = "";
        state.view.performanceProject = ""; state.marketing.performance = null;
        capture.jobs.clear(); capture.queue = [];
        showPage(["website", "ads", "growth", "reddit", "channels"].includes(state.view.page) ? state.view.page : "dashboard"); void load(); });
    } catch (error) { return message("error", `Marketing is unavailable: ${error.message}`); }
    const view = $(`[data-view="marketing"]`);
    $(`[data-marketing-install]`)?.addEventListener("click", () => void installPlugin());
    $(`[data-marketing-content-new]`)?.addEventListener("click", () => openContent());
    $(`[data-marketing-content-form]`)?.addEventListener("submit", saveContent);
    $(`[data-marketing-content-delete]`)?.addEventListener("click", () => void deleteContent());
    $(`[data-marketing-metric-new]`)?.addEventListener("click", () => openMetric());
    $(`[data-marketing-ad-new]`)?.addEventListener("click", () => openAd());
    $(`[data-marketing-ad-form]`)?.addEventListener("submit", saveAd);
    $(`[data-marketing-ad-delete]`)?.addEventListener("click", () => void deleteAd());
    $(`[data-marketing-ad-channel]`)?.addEventListener("change", describeAdPlatform);
    $(`[data-marketing-ad-results-form]`)?.addEventListener("submit", saveAdResults);
    $(`[data-marketing-experiment-new]`)?.addEventListener("click", () => openExperiment());
    $(`[data-marketing-experiment-form]`)?.addEventListener("submit", saveExperiment);
    $(`[data-marketing-experiment-delete]`)?.addEventListener("click", () => void deleteExperiment());
    $(`[data-marketing-funnel-new]`)?.addEventListener("click", () => openFunnel());
    $(`[data-marketing-funnel-form]`)?.addEventListener("submit", saveFunnel);
    $(`[data-marketing-funnel-week]`)?.addEventListener("change", (event) => fillFunnel(funnelForWeek(Date.parse(`${event.target.value}T00:00:00Z`))));
    $(`[data-marketing-link-form]`)?.addEventListener("submit", buildLink);
    $(`[data-marketing-goals-open]`)?.addEventListener("click", openGoals);
    $(`[data-marketing-advisor-priority]`)?.addEventListener("change", (event) => { state.view.advisorPriority = event.target.value; renderAdvisor(); });
    $(`[data-marketing-advisor-show-done]`)?.addEventListener("change", (event) => { state.view.advisorShowDone = event.target.checked; renderAdvisor(); });
    $(`[data-marketing-goals-form]`)?.addEventListener("submit", saveGoals);
    $(`[data-marketing-funnel-analytics]`)?.addEventListener("click", () => void fillFunnelFromAnalytics());
    $(`[data-marketing-performance-project]`)?.addEventListener("change", (event) => { state.view.performanceProject = event.target.value; void loadPerformance(); });
    $(`[data-marketing-goals-list]`)?.addEventListener("click", (event) => {
      event.target.closest("[data-marketing-goal-remove]")?.closest("[data-marketing-goal]")?.remove();
    });
    $$(`[data-marketing-goal-add]`).forEach((button) => button.addEventListener("click", () => addGoal(button.dataset.marketingGoalAdd)));
    $(`[data-marketing-reddit-new]`)?.addEventListener("click", () => openRedditThread());
    $(`[data-marketing-reddit-form]`)?.addEventListener("submit", saveRedditThread);
    $(`[data-marketing-reddit-delete]`)?.addEventListener("click", () => void deleteRedditThread());
    $(`[data-marketing-reddit-watch-edit]`)?.addEventListener("click", openRedditWatch);
    $(`[data-marketing-reddit-rules-copy]`)?.addEventListener("click", copyRedditRules);
    $(`[data-marketing-reddit-walk]`)?.addEventListener("click", toggleRedditWalk);
    $(`[data-marketing-reddit-posts-update]`)?.addEventListener("click", updateRedditPosts);
    $(`[data-marketing-reddit-post-new]`)?.addEventListener("click", () => openRedditPost());
    $(`[data-marketing-reddit-post-form]`)?.addEventListener("submit", saveRedditPost);
    $(`[data-marketing-reddit-post-delete]`)?.addEventListener("click", () => void deleteRedditPost());
    $(`[data-marketing-reddit-posts-more-button]`)?.addEventListener("click", () => { state.view.redditPostsAll = !state.view.redditPostsAll; renderRedditPosts(); });
    $(`[data-marketing-reddit-watch-form]`)?.addEventListener("submit", saveRedditWatch);
    $(`[data-marketing-link-copy]`)?.addEventListener("click", () => copyText($(`[data-marketing-link-output]`).textContent, "Link copied."));
    $(`[data-marketing-report-open]`)?.addEventListener("click", () => void openReport());
    $(`[data-marketing-report-copy]`)?.addEventListener("click", () => copyText($(`[data-marketing-report-text]`).value, "Report copied."));
    $(`[data-marketing-report-download]`)?.addEventListener("click", downloadReport);
    $(`[data-marketing-ads-search]`)?.addEventListener("input", renderAds);
    $(`[data-marketing-ads-channel-filter]`)?.addEventListener("change", (event) => { state.view.adChannel = event.target.value; renderAds(); });
    $(`[data-marketing-ads-currency]`)?.addEventListener("change", (event) => { state.view.adCurrency = event.target.value; renderAds(); });
    $(`[data-marketing-metric-channel]`)?.addEventListener("change", describePreviousSnapshot);
    $(`[data-marketing-refresh]`)?.addEventListener("click", () => { closeMenu(); void refreshFeeds(); });
    $(`[data-marketing-metric-form]`)?.addEventListener("submit", saveMetric);
    $$(`[data-marketing-settings-open]`).forEach((button) => button.addEventListener("click", () => { closeMenu(); openSettings(); }));
    $$(`[data-marketing-catalog-open]`).forEach((button) => button.addEventListener("click", () => { closeMenu(); showPage("catalog"); }));
    $$(`[data-marketing-capture-stale]`).forEach((button) => button.addEventListener("click", captureStale));
    $(`[data-marketing-campaign-new]`)?.addEventListener("click", () => openCampaign());
    $(`[data-marketing-campaign-form]`)?.addEventListener("submit", saveCampaign);
    $(`[data-marketing-campaign-delete]`)?.addEventListener("click", () => void deleteCampaign());
    $(`[data-marketing-keyword-form]`)?.addEventListener("submit", saveKeyword);
    $(`[data-marketing-keyword-delete]`)?.addEventListener("click", () => void deleteKeyword());
    $(`[data-marketing-keyword-import-form]`)?.addEventListener("submit", importKeywords);
    $(`[data-marketing-keyword-import-text]`)?.addEventListener("input", previewKeywordImport);
    $(`[data-marketing-keyword-import-limit]`)?.addEventListener("input", previewKeywordImport);
    $(`[data-marketing-assistant-form]`)?.addEventListener("submit", (event) => void runAssistant(event));
    $(`[data-marketing-agent-handoff]`)?.addEventListener("click", () => void handOff(campaignById($(`[data-marketing-assistant-campaign]`)?.value)));
    $(`[data-marketing-x-form]`)?.addEventListener("submit", postToX);
    $(`[data-marketing-x-text]`)?.addEventListener("input", updateXCount);
    $(`[data-marketing-x-content]`)?.addEventListener("change", (event) => { if (event.target.value) composeFromContent(event.target.value); });
    $(`[data-marketing-x-settings]`)?.addEventListener("click", openXSettings);
    $(`[data-marketing-x-connect-form]`)?.addEventListener("submit", saveXConnection);
    $(`[data-marketing-x-disconnect]`)?.addEventListener("click", () => void disconnectX());
    $(`[data-marketing-x-guide]`)?.addEventListener("click", startXSetup);
    view?.addEventListener("change", (event) => {
      const box = event.target.closest?.("[data-marketing-campaign-task]");
      if (box) void toggleCampaignTask(box.dataset.marketingCampaignTask, Number(box.dataset.index), box.checked);
    });
    $(`[data-marketing-channels-search]`)?.addEventListener("input", renderChannelHealth);
    $(`[data-marketing-channels-category]`)?.addEventListener("change", renderChannelHealth);
    $(`[data-marketing-dashboard-open]`)?.addEventListener("click", () => showPage("dashboard"));
    $(`[data-marketing-catalog-list]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-marketing-channel-add]");
      if (button) void addChannel(button.dataset.marketingChannelAdd);
    });
    $(`[data-marketing-catalog-search]`)?.addEventListener("input", renderCatalog);
    $(`[data-marketing-catalog-category]`)?.addEventListener("change", renderCatalog);
    $(`[data-marketing-settings-form]`)?.addEventListener("submit", saveSettings);
    $$(`[data-marketing-export-all]`).forEach((button) => button.addEventListener("click", () => { closeMenu(); void exportAll(); }));
    $(`[data-marketing-uninstall]`)?.addEventListener("click", () => void uninstallPlugin());
    for (const selector of ["search", "channel-filter", "campaign-filter"])
      $(`[data-marketing-${selector}]`)?.addEventListener(selector === "search" ? "input" : "change", () => { state.view.contentLimit = 50; renderContent(); });
    $$(`[data-marketing-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));

    // One delegated listener covers controls that are re-rendered from data.
    view?.addEventListener("click", (event) => {
      const target = event.target.closest("button");
      if (!target || !view.contains(target)) return;
      const data = target.dataset;
      if (data.marketingContentEdit) openContent(contentById(data.marketingContentEdit));
      else if (data.marketingCampaignEdit) openCampaign(campaignById(data.marketingCampaignEdit));
      else if ("marketingCampaignOpen" in data) openCampaign();
      else if (data.marketingCampaignTemplate) openCampaign(null, state.marketing.campaignTemplates[Number(data.marketingCampaignTemplate)]);
      else if (data.marketingCampaignTab) { state.view.campaignStatus = data.marketingCampaignTab; renderCampaigns(); }
      else if (data.marketingCampaignDraft) draftForCampaign(campaignById(data.marketingCampaignDraft));
      else if (data.marketingCampaignAsk) { void runAssistant(null, data.marketingCampaignAsk); $(`[data-marketing-assistant]`)?.scrollIntoView({ behavior: "smooth", block: "center" }); }
      else if (data.marketingCampaignHandoff) void handOff(campaignById(data.marketingCampaignHandoff));
      else if (data.marketingCampaignBrief) copyText(campaignBrief(campaignById(data.marketingCampaignBrief)), "Campaign brief copied as Markdown.");
      else if (data.marketingCampaignLink) copyText(campaignById(data.marketingCampaignLink)?.stats?.taggedLink || "", "Tagged link copied.");
      else if (data.marketingSuggestion) runSuggestion(Number(data.marketingSuggestion));
      else if (data.marketingAdvisorAction) runAction(state.marketing.advisor?.items?.[Number(data.marketingAdvisorAction)]?.action);
      else if (data.marketingAdvisorDone) markAdvisorDone(data.marketingAdvisorDone);
      else if (data.marketingAdvisorArea) { state.view.advisorArea = data.marketingAdvisorArea === "all" ? "" : data.marketingAdvisorArea; renderAdvisor(); }
      else if (data.marketingPlanAd) openAdFor(state.marketing.adPlan?.recommendations?.[Number(data.marketingPlanAd)] || {});
      else if (data.marketingPlanLink) copyText(state.marketing.adPlan?.recommendations?.[Number(data.marketingPlanLink)]?.link || "", "Tagged ad link copied.");
      else if (data.marketingReadinessAction) runAction(state.marketing.adPlan?.readiness?.[Number(data.marketingReadinessAction)]?.action);
      else if (data.marketingIdea) void applyIdea(Number(data.marketingIdea));
      else if (data.marketingKeywordEdit) openKeyword(keywordById(data.marketingKeywordEdit));
      else if (data.marketingKeywordTrack) openKeyword(null, data.marketingKeywordTrack);
      else if (data.marketingKeywordView) { state.view.keywordView = data.marketingKeywordView; renderKeywords(); }
      else if ("marketingKeywordImport" in data) openKeywordImport();
      else if ("marketingKeywordNew" in data) openKeyword();
      else if (data.marketingXCompose) composeFromContent(data.marketingXCompose);
      else if (data.marketingAppStoreRange) { state.view.appStoreRange = Number(data.marketingAppStoreRange); renderAppStore(); }
      else if (data.marketingAppStoreMetric) { state.view.appStoreMetric = data.marketingAppStoreMetric; renderAppStore(); }
      else if (data.marketingPage) showPage(data.marketingPage);
      else if (data.marketingAdEdit) openAd(adById(data.marketingAdEdit));
      else if (data.marketingAdResults) openAdResults(adById(data.marketingAdResults));
      else if ("marketingAdOpen" in data) openAd();
      else if (data.marketingExperimentEdit) openExperiment(experimentById(data.marketingExperimentEdit));
      else if ("marketingExperimentOpen" in data) openExperiment();
      else if ("marketingExperimentsTab" in data) { state.view.experimentStatus = data.marketingExperimentsTab; renderGrowth(); }
      else if (data.marketingFunnelEdit) openFunnel(funnelById(data.marketingFunnelEdit));
      else if (data.marketingFunnelDelete) void deleteFunnel(data.marketingFunnelDelete);
      else if ("marketingFunnelOpen" in data) openFunnel();
      else if (data.marketingRedditPostEdit) openRedditPost(redditPostById(data.marketingRedditPostEdit));
      else if (data.marketingRedditEdit) openRedditThread(redditThreadById(data.marketingRedditEdit));
      else if ("marketingRedditOpen" in data) openRedditThread();
      else if ("marketingRedditTab" in data) { state.view.redditStatus = data.marketingRedditTab; renderReddit(); }
      else if (data.marketingRedditCopy) copyText(redditThreadById(data.marketingRedditCopy)?.taggedLink || "", "Link copied.");
      else if (data.marketingAdsChannel) { state.view.adChannel = state.view.adChannel === data.marketingAdsChannel ? "" : data.marketingAdsChannel; renderAds(); }
      else if ("marketingAdsStatusTab" in data) { state.view.adStatus = data.marketingAdsStatusTab; renderAds(); }
      else if (data.marketingRange) { state.view.rangeDays = Number(data.marketingRange); state.marketing.traffic = null; render(); void loadTraffic(); }
      else if (data.marketingPerformanceRange) { state.view.performanceDays = Number(data.marketingPerformanceRange); renderPerformance(); void loadPerformance(); }
      else if (data.marketingChartMetric) { state.view.chartMetric = data.marketingChartMetric; render(); }
      else if (data.marketingChannelSort) { state.view.channelSort = data.marketingChannelSort; render(); }
      else if (data.marketingChannelLog) openMetric(data.marketingChannelLog);
      else if (data.marketingChannelContext) copyChannelContext(data.marketingChannelContext);
      else if (data.marketingChannelDelete) void deleteChannel(data.marketingChannelDelete);
      else if (data.marketingChannelCapture) startCapture(data.marketingChannelCapture);
      else if (data.marketingChannelRefresh) void refreshChannel(data.marketingChannelRefresh);
      else if ("marketingGuideLink" in data) openSettings();
      else if ("marketingReadyCopy" in data) copyText(shownReadyPost()?.text || "", "Post text copied.");
      else if ("marketingReadyShare" in data) shareReadyPost();
      else if (data.marketingReadyClose) void closeReadyPost(data.marketingReadyClose, target);
      else if ("marketingHealthTab" in data) { state.view.healthFilter = data.marketingHealthTab; renderChannelHealth(); }
      else if ("marketingMetricOpen" in data) openMetric();
      else if ("marketingStatusTab" in data) { state.view.status = data.marketingStatusTab; state.view.contentLimit = 50; renderContent(); }
      else if (data.marketingStage) { state.view.status = state.view.status === data.marketingStage ? "" : data.marketingStage; state.view.contentLimit = 50; renderContent(); }
      else if ("marketingContentMoreButton" in data) { state.view.contentLimit += 50; renderContent(); }
    });
    document.addEventListener("click", (event) => {
      const menu = $(`[data-marketing-menu]`);
      if (menu?.open && !menu.contains(event.target)) menu.open = false;
    });
  }
  return init();
});
