// Competitor Watch plugin: organization-private competitive intelligence.
window.ForkMeshDashboard.registerPage("competitors", ({
  select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel,
}) => {
  const state = { organizations: [], organization: "", competitors: [], summary: {} };
  const currentSession = () => session();

  async function request(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    if (currentSession()?.sessionToken) headers.authorization = `Bearer ${currentSession().sessionToken}`;
    if (body !== null) headers["content-type"] = "application/json";
    const response = await fetch(path, { method, headers, cache: "no-store",
      body: body === null ? undefined : JSON.stringify(body) });
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
      error.status = response.status; throw error;
    }
    return payload;
  }
  function message(kind, value = "") {
    const node = $(`[data-competitor-${kind}]`); if (!node) return;
    const slot = node.querySelector("[data-competitor-message-text]") || node;
    slot.textContent = value; node.classList.toggle("hidden", !value);
  }
  function selectedOrg() { return state.organizations.find((org) => org.name === state.organization); }
  function canManage() { return ["owner", "admin"].includes(selectedOrg()?.role || ""); }
  function orgPath(suffix = "") { return `/api/competitors/${encodeURIComponent(state.organization)}${suffix}`; }
  function pluginPath(suffix = "") { return `/api/plugins/${encodeURIComponent(state.organization)}/competitor${suffix}`; }
  function byId(id) { return state.competitors.find((item) => item.id === id); }
  function downloadJson(payload, filename) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url; link.download = filename; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  function renderInstall() {
    const installed = Boolean(selectedOrg()?.installed);
    $(`[data-competitor-install-panel]`)?.classList.toggle("hidden", installed);
    $(`[data-competitor-app]`)?.classList.toggle("hidden", !installed);
    $$(`[data-competitor-requires-install]`).forEach((node) => node.classList.toggle("hidden", !installed));
    const org = $(`[data-competitor-install-org]`); if (org) org.textContent = state.organization;
    $(`[data-competitor-install]`)?.classList.toggle("hidden", !canManage());
    $(`[data-competitor-install-denied]`)?.classList.toggle("hidden", canManage());
  }
  function renderList() {
    const query = String($(`[data-competitor-search]`)?.value || "").trim().toLowerCase();
    const status = $(`[data-competitor-status-filter]`)?.value || "";
    const items = state.competitors.filter((item) => {
      if (status && item.status !== status) return false;
      return !query || [item.name, item.category, item.positioning, item.strengths, item.weaknesses, item.notes]
        .some((value) => String(value || "").toLowerCase().includes(query));
    });
    const host = $(`[data-competitor-list]`); if (!host) return;
    host.innerHTML = items.map((item) => `<tr class="hover:bg-secondary"><td class="px-4 py-3"><button data-competitor-edit="${escapeHtml(item.id)}" class="block text-left font-semibold text-foreground">${escapeHtml(item.name)}</button>${item.website?.startsWith("https://") ? `<a href="${escapeHtml(item.website)}" target="_blank" rel="noopener noreferrer" class="mt-1 inline-block text-xs text-primary">Open website ↗</a>` : ""}</td><td class="px-4 py-3 text-muted-foreground">${escapeHtml(item.category || "—")}</td><td class="max-w-lg px-4 py-3 text-foreground">${escapeHtml(item.positioning || "—")}</td><td class="px-4 py-3 text-xs text-muted-foreground">${item.lastReviewedAt ? escapeHtml(relativeTimeLabel(item.lastReviewedAt)) : "Never"}</td><td class="px-4 py-3"><span class="fm-label${item.status === "primary" ? " fm-label-accent" : ""}">${escapeHtml(item.status)}</span></td><td class="px-4 py-3 text-right"><button data-competitor-edit="${escapeHtml(item.id)}" class="fm-btn fm-btn-sm">Edit</button></td></tr>`).join("");
    $(`[data-competitor-empty]`)?.classList.toggle("hidden", items.length > 0);
  }
  function render() {
    for (const key of ["watchlist", "primary", "needsReview", "archived"]) {
      const node = $(`[data-competitor-stat-${key}]`); if (node) node.textContent = Number(state.summary[key] || 0).toLocaleString();
    }
    renderList();
  }
  function openEditor(item = null) {
    $(`[data-competitor-dialog-title]`).textContent = item ? "Edit competitor" : "Add competitor";
    $(`[data-competitor-id]`).value = item?.id || "";
    for (const key of ["name", "website", "category", "positioning", "strengths", "weaknesses", "notes"])
      $(`[data-competitor-${key}]`).value = item?.[key] || "";
    $(`[data-competitor-status]`).value = item?.status || "watch";
    $(`[data-competitor-reviewed]`).checked = !item;
    $(`[data-competitor-delete]`).classList.toggle("hidden", !item);
    $(`[data-competitor-dialog]`).showModal();
  }
  async function save(event) {
    event.preventDefault(); const id = $(`[data-competitor-id]`).value;
    const body = { status: $(`[data-competitor-status]`).value };
    for (const key of ["name", "website", "category", "positioning", "strengths", "weaknesses", "notes"])
      body[key] = $(`[data-competitor-${key}]`).value;
    if ($(`[data-competitor-reviewed]`).checked) body.lastReviewedAt = Date.now();
    try { await request(orgPath(`/items${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-competitor-dialog]`).close(); message("notice", "Competitor saved."); await load(); }
    catch (error) { message("error", `Could not save competitor: ${error.message}`); }
  }
  async function remove() {
    const id = $(`[data-competitor-id]`).value;
    if (!id || !confirm("Delete this competitor profile?")) return;
    try { await request(orgPath(`/items/${id}`), "DELETE", {}); $(`[data-competitor-dialog]`).close();
      message("notice", "Competitor deleted."); await load(); }
    catch (error) { message("error", `Could not delete competitor: ${error.message}`); }
  }
  async function install() {
    try { await request(pluginPath("/install"), "POST", {}); selectedOrg().installed = true;
      message("notice", "Competitor Watch installed."); await load(); }
    catch (error) { message("error", `Could not install: ${error.message}`); }
  }
  async function exportAll() {
    try { const payload = await request(pluginPath("/export"));
      downloadJson(payload.export || {}, payload.filename || "forkmesh-competitors.json"); }
    catch (error) { message("error", `Export failed: ${error.message}`); }
  }
  async function uninstall() {
    if (!confirm(`Uninstall Competitor Watch from ${state.organization}? An export downloads first, then its stored data is removed.`)) return;
    try { const snapshot = await request(pluginPath("/export"));
      if (snapshot.hasData) downloadJson(snapshot.export || {}, snapshot.filename);
      await request(pluginPath("/uninstall"), "POST", {}); selectedOrg().installed = false;
      $(`[data-competitor-manage-dialog]`).close(); message("notice", "Competitor Watch uninstalled."); await load(); }
    catch (error) { message("error", `Could not uninstall: ${error.message}`); }
  }
  async function load() {
    renderInstall(); if (!selectedOrg()?.installed) return;
    try { const payload = await request(orgPath()); state.competitors = payload.competitors || [];
      state.summary = payload.summary || {}; message("error", ""); render(); }
    catch (error) { if (error.status === 409) { selectedOrg().installed = false; renderInstall(); }
      else message("error", `Competitor Watch is unavailable: ${error.message}`); }
  }
  async function init() {
    if (!currentSession()?.sessionToken) return message("error", "Sign in to use Competitor Watch.");
    try { const payload = await request("/api/competitors"); state.organizations = payload.organizations || [];
      if (!state.organizations.length) return message("error", "Join or create an organization before using Competitor Watch.");
      const select = $(`[data-competitor-organization]`); select.innerHTML = state.organizations.map((org) => `<option value="${escapeHtml(org.name)}">${escapeHtml(org.name)} · ${escapeHtml(org.role)}${org.installed ? "" : " · not installed"}</option>`).join("");
      state.organization = state.organizations[0].name; select.value = state.organization;
      select.addEventListener("change", () => { state.organization = select.value; void load(); }); await load(); }
    catch (error) { return message("error", `Competitor Watch is unavailable: ${error.message}`); }
    $(`[data-competitor-install]`)?.addEventListener("click", () => void install());
    $(`[data-competitor-new]`)?.addEventListener("click", () => openEditor());
    $(`[data-competitor-list]`)?.addEventListener("click", (event) => { const button = event.target.closest("[data-competitor-edit]"); if (button) openEditor(byId(button.dataset.competitorEdit)); });
    $(`[data-competitor-form]`)?.addEventListener("submit", save);
    $(`[data-competitor-delete]`)?.addEventListener("click", () => void remove());
    $(`[data-competitor-manage]`)?.addEventListener("click", () => $(`[data-competitor-manage-dialog]`).showModal());
    $(`[data-competitor-export]`)?.addEventListener("click", () => void exportAll());
    $(`[data-competitor-uninstall]`)?.addEventListener("click", () => void uninstall());
    $(`[data-competitor-search]`)?.addEventListener("input", renderList);
    $(`[data-competitor-status-filter]`)?.addEventListener("change", renderList);
    $$(`[data-competitor-dialog-close]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
  }
  return init();
});
