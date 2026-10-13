window.ForkMeshDashboard.registerPage("status-page", ({ select: $, selectAll: $$, session, escapeHtml }) => {
  const PACKAGE = "status-page";
  const PROBE_TIMEOUT_MS = 10000;
  const STATUS_LABELS = { operational: "Operational", degraded: "Degraded", down: "Down", unknown: "No data", checking: "Checking…" };
  const MONITORING_LABELS = { missing: "Sample missing", stale: "Monitoring delayed", empty: "No monitoring data", rateLimited: "Monitoring failed (rate limited)" };
  const MONITOR_ERRORS = {
    invalid_name: "Give the monitor a name of 1 to 40 characters.",
    invalid_url: "Enter an http or https address with a host name, up to 300 characters, without a user name or password.",
    too_many_monitors: "You already have as many monitors as Status Page allows. Remove one first.",
    monitor_not_found: "That monitor was already removed.",
    status_page_not_installed: "Status Page is not installed for your account.",
  };
  const state = {
    loaded: false, installed: false, packages: new Map(), market: [], systems: [], statusError: "",
    monitors: [], max: 20, probes: new Map(), busy: false,
  };
  const currentSession = () => session();

  function message(kind, value = "") {
    const node = $(`[data-status-${kind}]`); if (!node) return;
    node.textContent = value; node.classList.toggle("hidden", !value);
  }
  function monitorError(value = "") { message("monitor-error", value); }
  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    if (currentSession()?.sessionToken) headers.authorization = `Bearer ${currentSession().sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store", body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const code = String(payload.error || `HTTP ${response.status}`);
      const error = new Error(code.replaceAll("_", " "));
      error.status = response.status; error.code = code; error.payload = payload; throw error;
    }
    return payload;
  }
  function dependencyIds(row) {
    return (Array.isArray(row?.dependencies) ? row.dependencies : [])
      .map((item) => String(typeof item === "string" ? item : item?.id || ""));
  }
  function marketRow(id) { return state.market.find((row) => row.id === id) || null; }
  function checkRows() {
    return state.market.filter((row) => row.id !== PACKAGE && dependencyIds(row).includes(PACKAGE))
      .sort((a, b) => String(a.name || a.id).localeCompare(String(b.name || b.id)));
  }
  function installed(id) { return state.packages.get(id)?.installed === true; }
  function settings() { return state.packages.get(PACKAGE)?.settings || {}; }
  function onlyProblems() { return settings().onlyProblems === true; }
  function checkName(row) { return String(row?.name || row?.id || "").replace(/^Status:\s*/, ""); }

  function applyPackages(packages) {
    if (!Array.isArray(packages)) return;
    state.packages = new Map(packages.filter((item) => item && item.id).map((item) => [String(item.id), item]));
    const entry = state.packages.get(PACKAGE);
    state.installed = entry ? entry.installed !== false : true;
    syncNavigation();
  }
  function syncNavigation() {
    const account = String(currentSession()?.nodeName || "");
    if (account) window.ForkMeshAPI?.rememberStatusPage?.(account, state.installed);
    $$(`[data-plugin-nav="${PACKAGE}"]`).forEach((link) => link.classList.toggle("hidden", !state.installed));
    $$(`[data-plugin-navigation]`).forEach((group) => {
      group.classList.toggle("hidden", !group.querySelector("[data-plugin-nav]:not(.hidden)"));
    });
  }
  function renderInstall() {
    $(`[data-status-install-panel]`)?.classList.toggle("hidden", !state.loaded || state.installed);
    $(`[data-status-app]`)?.classList.toggle("hidden", !state.loaded || !state.installed);
    $$(`[data-status-requires-install]`).forEach((node) => node.classList.toggle("hidden", !state.loaded || !state.installed));
  }

  function watchedSystem(id) {
    const rows = checkRows();
    if (!rows.length) return true;
    return rows.some((row) => {
      if (!installed(row.id)) return false;
      const monitors = row.monitors || {};
      return (monitors.systems || []).includes(id)
        || (monitors.prefixes || []).some((prefix) => prefix && id.startsWith(prefix));
    });
  }
  function visualState(item) {
    const monitoring = String(item?.monitoringState || "");
    return monitoring ? "monitoring-" + monitoring.replace(/[A-Z]/g, (ch) => "-" + ch.toLowerCase()) : String(item?.status || "unknown");
  }
  function stateLabel(item) { return MONITORING_LABELS[item?.monitoringState] || STATUS_LABELS[item?.status] || "No data"; }
  function isGreen(item) { return item?.status === "operational" && !item?.monitoringState; }
  function dot(item, label) {
    return `<li class="sp-dot-item" title="${escapeHtml(`${label}: ${stateLabel(item)}`)}"><span class="sp-dot is-${escapeHtml(visualState(item))}" aria-hidden="true"></span><span>${escapeHtml(label)}</span><span class="sr-only">${escapeHtml(stateLabel(item))}</span></li>`;
  }
  function customItem(monitor) {
    const probe = state.probes.get(monitor.id);
    return { status: probe && probe.url === monitor.url ? probe.status : "checking" };
  }
  function renderDots() {
    const hideGreen = onlyProblems();
    const systems = state.systems.filter((system) => watchedSystem(String(system.id || "")));
    const custom = state.monitors.map((monitor) => ({ monitor, item: customItem(monitor) }));
    const shownSystems = systems.filter((system) => !hideGreen || !isGreen(system));
    const shownCustom = custom.filter(({ item }) => !hideGreen || !isGreen(item));
    $(`[data-status-dots]`).innerHTML = shownSystems.map((system) => dot(system, String(system.label || system.id))).join("");
    $(`[data-status-custom-dots]`).innerHTML = shownCustom.map(({ monitor, item }) => dot(item, String(monitor.name || monitor.url))).join("");
    $(`[data-status-custom-dots]`).classList.toggle("hidden", !shownCustom.length);
    const all = [...systems, ...custom.map(({ item }) => item)].filter((item) => item.status !== "checking");
    const problems = all.filter((item) => !isGreen(item)).length;
    $(`[data-status-summary]`).textContent = state.statusError ? `Could not read /api/status: ${state.statusError}`
      : !all.length ? "" : problems ? `${problems} of ${all.length} need attention` : `All ${all.length} operational`;
    const empty = $(`[data-status-empty]`);
    const nothingWatched = !systems.length && !custom.length;
    const text = shownSystems.length || shownCustom.length ? ""
      : nothingWatched ? (state.statusError ? "" : "No checks are switched on. Turn one on under Checks or add a monitor.")
      : "Everything you watch is operational. Only systems that need attention are shown.";
    empty.textContent = text; empty.classList.toggle("hidden", !text);
  }
  function renderChecks() {
    const rows = checkRows();
    $(`[data-status-checks]`).innerHTML = rows.length ? rows.map((row) => {
      const on = installed(row.id);
      return `<div class="flex items-center justify-between gap-3 rounded-md px-2 py-1.5 hover:bg-secondary">
        <span class="min-w-0"><span class="block text-sm text-foreground">${escapeHtml(checkName(row))}</span><span class="block truncate text-xs text-muted-foreground" title="${escapeHtml(row.summary || "")}">${escapeHtml(row.summary || "")}</span></span>
        <button type="button" role="switch" class="sp-switch" data-status-check="${escapeHtml(row.id)}" aria-checked="${on ? "true" : "false"}" aria-label="${escapeHtml(`${checkName(row)} check`)}"${state.busy ? " disabled" : ""}></button>
      </div>`;
    }).join("") : `<p class="text-sm text-muted-foreground">No checks are published yet.</p>`;
  }
  function renderSettings() {
    const declared = Array.isArray(marketRow(PACKAGE)?.settings) ? marketRow(PACKAGE).settings : [];
    const values = settings();
    $(`[data-status-settings]`).innerHTML = declared.length ? declared.map((field) => {
      const key = escapeHtml(field.key), label = escapeHtml(field.label || field.key);
      const value = Object.prototype.hasOwnProperty.call(values, field.key) ? values[field.key] : field.default;
      if (field.type === "boolean") {
        return `<label class="flex items-center justify-between gap-3 text-sm text-foreground"><span>${label}</span><input type="checkbox" data-status-setting="${key}" data-status-setting-type="boolean" class="h-4 w-4"${value === true ? " checked" : ""}${state.busy ? " disabled" : ""}></label>`;
      }
      if (field.type === "choice") {
        const options = (field.options || []).map((option) => `<option value="${escapeHtml(option.value)}"${String(option.value) === String(value) ? " selected" : ""}>${escapeHtml(option.label || option.value)}</option>`).join("");
        return `<label class="flex flex-wrap items-center justify-between gap-3 text-sm text-foreground"><span>${label}</span><select data-status-setting="${key}" data-status-setting-type="choice" class="rounded-md border border-border bg-background px-2 py-1.5 text-sm text-foreground"${state.busy ? " disabled" : ""}>${options}</select></label>`;
      }
      return "";
    }).join("") : `<p class="text-sm text-muted-foreground">Status Page has no settings.</p>`;
  }
  function safeLink(url) { return /^https?:\/\//i.test(String(url || "")); }
  function renderMonitors() {
    $(`[data-status-monitor-count]`).textContent = `${state.monitors.length} of ${state.max}`;
    $(`[data-status-monitors]`).innerHTML = state.monitors.length ? state.monitors.map((monitor) => {
      const url = String(monitor.url || "");
      const address = safeLink(url)
        ? `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer" class="block truncate text-xs text-muted-foreground hover:underline" title="${escapeHtml(url)}">${escapeHtml(url)}</a>`
        : `<span class="block truncate text-xs text-muted-foreground">${escapeHtml(url)}</span>`;
      return `<div class="flex items-center justify-between gap-3 rounded-md px-2 py-1.5 hover:bg-secondary">
        <span class="flex min-w-0 items-center gap-2"><span class="sp-dot is-${escapeHtml(visualState(customItem(monitor)))}" aria-hidden="true"></span><span class="min-w-0"><span class="block text-sm text-foreground">${escapeHtml(monitor.name || "")}</span>${address}</span></span>
        <button type="button" data-status-monitor-remove="${escapeHtml(monitor.id)}" class="flex-none rounded-md border border-border px-2 py-1 text-xs text-muted-foreground hover:text-destructive">Remove</button>
      </div>`;
    }).join("") : `<p class="text-sm text-muted-foreground">No monitors yet. Add a site below to get a dot for it.</p>`;
    const full = state.monitors.length >= state.max;
    $$(`[data-status-monitor-form] input, [data-status-monitor-form] button`).forEach((node) => { node.disabled = full; });
  }
  function render() {
    renderInstall();
    if (!state.installed) return;
    renderDots(); renderChecks(); renderSettings(); renderMonitors();
    window.lucide?.createIcons?.();
  }

  async function probe(monitor) {
    const url = String(monitor.url || "");
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), PROBE_TIMEOUT_MS);
    let status = "down";
    try { await fetch(url, { mode: "no-cors", cache: "no-store", credentials: "omit", referrerPolicy: "no-referrer", signal: controller.signal }); status = "operational"; }
    catch { status = "down"; }
    finally { clearTimeout(timer); }
    if (!state.monitors.some((item) => item.id === monitor.id && item.url === url)) return;
    state.probes.set(monitor.id, { url, status });
    if (state.installed) { renderDots(); renderMonitors(); }
  }
  function probeAll() {
    state.probes.clear();
    state.monitors.forEach((monitor) => void probe(monitor));
  }
  async function loadStatus() {
    try {
      const response = await fetch("/api/status", { headers: { accept: "application/json" }, cache: "no-store" });
      const payload = await response.json();
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      state.systems = Array.isArray(payload.systems) ? payload.systems : [];
      state.statusError = "";
    } catch (error) {
      state.statusError = error.message || "request failed";
    }
  }
  async function loadMonitors() {
    try {
      const payload = await request("/api/status-monitors");
      state.monitors = Array.isArray(payload.monitors) ? payload.monitors : [];
      state.max = Number(payload.max || state.max);
    } catch (error) {
      if (error.code === "status_page_not_installed") { state.installed = false; syncNavigation(); }
      else message("error", `Could not load your monitors: ${error.message}`);
    }
  }
  async function loadMarket() {
    try { state.market = (await request("/api/market/packages")).packages || []; }
    catch (error) { message("error", `Could not load the checks: ${error.message}`); }
  }
  async function load() {
    const button = $(`[data-status-refresh]`); if (button) button.disabled = true;
    try {
      const [account] = await Promise.all([request("/api/plugins/@account"), loadMarket()]);
      applyPackages(account.packages || []);
      state.loaded = true; message("error", "");
      if (state.installed) {
        await Promise.all([loadStatus(), loadMonitors()]);
        probeAll();
      } else {
        state.systems = []; state.monitors = []; state.probes.clear();
      }
      render();
    } catch (error) {
      message("error", `Status Page is unavailable: ${error.message}`);
    } finally { if (button) button.disabled = false; }
  }
  async function lifecycle(id, action) {
    const payload = await request(`/api/plugins/@account/${encodeURIComponent(id)}/${action}`, "POST", {});
    applyPackages(payload.packages);
    return payload;
  }
  function changedNames(payload, except) {
    return (Array.isArray(payload?.changed) ? payload.changed : []).filter((id) => id !== except)
      .map((id) => id === PACKAGE ? "Status Page" : checkName(marketRow(id)) || id);
  }
  async function install() {
    const button = $(`[data-status-install]`); if (button) button.disabled = true;
    try { await lifecycle(PACKAGE, "install"); message("notice", "Status Page installed on all your devices, with the default checks switched on."); await load(); }
    catch (error) { message("error", `Could not install Status Page: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }
  async function removeStatusPage() {
    if (!confirm("Remove Status Page? This removes the status dots, all checks and your monitors from every device on your account: the web, the desktop app and the mobile app.")) return;
    state.busy = true; render();
    try {
      await lifecycle(PACKAGE, "uninstall");
      state.systems = []; state.monitors = []; state.probes.clear();
      message("notice", "Status Page removed from all your devices.");
    } catch (error) { message("error", `Could not remove Status Page: ${error.message}`); }
    finally { state.busy = false; render(); }
  }
  async function toggleCheck(id) {
    if (state.busy) return;
    const on = installed(id);
    state.busy = true; renderChecks();
    try {
      const payload = await lifecycle(id, on ? "uninstall" : "install");
      const name = checkName(marketRow(id));
      const others = changedNames(payload, id);
      message("notice", `Turned ${on ? "off" : "on"} the ${name} check on all your devices.${others.length ? ` Also ${on ? "removed" : "installed"}: ${others.join(", ")}.` : ""}`);
      if (!on && !state.systems.length && !state.statusError) await loadStatus();
    } catch (error) { message("error", `Could not change the ${checkName(marketRow(id))} check: ${error.message}`); }
    finally { state.busy = false; render(); }
  }
  async function saveSetting(control) {
    const key = control.dataset.statusSetting;
    const value = control.dataset.statusSettingType === "boolean" ? control.checked : control.value;
    state.busy = true;
    try {
      const payload = await request(`/api/plugins/@account/${PACKAGE}/settings`, "POST", { settings: { [key]: value } });
      applyPackages(payload.packages);
      message("notice", "Settings saved for all your devices.");
    } catch (error) {
      message("error", error.code === "invalid_setting" ? "That setting value is not allowed." : `Could not save the setting: ${error.message}`);
    } finally { state.busy = false; render(); }
  }
  async function addMonitor(event) {
    event.preventDefault();
    const nameInput = $(`[data-status-monitor-name]`), urlInput = $(`[data-status-monitor-url]`);
    const name = nameInput.value.trim(), url = urlInput.value.trim();
    if (!name || name.length > 40) return monitorError(MONITOR_ERRORS.invalid_name);
    if (!safeLink(url)) return monitorError(MONITOR_ERRORS.invalid_url);
    monitorError("");
    try {
      const payload = await request("/api/status-monitors/monitors", "POST", { name, url });
      state.monitors = Array.isArray(payload.monitors) ? payload.monitors : state.monitors;
      nameInput.value = ""; urlInput.value = "";
      if (payload.monitor) void probe(payload.monitor);
      render();
    } catch (error) {
      monitorError(MONITOR_ERRORS[error.code] || `Could not add the monitor: ${error.message}`);
      if (error.code === "status_page_not_installed") await load();
    }
  }
  async function removeMonitor(id) {
    monitorError("");
    try {
      const payload = await request(`/api/status-monitors/monitors/${encodeURIComponent(id)}`, "DELETE");
      state.monitors = Array.isArray(payload.monitors) ? payload.monitors : state.monitors.filter((item) => item.id !== id);
      state.probes.delete(id); render();
    } catch (error) {
      if (error.code === "monitor_not_found") { await loadMonitors(); render(); return; }
      monitorError(MONITOR_ERRORS[error.code] || `Could not remove the monitor: ${error.message}`);
    }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Status Page.");
    $(`[data-status-install]`)?.addEventListener("click", () => void install());
    $(`[data-status-refresh]`)?.addEventListener("click", () => { message("notice", ""); void load(); });
    $(`[data-status-remove]`)?.addEventListener("click", () => void removeStatusPage());
    $(`[data-status-checks]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-status-check]"); if (button && !button.disabled) void toggleCheck(button.dataset.statusCheck);
    });
    $(`[data-status-settings]`)?.addEventListener("change", (event) => {
      const control = event.target.closest("[data-status-setting]"); if (control) void saveSetting(control);
    });
    $(`[data-status-monitor-form]`)?.addEventListener("submit", (event) => void addMonitor(event));
    $(`[data-status-monitors]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-status-monitor-remove]"); if (button) void removeMonitor(button.dataset.statusMonitorRemove);
    });
    window.lucide?.createIcons?.();
    await load();
  }
  return init();
});
