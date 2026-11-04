  const PLUGINS_PLACE_NAMES = Object.freeze({ mobile: "Mobile", desktop: "Desktop", web: "Web" });
  const PLUGINS_TABS = Object.freeze([
    ["overview", "Overview"],
    ["data", "Data"],
    ["logs", "Logs"],
    ["settings", "Settings"],
  ]);
  const pluginsConsole = {
    packages: [],
    places: ["mobile", "desktop", "web"],
    staged: false,
    detail: null,
    tab: "overview",
    browse: null,
    org: null,
    busy: false,
    token: 0,
  };

  async function pluginsRequest(method, path, body) {
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
      const error = new Error(pluginsErrorText(payload.error, response.status));
      error.status = response.status;
      error.code = String(payload.error || "");
      throw error;
    }
    return payload;
  }

  function pluginsErrorText(code, status) {
    const known = {
      invalid_session: "Sign in to manage plugins.",
      admin_required: "Only an administrator of this instance can open the plugin console.",
      plugin_not_found: "This instance does not know that package.",
      table_not_found: "That table does not belong to this package.",
      table_not_created: "That table has not been created yet. It appears once the package is installed and used.",
      plugin_not_installed: "The package is not installed there.",
      plugin_not_account_synced: "This package is not installed per account.",
      plugin_installs_per_account: "Each account installs this package for itself.",
      organization_not_found: "No organization with that name that you belong to.",
      organization_admin_required: "You need to be an owner or admin of that organization.",
      invalid_places: "Pick at least one place.",
      plugin_disabled_here: "This package is turned off on this instance.",
    };
    return known[code] || (code ? String(code).replace(/_/g, " ") : `HTTP ${status}`);
  }

  function pluginsEl(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function pluginsIcon(name) {
    const icon = pluginsEl("i");
    icon.setAttribute("data-lucide", name);
    icon.setAttribute("aria-hidden", "true");
    return icon;
  }

  function pluginsIcons() {
    window.lucide?.createIcons?.();
  }

  function pluginsButton(label, icon, className) {
    const button = pluginsEl("button", className || "fm-btn fm-btn-sm");
    button.type = "button";
    if (icon) button.append(pluginsIcon(icon));
    button.append(document.createTextNode(label));
    return button;
  }

  function pluginsLabel(text, tone) {
    return pluginsEl("span", tone ? `fm-label fm-label-${tone}` : "fm-label", text);
  }

  function pluginsNotice(selector, message, tone) {
    const box = $(selector);
    if (!box) return;
    box.textContent = "";
    box.hidden = !message;
    if (!message) return;
    const flash = pluginsEl("div", tone === "danger" ? "fm-flash fm-flash-danger" : tone === "success" ? "fm-flash fm-flash-success" : "fm-flash");
    flash.append(pluginsIcon(tone === "danger" ? "circle-alert" : tone === "success" ? "circle-check" : "info"), pluginsEl("span", "fm-flash-body", message));
    box.append(flash);
    pluginsIcons();
  }

  function pluginsBytes(value) {
    const bytes = Number(value) || 0;
    if (bytes < 1024) return `${bytes} B`;
    const units = ["KB", "MB", "GB"];
    let size = bytes / 1024;
    let unit = 0;
    while (size >= 1024 && unit < units.length - 1) {
      size /= 1024;
      unit += 1;
    }
    return `${size.toFixed(size < 10 ? 1 : 0)} ${units[unit]}`;
  }

  function pluginsCount(value) {
    return Number(value || 0).toLocaleString();
  }

  function pluginsWhen(ms) {
    if (!ms) return "";
    const date = new Date(Number(ms));
    return `${date.toLocaleString()} (${formatTimeAgo(date.toISOString())})`;
  }

  function pluginsShips(row) {
    const surfaces = row.surfaces || {};
    const ships = [];
    if (surfaces.worker) ships.push("Worker");
    if (surfaces.dashboard) ships.push("Dashboard page");
    if (surfaces.www) ships.push("Site page");
    if (surfaces.desktop === "module") ships.push("Desktop module");
    if (surfaces.desktop === "core") ships.push("Desktop, in core");
    if (surfaces.world) ships.push("World");
    return ships;
  }

  function pluginsPlaces(list) {
    return (list || []).map((place) => PLUGINS_PLACE_NAMES[place] || place).join(", ");
  }

  function pluginsKindLabel(row) {
    if (row.kind === "plugin") return pluginsLabel("Plugin", "accent");
    if (row.kind === "switch") return pluginsLabel("Switch");
    return null;
  }

  function pluginsPolicyLabel(row) {
    if (row.policy?.disabled) return pluginsLabel("Off on this instance", "attention");
    if (row.policy?.restricted) return pluginsLabel("Limited here", "attention");
    return null;
  }

  function pluginsMineLabel(row) {
    if (!row.accountSync || !row.mine) return null;
    return row.mine.installed ? pluginsLabel("Installed for me", "success") : null;
  }

  function pluginsMatches(row) {
    const query = String($("[data-plugins-filter]")?.value || "").trim().toLowerCase();
    const kind = String($("[data-plugins-kind]")?.value || "");
    if (query && !`${row.name} ${row.id} ${row.category}`.toLowerCase().includes(query)) return false;
    if (kind === "plugin" || kind === "switch") return row.kind === kind;
    if (kind === "mine") return Boolean(row.mine?.installed);
    if (kind === "worker") return Boolean(row.surfaces?.worker);
    if (kind === "data") return Boolean(row.dataScope);
    if (kind === "policy") return Boolean(row.policy?.restricted);
    return true;
  }

  function pluginsRow(row) {
    const button = pluginsEl("button", "pc-row");
    button.type = "button";
    button.dataset.pluginsOpen = row.id;
    const name = pluginsEl("span", "pc-name");
    const title = pluginsEl("span", "pc-chips");
    title.append(pluginsEl("strong", "", row.name || row.id));
    for (const label of [pluginsKindLabel(row), pluginsMineLabel(row), pluginsPolicyLabel(row)]) if (label) title.append(label);
    if (row.inBuild === false) title.append(pluginsLabel("Not in this build", "attention"));
    const meta = pluginsEl("span", "pc-id", `${row.id}${row.version ? ` · ${row.version}` : ""}`);
    name.append(title, meta);
    const ships = pluginsEl("span", "pc-chips");
    const items = pluginsShips(row);
    if (!items.length) ships.append(pluginsEl("span", "pc-muted", "Core feature switch"));
    for (const item of items) ships.append(pluginsLabel(item));
    const places = pluginsEl("span", "pc-muted", row.policy?.disabled ? "Off" : pluginsPlaces(row.policy?.places || row.places));
    const installs = row.installs || {};
    const parts = [];
    if (row.accountSync || installs.accounts) parts.push(`${pluginsCount(installs.accounts)} accounts`);
    if (installs.organizations) parts.push(`${pluginsCount(installs.organizations)} orgs`);
    const counts = pluginsEl("span", "pc-muted pc-num", parts.join(" · ") || "—");
    button.append(name, ships, places, counts);
    return button;
  }

  function renderPluginsList() {
    const box = $("[data-plugins-list]");
    if (!box) return;
    box.textContent = "";
    const shown = pluginsConsole.packages.filter(pluginsMatches);
    const plugins = pluginsConsole.packages.filter((row) => row.kind === "plugin").length;
    const summary = $("[data-plugins-summary]");
    if (summary) {
      summary.textContent = pluginsConsole.packages.length
        ? `${shown.length} of ${pluginsConsole.packages.length} shown · ${plugins} plugins, ${pluginsConsole.packages.length - plugins} switches`
        : "";
    }
    if (!shown.length) {
      box.append(pluginsEl("p", "pc-empty", pluginsConsole.packages.length ? "No package matches." : "No packages."));
      return;
    }
    for (const row of shown) box.append(pluginsRow(row));
    pluginsIcons();
  }

  async function loadPluginsList() {
    const box = $("[data-plugins-list]");
    try {
      const payload = await pluginsRequest("GET", "/api/plugins/@console");
      pluginsConsole.packages = Array.isArray(payload.packages) ? payload.packages : [];
      pluginsConsole.places = Array.isArray(payload.places) ? payload.places : pluginsConsole.places;
      pluginsConsole.staged = payload.staged === true;
      pluginsNotice("[data-plugins-notice]", pluginsConsole.staged ? "" : "This App runs from a source checkout without a staged plugin registry, so build and install facts come from the catalog.", "info");
      renderPluginsList();
    } catch (error) {
      pluginsConsole.packages = [];
      if (box) box.textContent = "";
      pluginsNotice("[data-plugins-notice]", error.message, "danger");
    }
  }

  function pluginsSelected() {
    return new URLSearchParams(location.search).get("package") || "";
  }

  function pluginsNavigate(id, replace) {
    const url = new URL(location.href);
    if (id) url.searchParams.set("package", id);
    else url.searchParams.delete("package");
    url.searchParams.delete("tab");
    history[replace ? "replaceState" : "pushState"](null, "", `${url.pathname}${url.search}`);
    void pluginsRoute();
  }

  async function pluginsRoute() {
    const id = pluginsSelected();
    $("[data-plugins-list-view]").hidden = Boolean(id);
    $("[data-plugins-detail-view]").hidden = !id;
    if (!id) {
      pluginsConsole.detail = null;
      if (!pluginsConsole.packages.length) await loadPluginsList();
      else renderPluginsList();
      return;
    }
    const tab = new URLSearchParams(location.search).get("tab");
    pluginsConsole.tab = PLUGINS_TABS.some(([key]) => key === tab) ? tab : "overview";
    await loadPluginsDetail(id);
  }

  async function loadPluginsDetail(id) {
    const token = ++pluginsConsole.token;
    const box = $("[data-plugins-detail]");
    if (pluginsConsole.detail?.package?.id !== id) {
      box.textContent = "";
      box.append(pluginsEl("p", "pc-empty", "Loading..."));
      pluginsConsole.browse = null;
      pluginsConsole.org = null;
    }
    try {
      const payload = await pluginsRequest("GET", `/api/plugins/@console/${encodeURIComponent(id)}`);
      if (token !== pluginsConsole.token) return;
      pluginsConsole.detail = payload;
      pluginsNotice("[data-plugins-detail-notice]", "");
      renderPluginsDetail();
    } catch (error) {
      if (token !== pluginsConsole.token) return;
      box.textContent = "";
      pluginsNotice("[data-plugins-detail-notice]", error.message, "danger");
    }
  }

  function pluginsBox(title, aside) {
    const box = pluginsEl("div", "pc-box");
    const head = pluginsEl("div", "pc-box-head", title);
    if (aside !== undefined && aside !== null) head.append(typeof aside === "string" ? pluginsEl("span", "", aside) : aside);
    box.append(head);
    return box;
  }

  function pluginsFacts(rows) {
    const list = pluginsEl("dl", "pc-facts");
    for (const [label, value] of rows) {
      const item = pluginsEl("div", "pc-fact");
      item.append(pluginsEl("dt", "", label));
      const dd = pluginsEl("dd");
      if (value instanceof Node) dd.append(value);
      else dd.textContent = value === "" || value === null || value === undefined ? "—" : String(value);
      item.append(dd);
      list.append(item);
    }
    return list;
  }

  function pluginsLink(href, text) {
    const link = pluginsEl("a", "pc-link", text || href);
    link.href = href;
    return link;
  }

  function pluginsPackageLinks(ids) {
    if (!ids?.length) return "";
    const span = pluginsEl("span", "pc-chips");
    for (const id of ids) {
      const link = pluginsLink(`/dashboard/plugins?package=${encodeURIComponent(id)}`, id);
      link.dataset.pluginsOpen = id;
      span.append(link);
    }
    return span;
  }

  function pluginsTable(columns, rows, options) {
    const wrap = pluginsEl("div", "pc-table-wrap");
    const table = pluginsEl("table", "pc-table");
    const head = pluginsEl("tr");
    for (const column of columns) head.append(pluginsEl("th", "", column));
    const thead = pluginsEl("thead");
    thead.append(head);
    const body = pluginsEl("tbody");
    for (const row of rows) {
      const tr = pluginsEl("tr");
      if (options?.selected && options.selected(row)) tr.setAttribute("aria-selected", "true");
      for (const [index, cell] of row.cells.entries()) {
        const td = pluginsEl("td", options?.code?.includes(index) ? "pc-code" : "");
        if (cell instanceof Node) td.append(cell);
        else td.textContent = cell === null || cell === undefined ? "" : typeof cell === "object" ? JSON.stringify(cell) : String(cell);
        tr.append(td);
      }
      body.append(tr);
    }
    table.append(thead, body);
    wrap.append(table);
    return wrap;
  }

  function renderPluginsDetail() {
    const data = pluginsConsole.detail;
    const box = $("[data-plugins-detail]");
    if (!data || !box) return;
    const row = data.package;
    box.textContent = "";
    const head = pluginsEl("div", "pc-detail-head");
    const text = pluginsEl("div", "min-w-0");
    const title = pluginsEl("div", "pc-chips");
    title.append(pluginsEl("h2", "", row.name || row.id));
    for (const label of [pluginsKindLabel(row), pluginsMineLabel(row), pluginsPolicyLabel(row)]) if (label) title.append(label);
    text.append(title, pluginsEl("div", "pc-id", `${row.id}${row.version ? ` · version ${row.version}` : ""}${row.publisher ? ` · ${row.publisher}` : ""}`));
    if (row.summary) text.append(pluginsEl("p", "", row.summary));
    head.append(text, pluginsDetailActions(row));
    box.append(head);
    const tabs = pluginsEl("div", "pc-tabs");
    tabs.setAttribute("role", "tablist");
    for (const [key, label] of PLUGINS_TABS) {
      const tab = pluginsEl("button", "", label);
      tab.type = "button";
      tab.setAttribute("role", "tab");
      tab.dataset.pluginsTab = key;
      tab.setAttribute("aria-selected", String(pluginsConsole.tab === key));
      tabs.append(tab);
    }
    box.append(tabs);
    const panel = pluginsEl("div");
    panel.setAttribute("role", "tabpanel");
    if (pluginsConsole.tab === "data") renderPluginsData(panel, data);
    else if (pluginsConsole.tab === "logs") renderPluginsLogs(panel, data);
    else if (pluginsConsole.tab === "settings") renderPluginsSettings(panel, data);
    else renderPluginsOverview(panel, data);
    box.append(panel);
    pluginsIcons();
  }

  function pluginsDetailActions(row) {
    const actions = pluginsEl("div", "pc-actions");
    const surfaces = row.surfaces || {};
    for (const href of [surfaces.dashboard, surfaces.www]) {
      if (!href) continue;
      const open = pluginsEl("a", "fm-btn fm-btn-sm");
      open.href = href;
      open.append(pluginsIcon("external-link"), document.createTextNode(`Open ${href}`));
      actions.append(open);
    }
    if (row.accountSync && row.mine) {
      const install = row.mine.installed
        ? pluginsButton("Remove for me", "trash-2", "fm-btn fm-btn-sm fm-btn-danger")
        : pluginsButton("Install for me", "download");
      install.dataset.pluginsMine = row.mine.installed ? "uninstall" : "install";
      install.disabled = pluginsConsole.busy;
      actions.append(install);
    }
    const exporter = pluginsButton("Export data", "file-down");
    exporter.dataset.pluginsExport = row.id;
    exporter.disabled = pluginsConsole.busy || !pluginsConsole.detail?.storage?.tables;
    actions.append(exporter);
    return actions;
  }

  function renderPluginsOverview(panel, data) {
    const row = data.package;
    const surfaces = row.surfaces || {};
    const facts = pluginsBox("What it ships");
    facts.append(pluginsFacts([
      ["Kind", row.kind === "plugin" ? "Plugin: separate code added by the build" : row.kind === "switch" ? "Switch over core code" : ""],
      ["In this build", row.inBuild === null ? "Unknown (source checkout)" : row.inBuild ? "Yes" : "No"],
      ["Worker module", surfaces.worker],
      ["API prefix", surfaces.api],
      ["Dashboard page", surfaces.dashboard ? pluginsLink(surfaces.dashboard) : ""],
      ["Site page", surfaces.www ? pluginsLink(surfaces.www) : ""],
      ["Desktop", surfaces.desktop === "module" ? "Separate module" : surfaces.desktop === "core" ? "Linked into the core" : ""],
      ["World", surfaces.world ? "Yes" : "No"],
      ["Data", row.dataScope === "account" ? "Per account" : row.dataScope === "organization" ? "Per organization" : "None on the server"],
      ["Install", row.accountSync ? "Per account, synced to every device" : row.dataScope === "account" ? "Per account, through its own page" : "Per organization"],
      ["Default", row.default ? "Installed by default" : row.installedBefore ? `Installed for accounts made before ${row.installedBefore}` : "Off until chosen"],
      ["Available places", pluginsPlaces(row.places)],
      ["Needs", pluginsPackageLinks(row.dependencies)],
      ["Needed by", pluginsPackageLinks(row.dependents)],
    ]));
    panel.append(facts);
    panel.append(pluginsPolicyEditor(row));
    const installs = data.installs || {};
    const counts = row.installs || {};
    const usage = pluginsBox("Installs");
    const places = installs.places || {};
    usage.append(pluginsFacts([
      ["Accounts with it installed", pluginsCount(counts.accounts)],
      ["Accounts that removed it", pluginsCount(counts.removed)],
      ["Organizations", pluginsCount(counts.organizations)],
      ...pluginsConsole.places.map((place) => [`Shown on ${PLUGINS_PLACE_NAMES[place] || place}`, pluginsCount(places[place])]),
    ]));
    if (row.accountSync && row.mine?.installed) usage.append(pluginsPlacesEditor(row));
    panel.append(usage);
    if (!row.accountSync && row.dataScope !== "account") panel.append(pluginsOrganizations(row, installs.organizations || []));
  }

  function pluginsPolicyEditor(row) {
    const rule = row.policy || { restricted: false, disabled: false, places: row.places || [] };
    const aside = rule.disabled ? "off for everyone" : rule.restricted ? `limited${rule.updatedBy ? ` by ${rule.updatedBy}` : ""}` : "every place it supports";
    const box = pluginsBox("On this instance", aside);
    box.append(pluginsEl("p", "pc-empty", "Where anyone on this instance can use it. Accounts keep their own choice inside these places. Untick every place to turn it off here: accounts stop seeing it, new installs are refused and its API stops answering."));
    const form = pluginsEl("div", "pc-places");
    for (const place of row.places || []) {
      const label = pluginsEl("label");
      const input = pluginsEl("input");
      input.type = "checkbox";
      input.value = place;
      input.dataset.pluginsPolicyPlace = place;
      input.checked = (rule.places || []).includes(place);
      label.append(input, document.createTextNode(PLUGINS_PLACE_NAMES[place] || place));
      form.append(label);
    }
    const save = pluginsButton("Save for this instance", "check");
    save.dataset.pluginsPolicySave = row.id;
    save.disabled = pluginsConsole.busy;
    form.append(save);
    if (rule.restricted) {
      const reset = pluginsButton("Allow everywhere", "rotate-ccw");
      reset.dataset.pluginsPolicyReset = row.id;
      reset.disabled = pluginsConsole.busy;
      form.append(reset);
    }
    box.append(form);
    return box;
  }

  function savePluginsPolicy(reset) {
    const row = pluginsConsole.detail?.package;
    if (!row) return;
    const name = row.name || row.id;
    const places = $$("[data-plugins-policy-place]").filter((input) => input.checked).map((input) => input.value);
    if (!reset && !places.length && !window.confirm(`Turn ${name} off for everyone on this instance? Accounts stop seeing it, new installs are refused and its API stops answering. Its data stays until you remove it.`)) return;
    return pluginsBusy(async () => {
      const payload = await pluginsRequest("POST", `/api/plugins/@console/${encodeURIComponent(row.id)}/policy`, reset ? { reset: true } : { places });
      void window.ForkMeshAPI?.refreshPlugins?.(true);
      await loadPluginsDetail(row.id);
      pluginsConsole.packages = [];
      const rule = payload.policy || {};
      pluginsNotice("[data-plugins-detail-notice]", rule.disabled ? `${name} is off on this instance.` : rule.restricted ? `${name} is limited to ${pluginsPlaces(rule.places)} on this instance.` : `${name} is allowed on every place it supports.`, "success");
    });
  }

  function pluginsPlacesEditor(row) {
    const form = pluginsEl("div", "pc-places");
    form.append(pluginsEl("span", "pc-muted", "My places:"));
    for (const place of row.policy?.places || row.places || []) {
      const label = pluginsEl("label");
      const input = pluginsEl("input");
      input.type = "checkbox";
      input.value = place;
      input.dataset.pluginsPlace = place;
      input.checked = (row.mine.places || []).includes(place);
      label.append(input, document.createTextNode(PLUGINS_PLACE_NAMES[place] || place));
      form.append(label);
    }
    const save = pluginsButton("Save places", "check");
    save.dataset.pluginsPlacesSave = row.id;
    save.disabled = pluginsConsole.busy;
    form.append(save);
    return form;
  }

  function pluginsOrganizations(row, organizations) {
    const box = pluginsBox("Organizations", `${organizations.length} installed`);
    if (!organizations.length) box.append(pluginsEl("p", "pc-empty", "No organization has installed it."));
    else {
      box.append(pluginsTable(["Organization", "Installed by", "Installed"], organizations.map((org) => ({
        cells: [org.name, org.installedBy, pluginsWhen(org.installedAt)],
      }))));
    }
    const manage = pluginsEl("div", "pc-org");
    const input = pluginsEl("input", "pc-input");
    input.type = "text";
    input.placeholder = "Organization name";
    input.setAttribute("aria-label", "Organization to manage");
    input.dataset.pluginsOrgName = "";
    input.value = pluginsConsole.org?.name || "";
    const check = pluginsButton("Check organization", "building-2");
    check.dataset.pluginsOrgCheck = row.id;
    manage.append(input, check);
    const org = pluginsConsole.org;
    if (org?.plugin === row.id) {
      if (org.error) manage.append(pluginsEl("span", "pc-muted", org.error));
      else if (org.entry) {
        manage.append(org.entry.installed ? pluginsLabel(`Installed in ${org.name}`, "success") : pluginsLabel(`Not installed in ${org.name}`));
        if (org.canManage) {
          const action = org.entry.installed
            ? pluginsButton(`Remove from ${org.name}`, "trash-2", "fm-btn fm-btn-sm fm-btn-danger")
            : pluginsButton(`Install in ${org.name}`, "download");
          action.dataset.pluginsOrgAction = org.entry.installed ? "uninstall" : "install";
          action.disabled = pluginsConsole.busy;
          manage.append(action);
          if (org.entry.installed && org.entry.serverData) {
            const exporter = pluginsButton(`Export ${org.name}'s data`, "file-down");
            exporter.dataset.pluginsOrgAction = "export";
            exporter.disabled = pluginsConsole.busy;
            manage.append(exporter);
          }
        } else manage.append(pluginsEl("span", "pc-muted", "Only owners and admins of the organization can change it."));
      }
    }
    box.append(manage);
    return box;
  }

  function renderPluginsData(panel, data) {
    const storage = data.storage || {};
    const tables = data.tables || [];
    const box = pluginsBox("Tables", `${storage.tables || 0} created · ${pluginsCount(storage.rows)} rows · ${pluginsBytes(storage.bytes)}`);
    if (!tables.length) box.append(pluginsEl("p", "pc-empty", "This package keeps no tables on the server."));
    else {
      box.append(pluginsTable(["Table", "Owner", "Rows", "Size", ""], tables.map((table) => {
        let action = pluginsEl("span", "pc-muted", "Not created");
        if (table.exists) {
          action = pluginsButton("Browse", "table");
          action.dataset.pluginsBrowse = table.name;
        }
        return {
          table,
          cells: [table.name, table.owner === "core" ? "Install records (core)" : "Package", pluginsCount(table.rows), pluginsBytes(table.bytes), action],
        };
      }), { code: [0], selected: (item) => item.table.name === pluginsConsole.browse?.table }));
    }
    panel.append(box);
    const browse = pluginsConsole.browse;
    if (!browse) return;
    const viewer = pluginsBox(browse.table, browse.payload ? `${pluginsCount(browse.payload.total)} rows` : "Loading...");
    if (browse.error) viewer.append(pluginsEl("p", "pc-empty", browse.error));
    else if (browse.payload) {
      const payload = browse.payload;
      if (!payload.rows.length) viewer.append(pluginsEl("p", "pc-empty", "No rows."));
      else {
        viewer.append(pluginsTable(payload.columns, payload.rows.map((cells) => ({ cells })),
          { code: payload.columns.map((_, index) => index) }));
      }
      const pager = pluginsEl("div", "pc-pager");
      const first = payload.total ? payload.offset + 1 : 0;
      pager.append(pluginsEl("span", "pc-num", `Rows ${pluginsCount(first)}–${pluginsCount(payload.offset + payload.rows.length)} of ${pluginsCount(payload.total)}, newest first. Long values are cut.`));
      const nav = pluginsEl("span", "pc-actions");
      const previous = pluginsButton("Previous", "chevron-left");
      previous.dataset.pluginsPage = String(Math.max(0, payload.offset - payload.limit));
      previous.disabled = payload.offset <= 0;
      const next = pluginsButton("Next", "chevron-right");
      next.dataset.pluginsPage = String(payload.offset + payload.limit);
      next.disabled = payload.offset + payload.rows.length >= payload.total;
      nav.append(previous, next);
      pager.append(nav);
      viewer.append(pager);
    }
    panel.append(viewer);
  }

  async function browsePluginsTable(table, offset) {
    const id = pluginsConsole.detail?.package?.id;
    if (!id) return;
    pluginsConsole.browse = { table, offset, payload: pluginsConsole.browse?.table === table ? pluginsConsole.browse.payload : null };
    renderPluginsDetail();
    try {
      const query = new URLSearchParams({ table, offset: String(offset), limit: "50" });
      const payload = await pluginsRequest("GET", `/api/plugins/@console/${encodeURIComponent(id)}/rows?${query}`);
      if (pluginsConsole.browse?.table !== table) return;
      pluginsConsole.browse = { table, offset, payload };
    } catch (error) {
      pluginsConsole.browse = { table, offset, error: error.message };
    }
    renderPluginsDetail();
  }

  function renderPluginsLogs(panel, data) {
    const logs = data.logs || {};
    const errors = logs.errors || [];
    const days = logs.windowDays || 30;
    const errorBox = pluginsBox("Errors", `last ${days} days · ${(logs.prefixes || []).join(", ") || "no routes"}`);
    if (!errors.length) errorBox.append(pluginsEl("p", "pc-empty", "No errors on this package's routes."));
    else {
      errorBox.append(pluginsTable(["When", "Status", "Request", "Message", "Account"], errors.map((item) => ({
        cells: [pluginsWhen(item.ts), item.status, `${item.method} ${item.path}`, item.message, item.actor],
      })), { code: [2] }));
    }
    panel.append(errorBox);
    const activity = logs.activity || [];
    const activityBox = pluginsBox("Activity", `last ${days} days · installs, removals, settings, places and console reads`);
    if (!activity.length) activityBox.append(pluginsEl("p", "pc-empty", "No recorded activity."));
    else {
      activityBox.append(pluginsTable(["When", "Who", "Action", "Scope", "Outcome"], activity.map((item) => ({
        cells: [pluginsWhen(item.ts), item.actor, item.action, item.scope.replace(/_/g, " "), item.outcome],
      })), { code: [2] }));
    }
    panel.append(activityBox);
  }

  function renderPluginsSettings(panel, data) {
    const row = data.package;
    const settings = row.settings || [];
    const box = pluginsBox("Settings it declares", `${settings.length}`);
    if (!settings.length) box.append(pluginsEl("p", "pc-empty", "No settings."));
    else {
      box.append(pluginsTable(["Key", "Label", "Type", "Default", "Choices"], settings.map((item) => ({
        cells: [item.key, item.label, item.type, String(item.default), (item.options || []).map((option) => option.label || option.value).join(", ")],
      })), { code: [0] }));
    }
    panel.append(box);
    const permissions = row.permissions || [];
    const permBox = pluginsBox("Permissions", `${permissions.length}`);
    if (!permissions.length) permBox.append(pluginsEl("p", "pc-empty", "It asks for no permissions."));
    else {
      permBox.append(pluginsTable(["Permission", "Risk", "Why"], permissions.map((item) => ({
        cells: [item.id, item.risk || "", item.reason || item.description || ""],
      })), { code: [0] }));
    }
    panel.append(permBox);
    const origins = row.outboundOrigins || [];
    const originBox = pluginsBox("Outside services its Worker may call", `${origins.length}`);
    if (!origins.length) originBox.append(pluginsEl("p", "pc-empty", "None."));
    else originBox.append(pluginsTable(["Origin"], origins.map((origin) => ({ cells: [origin] })), { code: [0] }));
    panel.append(originBox);
  }

  function pluginsDownload(filename, payload) {
    const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const link = pluginsEl("a");
    link.href = url;
    link.download = filename;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  async function pluginsBusy(work) {
    if (pluginsConsole.busy) return;
    pluginsConsole.busy = true;
    renderPluginsDetail();
    try {
      await work();
    } catch (error) {
      pluginsNotice("[data-plugins-detail-notice]", error.message, "danger");
    }
    pluginsConsole.busy = false;
    renderPluginsDetail();
  }

  function exportPlugins(id) {
    return pluginsBusy(async () => {
      const payload = await pluginsRequest("GET", `/api/plugins/@console/${encodeURIComponent(id)}/export`);
      pluginsDownload(payload.filename || `forkmesh-${id}-tables.json`, payload);
      pluginsNotice("[data-plugins-detail-notice]", payload.truncated ? "Exported. The file was cut at the export limit; browse the tables for the rest." : "Exported every table of this package.", payload.truncated ? "info" : "success");
    });
  }

  function setMinePlugins(action) {
    const row = pluginsConsole.detail?.package;
    if (!row) return;
    const name = row.name || row.id;
    if (action === "uninstall" && !window.confirm(`Remove ${name} from your account? Its links and screens disappear from your web, desktop and phone apps, and anything it stored for your account is deleted.`)) return;
    return pluginsBusy(async () => {
      const payload = await pluginsRequest("POST", `/api/plugins/@account/${encodeURIComponent(row.id)}/${action}`, {});
      void window.ForkMeshAPI?.refreshPlugins?.(true);
      const changed = Array.isArray(payload.changed) ? payload.changed.filter((id) => id !== row.id) : [];
      await loadPluginsDetail(row.id);
      pluginsConsole.packages = [];
      pluginsNotice("[data-plugins-detail-notice]", `${name} was ${action === "install" ? "installed" : "removed"} for your account.${changed.length ? ` Also changed: ${changed.join(", ")}.` : ""}`, "success");
    });
  }

  function savePluginsPlaces() {
    const row = pluginsConsole.detail?.package;
    const places = $$("[data-plugins-place]").filter((input) => input.checked).map((input) => input.value);
    if (!row) return;
    if (!places.length) {
      pluginsNotice("[data-plugins-detail-notice]", pluginsErrorText("invalid_places"), "danger");
      return;
    }
    return pluginsBusy(async () => {
      await pluginsRequest("POST", `/api/plugins/@account/${encodeURIComponent(row.id)}/places`, { places });
      void window.ForkMeshAPI?.refreshPlugins?.(true);
      await loadPluginsDetail(row.id);
      pluginsNotice("[data-plugins-detail-notice]", `${row.name || row.id} now shows on ${pluginsPlaces(places)} for your account.`, "success");
    });
  }

  async function checkPluginsOrganization(id) {
    const name = String($("[data-plugins-org-name]")?.value || "").trim().toLowerCase();
    if (!name) return;
    pluginsConsole.org = { plugin: id, name };
    try {
      const payload = await pluginsRequest("GET", `/api/plugins/${encodeURIComponent(name)}`);
      const entry = (payload.plugins || []).find((item) => item.id === id) || null;
      pluginsConsole.org = { plugin: id, name, entry, canManage: payload.canManage === true, error: entry ? "" : "This organization cannot install it." };
    } catch (error) {
      pluginsConsole.org = { plugin: id, name, error: error.message };
    }
    renderPluginsDetail();
  }

  function runPluginsOrgAction(action) {
    const org = pluginsConsole.org;
    const row = pluginsConsole.detail?.package;
    if (!org?.name || !row) return;
    const name = row.name || row.id;
    if (action === "uninstall" && !window.confirm(`Remove ${name} from ${org.name}? Its data is downloaded first, then deleted for that organization.`)) return;
    return pluginsBusy(async () => {
      const method = action === "export" ? "GET" : "POST";
      const payload = await pluginsRequest(method, `/api/plugins/${encodeURIComponent(org.name)}/${encodeURIComponent(row.id)}/${action}`, method === "POST" ? {} : undefined);
      if (payload.export) pluginsDownload(payload.filename || `forkmesh-${row.id}-${org.name}.json`, payload.export);
      await loadPluginsDetail(row.id);
      await checkPluginsOrganization(row.id);
      pluginsConsole.packages = [];
      const done = action === "install" ? `Installed ${name} in ${org.name}.` : action === "uninstall" ? `Removed ${name} from ${org.name}.${payload.hadData ? " Its data was downloaded." : ""}` : `Downloaded ${org.name}'s ${name} data.`;
      pluginsNotice("[data-plugins-detail-notice]", done, "success");
    });
  }

  function initPluginsPage() {
    const view = $('[data-view="plugins"]');
    if (!view) return;
    $("[data-plugins-filter]")?.addEventListener("input", renderPluginsList);
    $("[data-plugins-kind]")?.addEventListener("change", renderPluginsList);
    $("[data-plugins-refresh]")?.addEventListener("click", () => void loadPluginsList());
    $("[data-plugins-back]")?.addEventListener("click", (event) => {
      event.preventDefault();
      pluginsNavigate("");
    });
    view.addEventListener("click", (event) => {
      const open = event.target.closest("[data-plugins-open]");
      if (open) {
        event.preventDefault();
        pluginsNavigate(open.dataset.pluginsOpen);
        return;
      }
      const tab = event.target.closest("[data-plugins-tab]");
      if (tab) {
        pluginsConsole.tab = tab.dataset.pluginsTab;
        const url = new URL(location.href);
        url.searchParams.set("tab", pluginsConsole.tab);
        history.replaceState(null, "", `${url.pathname}${url.search}`);
        renderPluginsDetail();
        return;
      }
      const browse = event.target.closest("[data-plugins-browse]");
      if (browse) return void browsePluginsTable(browse.dataset.pluginsBrowse, 0);
      const page = event.target.closest("[data-plugins-page]");
      if (page && pluginsConsole.browse) return void browsePluginsTable(pluginsConsole.browse.table, Number(page.dataset.pluginsPage) || 0);
      const exporter = event.target.closest("[data-plugins-export]");
      if (exporter) return void exportPlugins(exporter.dataset.pluginsExport);
      const mine = event.target.closest("[data-plugins-mine]");
      if (mine) return void setMinePlugins(mine.dataset.pluginsMine);
      if (event.target.closest("[data-plugins-places-save]")) return void savePluginsPlaces();
      if (event.target.closest("[data-plugins-policy-save]")) return void savePluginsPolicy(false);
      if (event.target.closest("[data-plugins-policy-reset]")) return void savePluginsPolicy(true);
      const check = event.target.closest("[data-plugins-org-check]");
      if (check) return void checkPluginsOrganization(check.dataset.pluginsOrgCheck);
      const orgAction = event.target.closest("[data-plugins-org-action]");
      if (orgAction) return void runPluginsOrgAction(orgAction.dataset.pluginsOrgAction);
    });
    view.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && event.target.matches("[data-plugins-org-name]")) {
        const id = pluginsConsole.detail?.package?.id;
        if (id) void checkPluginsOrganization(id);
      }
    });
    window.addEventListener("popstate", () => void pluginsRoute());
    void pluginsRoute();
  }

