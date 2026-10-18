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
    const button = appsEl("button", "fm-btn fm-btn-sm" + (danger ? " fm-btn-danger" : ""), label);
    button.type = "button";
    button.addEventListener("click", onClick);
    return button;
  }

  function appsLink(label, href) {
    const link = appsEl("a", "fm-btn fm-btn-sm", label);
    link.href = href;
    return link;
  }

  function appsEmpty(text) {
    return appsEl("p", "apps-list-empty", text);
  }

  function appsCount(name, value) {
    const counter = $(`[data-apps-count="${name}"]`);
    if (counter) counter.textContent = value ? String(value) : "";
  }

  function setAppsNotice(selector, message, isError) {
    const notice = $(selector);
    if (!notice) return;
    notice.hidden = !message;
    notice.textContent = "";
    if (message) notice.append(appsFlash(isError ? "danger" : "success", message));
    window.lucide?.createIcons?.();
  }

  function appsLucide(name, className) {
    const icon = appsEl("i", className);
    icon.setAttribute("data-lucide", name);
    icon.setAttribute("aria-hidden", "true");
    return icon;
  }

  function appsFlash(tone, message) {
    const box = appsEl("div", tone === "danger" ? "fm-flash fm-flash-danger" : "fm-flash fm-flash-success");
    box.append(appsLucide(tone === "danger" ? "circle-alert" : "circle-check", "h-4 w-4"), appsEl("span", "fm-flash-body", message));
    return box;
  }

  function appsBlankslate(icon, title, text) {
    const box = appsEl("div", "fm-blankslate");
    box.append(appsLucide(icon, "h-6 w-6"), appsEl("p", "fm-blankslate-title", title));
    if (text) box.append(appsEl("p", "fm-blankslate-desc", text));
    queueMicrotask(() => window.lucide?.createIcons?.());
    return box;
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
    const box = appsEl("div", "apps-tile");
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
    box.style.borderColor = "transparent";
    box.style.color = "#fff";
    box.textContent = icon.glyph || String(app?.name || "?").slice(0, 1).toUpperCase();
    return box;
  }

  function appsHeader(app, subtitle) {
    const head = appsEl("div", "apps-row-main");
    const text = appsEl("div", "apps-row-text");
    const title = appsEl("div", "apps-row-title");
    title.append(appsEl("span", "", app?.name || "App"));
    text.append(title);
    if (subtitle) text.append(appsEl("p", "apps-row-sub", subtitle));
    head.append(appsIcon(app), text);
    return head;
  }

  function appsScopeList(scopes) {
    const list = appsEl("div", "apps-scopes");
    for (const scope of scopes || []) {
      const chip = appsEl("span", "apps-scope", scope.scope || scope);
      if (scope.label) chip.title = scope.label;
      list.append(chip);
    }
    return list;
  }

  function appsRow(app, subtitle, actions, scopes) {
    const row = appsEl("div", "apps-row");
    const action = appsEl("div", "apps-row-action");
    for (const item of actions) action.append(item);
    row.append(appsHeader(app, subtitle), action, appsScopeList(scopes));
    return row;
  }

  function appsInstallCard(install, onRevoke) {
    const actions = install.canRevoke ? [appsButton("Revoke", () => onRevoke(install), true)] : [];
    return appsRow(install.app, `Installed by @${install.installedBy || "?"} · last used ${appsWhen(install.lastUsedAt)}`, actions, install.scopes);
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

  function appsOrgPolicy(org) {
    const policy = org.policy || { requestPolicy: "any", allowlist: [] };
    const form = appsEl("form", "apps-policy");
    const label = appsEl("label", "", "Members may request");
    const select = appsEl("select", "apps-input");
    for (const [value, text] of [["any", "any app"], ["allowlist", "only allowlisted apps"], ["none", "no apps"]]) {
      const option = appsEl("option", "", text);
      option.value = value;
      option.selected = policy.requestPolicy === value;
      select.append(option);
    }
    label.append(select);
    const allow = appsEl("label", "", "Allowlisted app ids (comma separated)");
    const input = appsEl("input", "apps-input font-mono");
    input.value = (policy.allowlist || []).join(", ");
    allow.append(input);
    const save = appsEl("button", "fm-btn", "Save policy");
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
    return form;
  }

  function renderOrgApps(org) {
    const block = appsEl("div", "apps-section");
    const head = appsEl("h2", "apps-section-head", org.name);
    head.append(appsEl("span", "fm-label", org.role));
    const list = appsEl("div", "apps-list");
    block.append(head, list);
    const installs = org.installs || [];
    if (!installs.length) list.append(appsEmpty("No apps installed for this organization."));
    for (const install of installs) list.append(appsInstallCard(install, revokeAppInstall));
    if (!org.canManage) return block;
    for (const request of org.requests || []) {
      const decide = async (action) => {
        try {
          await appsRequest("POST", `/api/apps/orgs/${encodeURIComponent(org.name)}/requests/${request.id}`, { action });
          setAppsNotice("[data-apps-notice]", action === "approve" ? "Approved. The member can finish installing." : "Request denied.");
        } catch (error) {
          setAppsNotice("[data-apps-notice]", error.message, true);
        }
        void loadInstalledApps();
      };
      list.append(appsRow(request.app, `@${request.requester} asked to install this for ${org.name}`,
        [appsButton("Approve", () => decide("approve")), appsButton("Deny", () => decide("deny"), true)], request.scopes));
    }
    list.append(appsOrgPolicy(org));
    return block;
  }

  async function loadInstalledApps() {
    const list = $("[data-apps-installed]");
    const orgs = $("[data-apps-orgs]");
    if (!list) return;
    try {
      const payload = await appsRequest("GET", "/api/apps/installed");
      const installs = payload.installs || [];
      const organizations = payload.organizations || [];
      list.textContent = "";
      if (!installs.length) list.append(appsEmpty("You have not installed any apps."));
      for (const install of installs) list.append(appsInstallCard(install, revokeAppInstall));
      orgs.textContent = "";
      orgs.hidden = !organizations.length;
      for (const org of organizations) orgs.append(renderOrgApps(org));
      appsCount("installed", installs.length + organizations.reduce((sum, org) => sum + (org.installs || []).length, 0));
    } catch (error) {
      list.textContent = "";
      list.append(appsEmpty(error.status === 401 ? "Sign in to see your apps." : error.message));
    }
  }

  async function loadAppDirectory() {
    const box = $("[data-apps-directory]");
    if (!box) return;
    try {
      const payload = await appsRequest("GET", "/api/apps/directory");
      box.textContent = "";
      if (!(payload.apps || []).length) box.append(appsEmpty("No apps are listed yet."));
      for (const app of payload.apps || []) {
        const rating = app.rating?.count ? `${app.rating.average.toFixed(1)} stars from ${app.rating.count}` : "not rated yet";
        const links = [];
        if (app.homepageUrl) {
          const visit = appsLink("Visit to install", app.homepageUrl);
          visit.rel = "noopener noreferrer";
          visit.target = "_blank";
          links.push(visit);
        }
        links.push(appsLink("Rate", `/market-reviews?market=forkmesh-apps&package=${encodeURIComponent(app.slug)}`));
        const by = `${app.organization || app.developer} · ${rating}`;
        const row = appsRow(app, app.description ? `${by} · ${app.description}` : by, links, app.scopes);
        box.append(row);
      }
    } catch (error) {
      box.textContent = "";
      box.append(appsEmpty(error.message));
    }
  }

  async function loadAppReviewQueue() {
    const tab = $('[data-apps-tab="review"]');
    const box = $("[data-apps-review]");
    if (!tab || !box) return;
    let payload;
    try {
      payload = await appsRequest("GET", "/api/apps/review");
    } catch (_) {
      tab.hidden = true;
      return;
    }
    tab.hidden = false;
    box.textContent = "";
    const apps = payload.apps || [];
    appsCount("review", apps.filter((app) => app.listing === "pending").length);
    if (!apps.length) box.append(appsEmpty("Nothing is waiting for review."));
    for (const app of apps) {
      const reason = appsEl("input", "apps-input apps-review-input");
      reason.placeholder = "Reason (required to reject or suspend)";
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
      const actions = [];
      if (app.listing === "pending") {
        actions.push(appsButton("Approve", () => act("approve")), appsButton("Reject", () => act("reject"), true));
      }
      if (app.listing === "suspended") actions.push(appsButton("Reinstate", () => act("reinstate")));
      else actions.push(appsButton("Suspend", () => act("suspend"), true));
      const row = appsRow(app, `by @${app.developer} · ${app.listing}${app.description ? ` · ${app.description}` : ""}`, actions, app.scopes);
      const urls = appsEl("div", "apps-scopes");
      for (const url of [...(app.redirectUris || []), app.eventUrl, app.interactivityUrl].filter(Boolean)) {
        urls.append(appsEl("span", "apps-scope break-all", url));
      }
      row.append(urls, reason);
      box.append(row);
    }
  }

  const appsPlugins = { market: [], installed: new Map(), places: new Map(), available: new Map(), busy: false, error: "", loaded: false, show: "all", query: "", category: "" };
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

  function appsPluginCategory(row) {
    return String(row?.category || "").trim() || "Other";
  }

  function appsPluginPlacesControl(row) {
    const name = row.name || row.id;
    const available = appsPluginAvailable(row.id);
    const chosen = appsPluginPlaces(row.id);
    const group = appsEl("div", "apps-row-places apps-places");
    group.dataset.appsPluginPlaces = row.id;
    group.setAttribute("role", "group");
    group.setAttribute("aria-label", `Show ${name} on`);
    for (const [place, label] of APPS_PLACES) {
      if (!available.includes(place)) continue;
      const item = appsEl("label", "apps-place");
      const input = appsEl("input", "");
      input.type = "checkbox";
      input.dataset.appsPluginPlace = place;
      input.checked = chosen.includes(place);
      input.disabled = appsPlugins.busy || (input.checked && chosen.length === 1);
      item.title = input.checked && chosen.length === 1 ? `Remove ${name} to take it off everywhere.` : `Show ${name} on ${label}`;
      input.addEventListener("change", () => {
        const next = input.checked ? [...chosen, place] : chosen.filter((item) => item !== place);
        void setAppsPluginPlaces(row, appsPlaceList(next));
      });
      item.append(input, appsEl("span", "", label));
      group.append(item);
    }
    return group;
  }

  function appsPluginWebHint(row) {
    const available = appsPluginAvailable(row.id);
    if (appsPluginPlaces(row.id).includes("web")) return null;
    const note = available.includes("web")
      ? "Not shown on the web. Turn on Web to show it here."
      : `Not available on the web: it shows on ${appsPlaceNames(available)} only.`;
    const hint = appsEl("p", "apps-row-sub", note);
    hint.dataset.appsPluginNotOnWeb = row.id;
    return hint;
  }

  function appsPluginCard(row) {
    const on = appsPlugins.installed.get(row.id) === true;
    const name = row.name || row.id;
    const card = appsEl("div", on ? "apps-row" : "apps-row apps-row-off");
    card.dataset.appsPlugin = row.id;
    const main = appsEl("div", "apps-row-main");
    const text = appsEl("div", "apps-row-text");
    const title = appsEl("div", "apps-row-title");
    title.append(appsEl("span", "", name));
    const parent = appsPluginDependencies(row)[0];
    const parentRow = parent && appsPluginRows().find((item) => item.id === parent);
    if (parentRow) title.append(appsEl("span", "fm-label", `Needs ${parentRow.name || parentRow.id}`));
    const children = appsPluginChildren(row.id).length;
    if (children) title.append(appsEl("span", "fm-label", `${children} add-on${children === 1 ? "" : "s"}`));
    text.append(title);
    if (row.summary) {
      const summary = appsEl("p", "apps-row-sub", row.summary);
      summary.title = row.summary;
      text.append(summary);
    }
    if (on) {
      const hint = appsPluginWebHint(row);
      if (hint) text.append(hint);
    }
    main.append(appsEl("div", "apps-tile", name.slice(0, 1).toUpperCase()), text);
    card.append(main);
    if (on) card.append(appsPluginPlacesControl(row));
    const action = appsEl("div", "apps-row-action");
    const toggle = appsEl("button", "apps-switch");
    toggle.type = "button";
    toggle.dataset.appsPluginToggle = row.id;
    toggle.setAttribute("role", "switch");
    toggle.setAttribute("aria-checked", on ? "true" : "false");
    toggle.setAttribute("aria-label", `${on ? "Remove" : "Install"} ${name}`);
    toggle.title = on ? `Remove ${name}` : `Install ${name}`;
    toggle.disabled = appsPlugins.busy;
    toggle.addEventListener("click", () => { void setAppsPlugin(row, !on); });
    action.append(toggle);
    card.append(action);
    return card;
  }

  function appsPluginMatches(row) {
    const on = appsPlugins.installed.get(row.id) === true;
    if (appsPlugins.show === "on" && !on) return false;
    if (appsPlugins.show === "off" && on) return false;
    if (appsPlugins.category && appsPluginCategory(row) !== appsPlugins.category) return false;
    const query = appsPlugins.query.trim().toLowerCase();
    if (!query) return true;
    return [row.name, row.id, row.summary, appsPluginCategory(row)].some((value) => String(value || "").toLowerCase().includes(query));
  }

  function renderAppsPluginsRail(rows) {
    const counts = new Map();
    for (const row of rows) counts.set(appsPluginCategory(row), (counts.get(appsPluginCategory(row)) || 0) + 1);
    const categories = [...counts.keys()].sort((a, b) => a.localeCompare(b));
    if (appsPlugins.category && !counts.has(appsPlugins.category)) appsPlugins.category = "";
    const rail = $("[data-apps-plugins-rail]");
    if (rail) {
      rail.textContent = "";
      const item = (value, label, count) => {
        const button = appsEl("button", "apps-rail-item");
        button.type = "button";
        button.setAttribute("aria-current", appsPlugins.category === value ? "true" : "false");
        button.append(appsEl("span", "", label), appsEl("span", "", count));
        button.addEventListener("click", () => {
          appsPlugins.category = value;
          renderAppsPlugins();
        });
        return button;
      };
      rail.append(item("", "All plugins", rows.length), appsEl("div", "apps-rail-divider"));
      for (const category of categories) rail.append(item(category, category, counts.get(category)));
    }
    const select = $("[data-apps-plugins-category]");
    if (select) {
      select.textContent = "";
      for (const [value, label] of [["", `All categories (${rows.length})`], ...categories.map((category) => [category, `${category} (${counts.get(category)})`])]) {
        const option = appsEl("option", "", label);
        option.value = value;
        option.selected = appsPlugins.category === value;
        select.append(option);
      }
    }
  }

  function appsPluginsToInstall() {
    const rows = appsPluginRows();
    const ids = new Set(appsPlugins.market.map((row) => row?.id));
    return rows.filter((row) => appsPlugins.installed.get(row.id) !== true
      && row.type !== "agent"
      && row.type !== "theme"
      && !ids.has(appsPluginDependencies(row)[0]));
  }

  function renderAppsPluginsInstallAll() {
    const button = $("[data-apps-plugins-install-all]");
    if (!button) return;
    const remaining = appsPlugins.error ? 0 : appsPluginsToInstall().length;
    const label = button.querySelector("[data-apps-plugins-install-all-label]");
    const text = !appsPlugins.loaded || appsPlugins.error ? "Install all" : remaining ? `Install all (${remaining})` : "All installed";
    if (label) label.textContent = text;
    button.disabled = appsPlugins.busy || remaining === 0;
    button.setAttribute("aria-label", text);
  }

  function renderAppsPlugins() {
    renderAppsPluginsInstallAll();
    const box = $("[data-apps-plugins]");
    if (!box) return;
    box.textContent = "";
    if (appsPlugins.error) {
      box.append(appsEmpty(appsPlugins.error));
      return;
    }
    const rows = appsPluginRows().slice().sort((a, b) => String(a.name || a.id).localeCompare(String(b.name || b.id)));
    appsCount("plugins", rows.filter((row) => appsPlugins.installed.get(row.id) === true).length);
    renderAppsPluginsRail(rows);
    if (!rows.length) {
      box.append(appsEmpty("No plugins are available for your account yet."));
      return;
    }
    const shown = rows.filter(appsPluginMatches);
    if (!shown.length) {
      box.append(appsEmpty("No plugins match."));
      return;
    }
    if (appsPlugins.category || appsPlugins.query.trim()) {
      for (const row of shown) box.append(appsPluginCard(row));
      return;
    }
    const groups = new Map();
    for (const row of shown) {
      const category = appsPluginCategory(row);
      if (!groups.has(category)) groups.set(category, []);
      groups.get(category).push(row);
    }
    for (const category of [...groups.keys()].sort((a, b) => a.localeCompare(b))) {
      const header = appsEl("div", "apps-group");
      header.append(appsEl("span", "", category), appsEl("span", "", groups.get(category).length));
      box.append(header);
      for (const row of groups.get(category)) box.append(appsPluginCard(row));
    }
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

  async function installAllAppsPlugins() {
    if (appsPlugins.busy) return;
    const pending = appsPluginsToInstall();
    if (!pending.length) return;
    const noun = pending.length === 1 ? "plugin" : "plugins";
    if (!window.confirm(`Install ${pending.length} ${noun} for your account? They show on the web, the desktop app and the mobile app, and you can remove any of them later.`)) return;
    appsPlugins.busy = true;
    renderAppsPlugins();
    const changed = new Set();
    const failed = [];
    for (const row of pending) {
      if (appsPlugins.installed.get(row.id) === true) continue;
      try {
        const payload = await appsRequest("POST", `/api/plugins/@account/${encodeURIComponent(row.id)}/install`, {});
        appsPluginsFrom(payload);
        for (const id of Array.isArray(payload.changed) ? payload.changed.map(String) : [row.id]) changed.add(id);
      } catch (error) {
        failed.push(row.name || row.id);
      }
    }
    if (changed.size) appsPluginsChanged([...changed]);
    const installed = `Installed ${changed.size}`;
    setAppsNotice("[data-apps-plugins-notice]", failed.length ? `${installed}, ${failed.length} failed: ${failed.join(", ")}.` : `${installed}.`, failed.length > 0);
    appsPlugins.busy = false;
    renderAppsPlugins();
  }

  async function runAppsInstallAllRequest() {
    const params = new URLSearchParams(location.search);
    if (!params.has("installAll") || appsPlugins.error) return;
    params.delete("installAll");
    const query = params.toString();
    history.replaceState(null, "", `${location.pathname}${query ? `?${query}` : ""}${location.hash}`);
    await installAllAppsPlugins();
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

  const APPS_TABS = Object.freeze(["plugins", "installed", "directory", "review"]);

  function showAppsTab(name) {
    const tab = APPS_TABS.includes(name) && !$(`[data-apps-tab="${name}"]`)?.hidden ? name : "plugins";
    for (const button of document.querySelectorAll("[data-apps-tab]")) {
      button.setAttribute("aria-selected", button.dataset.appsTab === tab ? "true" : "false");
    }
    for (const panel of document.querySelectorAll("[data-apps-tabpanel]")) {
      panel.hidden = panel.dataset.appsTabpanel !== tab;
    }
    return tab;
  }

  function initAppsPage() {
    if (!$("[data-apps-panel]")) return;
    for (const button of document.querySelectorAll("[data-apps-tab]")) {
      button.addEventListener("click", () => {
        const tab = showAppsTab(button.dataset.appsTab);
        history.replaceState(null, "", tab === "plugins" ? location.pathname + location.search : `#${tab}`);
      });
    }
    showAppsTab(location.hash.slice(1));
    $("[data-apps-plugins-refresh]")?.addEventListener("click", () => {
      setAppsNotice("[data-apps-plugins-notice]", "");
      void loadAppsPlugins();
    });
    $("[data-apps-plugins-install-all]")?.addEventListener("click", () => {
      void installAllAppsPlugins();
    });
    $("[data-apps-plugins-search]")?.addEventListener("input", (event) => {
      appsPlugins.query = event.target.value;
      if (appsPlugins.query.trim()) appsPlugins.category = "";
      renderAppsPlugins();
    });
    $("[data-apps-plugins-category]")?.addEventListener("change", (event) => {
      appsPlugins.category = event.target.value;
      renderAppsPlugins();
    });
    for (const button of document.querySelectorAll("[data-apps-plugins-show]")) {
      button.addEventListener("click", () => {
        appsPlugins.show = button.dataset.appsPluginsShow;
        for (const other of document.querySelectorAll("[data-apps-plugins-show]")) {
          other.setAttribute("aria-pressed", other === button ? "true" : "false");
        }
        renderAppsPlugins();
      });
    }
    void loadAppsPlugins().then(runAppsInstallAllRequest);
    void loadInstalledApps();
    void loadAppDirectory();
    void loadAppReviewQueue().then(() => {
      if (location.hash === "#review") showAppsTab("review");
    });
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
      if (!(payload.deliveries || []).length) box.append(appsBlankslate("send", "No deliveries yet", "Events sent to this app show up here."));
      for (const entry of payload.deliveries || []) {
        box.append(appsEl("p", "font-mono text-xs text-foreground", `${new Date(entry.ts).toLocaleString()}  ${entry.kind.padEnd(11)} ${(entry.eventType || "").padEnd(16)} ${String(entry.status).padStart(3)}  ${entry.outcome}  x${entry.attempts}${entry.detail ? "  " + entry.detail : ""}`));
      }
    } catch (error) {
      box.append(appsFlash("danger", error.message));
      window.lucide?.createIcons?.();
    }
  }

  function renderAppConsoleList() {
    const list = $("[data-app-list]");
    list.textContent = "";
    if (!appConsole.apps.length) list.append(appsBlankslate("code", "No apps yet", "You have not registered an app yet."));
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
      list.append(error.status === 401
        ? appsBlankslate("lock", "Sign in to build apps")
        : appsBlankslate("circle-alert", "Your apps did not load", error.message));
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
