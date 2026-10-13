// 09-apps.js, loaded by dashboard.js when its page runs (app/src/dashboard_bundle.py LAZY_CHUNKS).
(() => {
  const chunks = window.__forkmeshDashboardChunks = window.__forkmeshDashboardChunks || {};
  chunks["apps"] = (core) => {
  const { $, state } = core;
  const APPS_ERRORS = Object.freeze({
    invalid_name: "Give the app a name of 2 to 40 characters.",
    invalid_redirect_uri: "Redirect URLs must be https, without a fragment or credentials.",
    invalid_redirect_uris: "Add between 1 and 10 redirect URLs.",
    invalid_scopes: "Choose at least one scope.",
    invalid_events: "One of the events is unknown.",
    event_scope_missing: "An event needs its scope: issue events need issues:read, chat events chat:read, and so on.",
    invalid_event_url: "The event URL must be a public https address.",
    invalid_interactivity_url: "The interactivity URL must be a public https address.",
    invalid_commands: "Commands are /name | https://url | description; names are lowercase and cannot shadow built-in commands.",
    commands_scope_missing: "Slash commands need the commands scope.",
    invalid_homepage_url: "The homepage must be an https address.",
    content_changed: "The app changed while you were reviewing it. Reload the queue.",
    reason_required: "Give a reason: the developer sees it.",
    invalid_icon: "The icon must be one letter or emoji and a color.",
    slug_taken: "Another app already uses that name.",
    too_many_apps: "You have reached the app limit.",
    not_org_admin: "Only an owner or admin of that organization can do this.",
    invalid_places: "Choose at least one place this plugin can show on.",
    plugin_not_installed: "Install the plugin first.",
  });

  async function appsRequest(method, path, body) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token && token !== "cookie") headers.authorization = `Bearer ${token}`;
    const init = { method, headers, cache: "no-store", credentials: "same-origin" };
    if (body !== undefined) {
      headers["content-type"] = "application/json";
      init.body = JSON.stringify(body);
    }
    const response = await fetch(path, init);
    const payload = await response.json().catch(() => ({}));
    if (!response.ok || payload.ok === false) {
      const error = new Error(APPS_ERRORS[payload.error] || payload.error || `HTTP ${response.status}`);
      error.status = response.status;
      error.code = String(payload.error || "");
      throw error;
    }
    return payload;
  }

  function appsEl(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = String(text);
    return node;
  }

  function appsButton(label, onClick, danger) {
    const button = appsEl("button", "inline-flex items-center gap-1.5 rounded-md border border-border px-3 py-1.5 text-xs font-semibold hover:bg-secondary transition-colors " + (danger ? "text-red-500" : "text-muted-foreground hover:text-foreground"), label);
    button.type = "button";
    button.addEventListener("click", onClick);
    return button;
  }

  function setAppsNotice(selector, message, isError) {
    const notice = $(selector);
    if (!notice) return;
    notice.hidden = !message;
    notice.textContent = message || "";
    notice.classList.toggle("text-red-500", Boolean(isError));
    notice.classList.toggle("text-muted-foreground", !isError);
  }

  function appsWhen(ms) {
    if (!ms) return "never";
    const seconds = Math.max(0, Math.round((Date.now() - ms) / 1000));
    if (seconds < 90) return `${seconds}s ago`;
    const minutes = Math.round(seconds / 60);
    if (minutes < 90) return `${minutes} min ago`;
    const hours = Math.round(minutes / 60);
    if (hours < 48) return `${hours} h ago`;
    return new Date(ms).toLocaleDateString();
  }

  function appsIcon(app) {
    const box = appsEl("div", "shrink-0 h-9 w-9 rounded-md flex items-center justify-center text-sm font-semibold text-white overflow-hidden");
    const icon = app?.icon || {};
    if (icon.data && /^data:image\/(png|webp);base64,/.test(icon.data)) {
      const img = document.createElement("img");
      img.alt = "";
      img.src = icon.data;
      img.className = "h-full w-full object-cover";
      box.append(img);
      return box;
    }
    box.style.background = /^#[0-9a-f]{6}$/i.test(icon.color || "") ? icon.color : "#6366f1";
    box.textContent = icon.glyph || String(app?.name || "?").slice(0, 1).toUpperCase();
    return box;
  }

  function appsHeader(app, subtitle) {
    const head = appsEl("div", "flex items-start gap-3 min-w-0");
    const text = appsEl("div", "min-w-0");
    text.append(appsEl("p", "text-sm font-semibold text-foreground", app?.name || "App"));
    if (subtitle) text.append(appsEl("p", "text-xs text-muted-foreground", subtitle));
    head.append(appsIcon(app), text);
    return head;
  }

  function appsScopeList(scopes) {
    const list = appsEl("div", "flex flex-wrap gap-1");
    for (const scope of scopes || []) {
      const chip = appsEl("span", "rounded border border-border px-1.5 py-0.5 font-mono text-[11px] text-muted-foreground", scope.scope || scope);
      if (scope.label) chip.title = scope.label;
      list.append(chip);
    }
    return list;
  }

  function appsInstallCard(install, onRevoke) {
    const card = appsEl("div", "rounded-md border border-border p-3 grid gap-2");
    const top = appsEl("div", "flex flex-col sm:flex-row sm:items-start sm:justify-between gap-2");
    top.append(appsHeader(install.app, `Installed by @${install.installedBy || "?"} · last used ${appsWhen(install.lastUsedAt)}`));
    if (install.canRevoke) {
      top.append(appsButton("Revoke", () => onRevoke(install), true));
    }
    card.append(top, appsScopeList(install.scopes));
    return card;
  }

  async function revokeAppInstall(install) {
    if (!window.confirm(`Revoke ${install.app?.name || "this app"}? Its tokens stop working immediately.`)) return;
    try {
      await appsRequest("DELETE", `/api/apps/installs/${install.installId}`);
      setAppsNotice("[data-apps-notice]", `${install.app?.name || "The app"} was revoked.`);
    } catch (error) {
      setAppsNotice("[data-apps-notice]", error.message, true);
    }
    void loadInstalledApps();
  }

  function renderOrgApps(org) {
    const block = appsEl("div", "grid gap-3");
    block.append(appsEl("h2", "text-sm font-semibold text-foreground", `${org.name} (${org.role})`));
    const installs = org.installs || [];
    if (!installs.length) block.append(appsEl("p", "text-sm text-muted-foreground", "No apps installed for this organization."));
    for (const install of installs) block.append(appsInstallCard(install, revokeAppInstall));
    if (!org.canManage) return block;
    for (const request of org.requests || []) {
      const card = appsEl("div", "rounded-md border border-border p-3 grid gap-2");
      const top = appsEl("div", "flex flex-col sm:flex-row sm:items-start sm:justify-between gap-2");
      top.append(appsHeader(request.app, `@${request.requester} asked to install this for ${org.name}`));
      const actions = appsEl("div", "flex gap-2");
      const decide = async (action) => {
        try {
          await appsRequest("POST", `/api/apps/orgs/${encodeURIComponent(org.name)}/requests/${request.id}`, { action });
          setAppsNotice("[data-apps-notice]", action === "approve" ? "Approved. The member can finish installing." : "Request denied.");
        } catch (error) {
          setAppsNotice("[data-apps-notice]", error.message, true);
        }
        void loadInstalledApps();
      };
      actions.append(appsButton("Approve", () => decide("approve")), appsButton("Deny", () => decide("deny"), true));
      top.append(actions);
      card.append(top, appsScopeList(request.scopes));
      block.append(card);
    }
    const policy = org.policy || { requestPolicy: "any", allowlist: [] };
    const form = appsEl("form", "flex flex-col sm:flex-row sm:items-end gap-2");
    const label = appsEl("label", "grid gap-1 text-xs text-muted-foreground", "Members may request");
    const select = appsEl("select", "rounded-md border border-border bg-background px-2 py-1.5 text-sm text-foreground");
    for (const [value, text] of [["any", "any app"], ["allowlist", "only allowlisted apps"], ["none", "no apps"]]) {
      const option = appsEl("option", "", text);
      option.value = value;
      option.selected = policy.requestPolicy === value;
      select.append(option);
    }
    label.append(select);
    const allow = appsEl("label", "grid gap-1 text-xs text-muted-foreground min-w-0 flex-1", "Allowlisted app ids (comma separated)");
    const input = appsEl("input", "rounded-md border border-border bg-background px-2 py-1.5 font-mono text-xs text-foreground");
    input.value = (policy.allowlist || []).join(", ");
    allow.append(input);
    const save = appsEl("button", "inline-flex items-center justify-center rounded-md bg-primary px-3 py-2 text-xs font-semibold text-primary-foreground", "Save policy");
    save.type = "submit";
    form.append(label, allow, save);
    form.addEventListener("submit", async (event) => {
      event.preventDefault();
      try {
        await appsRequest("PUT", `/api/apps/orgs/${encodeURIComponent(org.name)}/policy`, {
          requestPolicy: select.value,
          allowlist: input.value.split(/[\s,]+/).filter(Boolean),
        });
        setAppsNotice("[data-apps-notice]", `Saved the app policy for ${org.name}.`);
      } catch (error) {
        setAppsNotice("[data-apps-notice]", error.message, true);
      }
    });
    block.append(form);
    return block;
  }

  async function loadInstalledApps() {
    const list = $("[data-apps-installed]");
    const orgs = $("[data-apps-orgs]");
    if (!list) return;
    try {
      const payload = await appsRequest("GET", "/api/apps/installed");
      list.textContent = "";
      if (!(payload.installs || []).length) list.append(appsEl("p", "text-sm text-muted-foreground", "You have not installed any apps."));
      for (const install of payload.installs || []) list.append(appsInstallCard(install, revokeAppInstall));
      orgs.textContent = "";
      orgs.hidden = !(payload.organizations || []).length;
      for (const org of payload.organizations || []) orgs.append(renderOrgApps(org));
    } catch (error) {
      list.textContent = "";
      list.append(appsEl("p", "text-sm text-muted-foreground", error.status === 401 ? "Sign in to see your apps." : error.message));
    }
  }

  async function loadAppDirectory() {
    const box = $("[data-apps-directory]");
    if (!box) return;
    try {
      const payload = await appsRequest("GET", "/api/apps/directory");
      box.textContent = "";
      if (!(payload.apps || []).length) box.append(appsEl("p", "text-sm text-muted-foreground", "No apps are listed yet."));
      for (const app of payload.apps || []) {
        const card = appsEl("div", "rounded-md border border-border p-3 grid gap-2");
        const rating = app.rating?.count ? `${app.rating.average.toFixed(1)} stars from ${app.rating.count}` : "not rated yet";
        const top = appsEl("div", "flex flex-col sm:flex-row sm:items-start sm:justify-between gap-2");
        top.append(appsHeader(app, `${app.organization || app.developer} · ${rating}`));
        const links = appsEl("div", "flex gap-2");
        if (app.homepageUrl) {
          const visit = appsEl("a", "inline-flex items-center rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-muted-foreground hover:bg-secondary", "Visit to install");
          visit.href = app.homepageUrl;
          visit.rel = "noopener noreferrer";
          visit.target = "_blank";
          links.append(visit);
        }
        const rate = appsEl("a", "inline-flex items-center rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-muted-foreground hover:bg-secondary", "Rate");
        rate.href = `/market-reviews?market=forkmesh-apps&package=${encodeURIComponent(app.slug)}`;
        links.append(rate);
        top.append(links);
        card.append(top);
        if (app.description) card.append(appsEl("p", "text-sm text-muted-foreground", app.description));
        card.append(appsScopeList(app.scopes));
        box.append(card);
      }
    } catch (error) {
      box.textContent = "";
      box.append(appsEl("p", "text-sm text-muted-foreground", error.message));
    }
  }

  async function loadAppReviewQueue() {
    const panel = $("[data-apps-review-panel]");
    const box = $("[data-apps-review]");
    if (!panel || !box) return;
    let payload;
    try {
      payload = await appsRequest("GET", "/api/apps/review");
    } catch (_) {
      panel.hidden = true;
      return;
    }
    panel.hidden = false;
    box.textContent = "";
    if (!(payload.apps || []).length) box.append(appsEl("p", "text-sm text-muted-foreground", "Nothing is waiting for review."));
    for (const app of payload.apps || []) {
      const card = appsEl("div", "rounded-md border border-border p-3 grid gap-2");
      const top = appsEl("div", "flex flex-col sm:flex-row sm:items-start sm:justify-between gap-2");
      top.append(appsHeader(app, `by @${app.developer} · ${app.listing}`));
      const reason = appsEl("input", "rounded-md border border-border bg-background px-2 py-1.5 text-xs text-foreground");
      reason.placeholder = "Reason (required to reject or suspend)";
      const actions = appsEl("div", "flex flex-wrap gap-2");
      const act = async (action) => {
        try {
          await appsRequest("POST", `/api/apps/${app.id}/review`, { action, reason: reason.value, contentHash: app.contentHash });
          setAppsNotice("[data-apps-notice]", `${app.name}: ${action} recorded.`);
        } catch (error) {
          setAppsNotice("[data-apps-notice]", error.message, true);
        }
        void loadAppReviewQueue();
        void loadAppDirectory();
      };
      if (app.listing === "pending") {
        actions.append(appsButton("Approve", () => act("approve")), appsButton("Reject", () => act("reject"), true));
      }
      if (app.listing === "suspended") actions.append(appsButton("Reinstate", () => act("reinstate")));
      else actions.append(appsButton("Suspend", () => act("suspend"), true));
      top.append(actions);
      card.append(top);
      if (app.description) card.append(appsEl("p", "text-sm text-muted-foreground", app.description));
      card.append(appsScopeList(app.scopes));
      for (const url of [...(app.redirectUris || []), app.eventUrl, app.interactivityUrl].filter(Boolean)) {
        card.append(appsEl("p", "font-mono text-[11px] text-muted-foreground break-all", url));
      }
      card.append(reason);
      box.append(card);
    }
  }

  const appsPlugins = { market: [], installed: new Map(), places: new Map(), available: new Map(), busy: false, error: "", loaded: false };
  const APPS_PLACES = Object.freeze([["mobile", "Phone"], ["desktop", "Desktop"], ["web", "Web"]]);

  function appsPlaceList(value) {
    return Array.isArray(value) ? APPS_PLACES.map(([place]) => place).filter((place) => value.includes(place)) : [];
  }

  function appsPlaceNames(places) {
    return APPS_PLACES.filter(([place]) => places.includes(place)).map(([, label]) => label).join(", ");
  }

  function appsPluginAvailable(id) {
    const given = appsPlugins.available.get(id) || [];
    if (given.length) return given;
    const market = appsPlaceList(appsPlugins.market.find((row) => row?.id === id)?.places);
    return market.length ? market : APPS_PLACES.map(([place]) => place);
  }

  function appsPluginPlaces(id) {
    const available = appsPluginAvailable(id);
    const chosen = (appsPlugins.places.get(id) || []).filter((place) => available.includes(place));
    return chosen.length ? chosen : available;
  }

  function appsPluginOnWeb(id) {
    return appsPlugins.installed.get(id) === true && appsPluginPlaces(id).includes("web");
  }

  function appsPluginDependencies(row) {
    return (Array.isArray(row?.dependencies) ? row.dependencies : [])
      .map((item) => String(typeof item === "string" ? item : item?.id || ""))
      .filter(Boolean);
  }

  function appsPluginRows() {
    return appsPlugins.market.filter((row) => row?.accountSync === true && appsPlugins.installed.has(row.id));
  }

  function appsPluginChildren(id) {
    return appsPluginRows().filter((row) => row.id !== id && appsPluginDependencies(row)[0] === id);
  }

  function appsPluginDependents(id) {
    const rows = appsPluginRows();
    const found = new Set();
    let frontier = [id];
    while (frontier.length) {
      const next = [];
      for (const row of rows) {
        if (found.has(row.id) || row.id === id) continue;
        if (appsPluginDependencies(row).some((dependency) => frontier.includes(dependency))) {
          found.add(row.id);
          next.push(row.id);
        }
      }
      frontier = next;
    }
    return rows.filter((row) => found.has(row.id));
  }

  function appsPluginsFrom(payload) {
    if (!Array.isArray(payload?.packages)) return;
    appsPlugins.installed = new Map(payload.packages.map((row) => [String(row.id), row.installed === true]));
    appsPlugins.places = new Map(payload.packages.map((row) => [String(row.id), appsPlaceList(row.places)]));
    appsPlugins.available = new Map(payload.packages.map((row) => [String(row.id), appsPlaceList(row.availablePlaces)]));
  }

  function appsPluginPlacesControl(row) {
    const name = row.name || row.id;
    const available = appsPluginAvailable(row.id);
    const chosen = appsPluginPlaces(row.id);
    const box = appsEl("div", "grid gap-1");
    const group = appsEl("div", "flex flex-wrap items-center gap-x-4 gap-y-1 text-xs text-muted-foreground");
    group.dataset.appsPluginPlaces = row.id;
    group.setAttribute("role", "group");
    group.setAttribute("aria-label", `Show ${name} on`);
    group.append(appsEl("span", "font-semibold text-foreground", "Show on"));
    for (const [place, label] of APPS_PLACES) {
      if (!available.includes(place)) continue;
      const item = appsEl("label", "inline-flex items-center gap-1.5");
      const input = appsEl("input", "h-3.5 w-3.5");
      input.type = "checkbox";
      input.dataset.appsPluginPlace = place;
      input.checked = chosen.includes(place);
      input.disabled = appsPlugins.busy || (input.checked && chosen.length === 1);
      if (input.checked && chosen.length === 1) input.title = `Remove ${name} to take it off everywhere.`;
      input.addEventListener("change", () => {
        const next = input.checked ? [...chosen, place] : chosen.filter((item) => item !== place);
        void setAppsPluginPlaces(row, appsPlaceList(next));
      });
      item.append(input, appsEl("span", "", label));
      group.append(item);
    }
    box.append(group);
    if (!chosen.includes("web")) {
      const note = available.includes("web")
        ? "Not shown on the web. Turn on Web to show it here."
        : `Not available on the web: it shows on ${appsPlaceNames(available)} only.`;
      const hint = appsEl("p", "text-xs text-muted-foreground", note);
      hint.dataset.appsPluginNotOnWeb = row.id;
      box.append(hint);
    }
    return box;
  }

  function appsPluginCard(row, depth) {
    const on = appsPlugins.installed.get(row.id) === true;
    const card = appsEl("div", "rounded-md border border-border p-3 grid gap-2");
    card.dataset.appsPlugin = row.id;
    const top = appsEl("div", "flex flex-col sm:flex-row sm:items-start sm:justify-between gap-2");
    const text = appsEl("div", "min-w-0");
    text.append(appsEl("p", "text-sm font-semibold text-foreground", row.name || row.id));
    text.append(appsEl("p", "text-xs text-muted-foreground", on ? "Installed" : "Not installed"));
    if (row.summary) text.append(appsEl("p", "mt-1 text-sm text-muted-foreground", row.summary));
    const toggle = appsButton(on ? "Remove" : "Install", () => { void setAppsPlugin(row, !on); }, on);
    toggle.dataset.appsPluginToggle = row.id;
    toggle.setAttribute("aria-label", `${on ? "Remove" : "Install"} ${row.name || row.id}`);
    toggle.disabled = appsPlugins.busy;
    top.append(text, toggle);
    card.append(top);
    if (on) card.append(appsPluginPlacesControl(row));
    const children = depth < 3 ? appsPluginChildren(row.id) : [];
    if (children.length) {
      const nest = appsEl("div", "grid gap-2 pl-4 border-l border-border");
      for (const child of children) nest.append(appsPluginCard(child, depth + 1));
      card.append(nest);
    }
    return card;
  }

  function renderAppsPlugins() {
    const box = $("[data-apps-plugins]");
    if (!box) return;
    box.textContent = "";
    if (appsPlugins.error) {
      box.append(appsEl("p", "text-sm text-muted-foreground", appsPlugins.error));
      return;
    }
    const rows = appsPluginRows();
    const known = new Set(rows.map((row) => row.id));
    const roots = rows.filter((row) => !known.has(appsPluginDependencies(row)[0] || ""));
    if (!roots.length) {
      box.append(appsEl("p", "text-sm text-muted-foreground", "No plugins are available for your account yet."));
      return;
    }
    for (const row of roots) box.append(appsPluginCard(row, 0));
  }

  function appsPluginNames(ids) {
    const names = new Map(appsPlugins.market.map((row) => [row.id, row.name || row.id]));
    return ids.map((id) => names.get(id) || id);
  }

  function appsPluginsChanged(changed) {
    const account = String(state.session?.nodeName || "");
    for (const id of changed) window.ForkMeshAPI?.rememberPlugin?.(account, id, appsPluginOnWeb(id));
    void window.ForkMeshAPI?.refreshPlugins?.(true);
    window.dispatchEvent(new CustomEvent("forkmesh:plugins-changed", { detail: { changed } }));
  }

  async function setAppsPluginPlaces(row, places) {
    if (appsPlugins.busy || !places.length) return;
    const name = row.name || row.id;
    appsPlugins.busy = true;
    renderAppsPlugins();
    try {
      const payload = await appsRequest("POST", `/api/plugins/@account/${encodeURIComponent(row.id)}/places`, { places });
      appsPluginsFrom(payload);
      appsPluginsChanged([row.id]);
      setAppsNotice("[data-apps-plugins-notice]", `${name} now shows on ${appsPlaceNames(appsPluginPlaces(row.id))}.`);
    } catch (error) {
      setAppsNotice("[data-apps-plugins-notice]", `${name}: ${error.message}`, true);
    }
    appsPlugins.busy = false;
    renderAppsPlugins();
  }

  async function setAppsPlugin(row, on) {
    if (appsPlugins.busy) return;
    const name = row.name || row.id;
    if (!on) {
      const going = appsPluginDependents(row.id).filter((item) => appsPlugins.installed.get(item.id) === true);
      const also = going.length ? ` This also removes ${appsPluginNames(going.map((item) => item.id)).join(", ")}, which need it.` : "";
      if (!window.confirm(`Remove ${name}?${also} Its links and screens disappear from the web, the desktop app and the mobile app on your account, and anything it stored for your account is deleted.`)) return;
    }
    appsPlugins.busy = true;
    renderAppsPlugins();
    try {
      const payload = await appsRequest("POST", `/api/plugins/@account/${encodeURIComponent(row.id)}/${on ? "install" : "uninstall"}`, {});
      appsPluginsFrom(payload);
      const changed = Array.isArray(payload.changed) ? payload.changed.map(String) : [row.id];
      appsPluginsChanged(changed);
      const others = changed.filter((id) => id !== row.id);
      setAppsNotice("[data-apps-plugins-notice]", `${name} was ${on ? "installed" : "removed"}.${others.length ? ` ${on ? "Also installed" : "Also removed"}: ${appsPluginNames(others).join(", ")}.` : ""}`);
    } catch (error) {
      setAppsNotice("[data-apps-plugins-notice]", `${name}: ${error.message}`, true);
    }
    appsPlugins.busy = false;
    renderAppsPlugins();
  }

  async function loadAppsPlugins() {
    if (!$("[data-apps-plugins]")) return;
    try {
      const [market, account] = await Promise.all([
        appsRequest("GET", "/api/market/packages"),
        appsRequest("GET", "/api/plugins/@account"),
      ]);
      appsPlugins.market = Array.isArray(market.packages) ? market.packages : [];
      appsPluginsFrom(account);
      appsPlugins.error = "";
    } catch (error) {
      appsPlugins.error = error.status === 401 ? "Sign in to manage your plugins." : error.message;
    }
    appsPlugins.loaded = true;
    renderAppsPlugins();
  }

  function initAppsPage() {
    if (!$("[data-apps-panel]")) return;
    $("[data-apps-plugins-refresh]")?.addEventListener("click", () => {
      setAppsNotice("[data-apps-plugins-notice]", "");
      void loadAppsPlugins();
    });
    void loadAppsPlugins();
    void loadInstalledApps();
    void loadAppDirectory();
    void loadAppReviewQueue();
  }

  const appConsole = { apps: [], scopes: [], events: [], orgs: [] };

  function appFormField(name) {
    return $(`[data-app-field="${name}"]`);
  }

  function renderAppChoices(boxSelector, values, chosen, labelOf) {
    const box = $(boxSelector);
    if (!box) return;
    box.textContent = "";
    for (const value of values) {
      const label = appsEl("label", "flex items-start gap-2 text-xs text-foreground");
      const input = appsEl("input");
      input.type = "checkbox";
      input.value = value;
      input.checked = chosen.includes(value);
      const text = appsEl("span", "", labelOf(value));
      label.append(input, text);
      box.append(label);
    }
  }

  function checkedValues(boxSelector) {
    return Array.from(document.querySelectorAll(`${boxSelector} input:checked`)).map((input) => input.value);
  }

  function resetAppForm(app) {
    const record = app || {};
    appFormField("id").value = record.id || "";
    appFormField("name").value = record.name || "";
    appFormField("description").value = record.description || "";
    appFormField("glyph").value = record.icon?.glyph || "";
    appFormField("color").value = record.icon?.color || "#6366f1";
    appFormField("organization").value = record.organization || "";
    appFormField("organization").disabled = Boolean(record.id);
    appFormField("redirectUris").value = (record.redirectUris || []).join("\n");
    appFormField("eventUrl").value = record.eventUrl || "";
    appFormField("homepageUrl").value = record.homepageUrl || "";
    appFormField("interactivityUrl").value = record.interactivityUrl || "";
    appFormField("bot").checked = Boolean(record.bot);
    appFormField("commands").value = (record.commands || []).map((command) => `/${command.command} | ${command.url || ""} | ${command.description || ""}`).join("\n");
    renderAppChoices("[data-app-scopes]", appConsole.scopes.map((scope) => scope.scope), record.scopes || [], (value) => {
      const scope = appConsole.scopes.find((entry) => entry.scope === value);
      return `${value}: ${scope?.label || ""}`;
    });
    renderAppChoices("[data-app-events]", appConsole.events, record.events || [], (value) => value);
    $("[data-app-form-title]").textContent = record.id ? `Edit ${record.name}` : "New app";
    $("[data-app-form-submit]").textContent = record.id ? "Save changes" : "Create app";
  }

  function appFormBody() {
    const commands = appFormField("commands").value.split("\n").map((line) => line.trim()).filter(Boolean).map((line) => {
      const [command, url, description] = line.split("|").map((part) => (part || "").trim());
      return { command: command.replace(/^\//, ""), url, description };
    });
    const body = {
      name: appFormField("name").value,
      description: appFormField("description").value,
      icon: { glyph: appFormField("glyph").value, color: appFormField("color").value },
      redirectUris: appFormField("redirectUris").value.split(/\s+/).filter(Boolean),
      scopes: checkedValues("[data-app-scopes]"),
      events: checkedValues("[data-app-events]"),
      eventUrl: appFormField("eventUrl").value.trim(),
      homepageUrl: appFormField("homepageUrl").value.trim(),
      commands,
      interactivityUrl: appFormField("interactivityUrl").value.trim(),
      bot: { enabled: appFormField("bot").checked },
    };
    if (!appFormField("id").value && appFormField("organization").value) body.organization = appFormField("organization").value;
    return body;
  }

  function showAppSecrets(lines) {
    const panel = $("[data-app-secrets]");
    panel.hidden = false;
    $("[data-app-secrets-text]").textContent = lines.join("\n");
  }

  async function submitAppForm(event) {
    event.preventDefault();
    const id = appFormField("id").value;
    try {
      if (id) {
        await appsRequest("PATCH", `/api/apps/${id}`, appFormBody());
        setAppsNotice("[data-app-console-notice]", "Saved.");
      } else {
        const created = await appsRequest("POST", "/api/apps", appFormBody());
        showAppSecrets([
          `Client ID:      ${created.app.clientId}`,
          `Client secret:  ${created.clientSecret}`,
          `Signing secret: ${created.signingSecret}`,
          `Authorize URL:  ${location.origin}/oauth/authorize`,
          `Token URL:      ${location.origin}/oauth/token`,
        ]);
        setAppsNotice("[data-app-console-notice]", `${created.app.name} was created.`);
      }
      resetAppForm(null);
      void loadMyApps();
    } catch (error) {
      setAppsNotice("[data-app-console-notice]", error.message, true);
    }
  }

  async function appConsoleAction(app, label, method, path, body, onDone) {
    try {
      const payload = await appsRequest(method, path, body);
      if (onDone) onDone(payload);
      else setAppsNotice("[data-app-console-notice]", `${app.name}: ${label}.`);
    } catch (error) {
      setAppsNotice("[data-app-console-notice]", `${app.name}: ${error.message}`, true);
    }
    void loadMyApps();
  }

  async function showAppDeliveries(app) {
    const panel = $("[data-app-deliveries-panel]");
    const box = $("[data-app-deliveries]");
    panel.hidden = false;
    $("[data-app-deliveries-title]").textContent = `Delivery log: ${app.name}`;
    box.textContent = "";
    try {
      const payload = await appsRequest("GET", `/api/apps/${app.id}/deliveries`);
      if (!(payload.deliveries || []).length) box.append(appsEl("p", "text-sm text-muted-foreground", "No deliveries yet."));
      for (const entry of payload.deliveries || []) {
        box.append(appsEl("p", "font-mono text-xs text-foreground", `${new Date(entry.ts).toLocaleString()}  ${entry.kind.padEnd(11)} ${(entry.eventType || "").padEnd(16)} ${String(entry.status).padStart(3)}  ${entry.outcome}  x${entry.attempts}${entry.detail ? "  " + entry.detail : ""}`));
      }
    } catch (error) {
      box.append(appsEl("p", "text-sm text-red-500", error.message));
    }
  }

  function renderAppConsoleList() {
    const list = $("[data-app-list]");
    list.textContent = "";
    if (!appConsole.apps.length) list.append(appsEl("p", "text-sm text-muted-foreground", "You have not registered an app yet."));
    for (const app of appConsole.apps) {
      const card = appsEl("div", "rounded-md border border-border p-3 grid gap-2");
      const status = [
        app.listing,
        `${app.installCount} install${app.installCount === 1 ? "" : "s"}`,
        app.eventUrl ? (app.eventsDisabled ? "events switched off" : app.eventUrlVerified ? "events verified" : "event URL not verified") : "",
      ].filter(Boolean).join(" · ");
      card.append(appsHeader(app, status));
      card.append(appsEl("p", "font-mono text-[11px] text-muted-foreground break-all", `client_id ${app.clientId} · bot ${app.bot ? app.botName : "off"}`));
      card.append(appsScopeList(app.scopes));
      const actions = appsEl("div", "flex flex-wrap gap-2");
      actions.append(
        appsButton("Edit", () => resetAppForm(app)),
        appsButton("Rotate client secret", () => appConsoleAction(app, "client secret rotated", "POST", `/api/apps/${app.id}/rotate`, { secret: "client" }, (payload) => showAppSecrets([`Client secret: ${payload.clientSecret}`]))),
        appsButton("Rotate signing secret", () => appConsoleAction(app, "signing secret rotated", "POST", `/api/apps/${app.id}/rotate`, { secret: "signing" }, (payload) => showAppSecrets([`Signing secret: ${payload.signingSecret}`]))),
      );
      if (app.eventUrl) actions.append(appsButton("Verify event URL", () => appConsoleAction(app, "event URL verified", "POST", `/api/apps/${app.id}/verify`, {})));
      actions.append(appsButton("Delivery log", () => showAppDeliveries(app)));
      if (app.listing === "private" || app.listing === "rejected") {
        actions.append(appsButton("Submit to the App Directory", () => appConsoleAction(app, "sent for review", "POST", `/api/apps/${app.id}/submit`, {})));
      }
      actions.append(appsButton("Delete", () => {
        if (window.confirm(`Delete ${app.name}? Every install is revoked.`)) void appConsoleAction(app, "deleted", "DELETE", `/api/apps/${app.id}`, {});
      }, true));
      card.append(actions);
      list.append(card);
    }
  }

  async function loadMyApps() {
    try {
      const payload = await appsRequest("GET", "/api/apps");
      appConsole.apps = payload.apps || [];
      if (!appConsole.scopes.length) {
        appConsole.scopes = payload.scopes || [];
        appConsole.events = payload.events || [];
        resetAppForm(null);
      }
      renderAppConsoleList();
    } catch (error) {
      const list = $("[data-app-list]");
      list.textContent = "";
      list.append(appsEl("p", "text-sm text-muted-foreground", error.status === 401 ? "Sign in to build apps." : error.message));
    }
  }

  async function loadAppConsoleOrgs() {
    try {
      const payload = await appsRequest("GET", "/api/apps/installed");
      const select = appFormField("organization");
      for (const org of payload.organizations || []) {
        if (!org.canManage) continue;
        const option = appsEl("option", "", org.name);
        option.value = org.name;
        select.append(option);
      }
    } catch (_) {}
  }

  function initAppsBuildPage() {
    if (!$("[data-app-console]")) return;
    $("[data-app-form]")?.addEventListener("submit", (event) => { void submitAppForm(event); });
    $("[data-app-form-reset]")?.addEventListener("click", () => resetAppForm(null));
    void loadMyApps();
    void loadAppConsoleOrgs();
  }

  return { initAppsBuildPage, initAppsPage };
  };
})();
