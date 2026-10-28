// Organization CRM plugin. The package registers against the bounded dashboard
// socket and keeps all implementation state inside its initializer.
window.ForkMeshDashboard.registerPage("crm", ({
  select: $, selectAll: $$, session, escapeHtml, relativeTimeLabel,
}) => {
  const state = {
    get session() { return session(); },
    crmView: {
    organizations: [], organization: "", role: "", canManageSettings: false,
    installed: false,
    contacts: [], emails: [], mail: [], settings: {}, summary: {}, loading: true,
    // The summary revision last applied (see loadCrm).
    revision: "",
    // Founders outreach: access, templates, roster, and recent sends for the
    // selected organization (null until the summary has loaded).
    outreach: null,
    views: [], members: [], activeView: "",
    stageTab: "", virtualColumns: {}, pipelineEdit: null, membersView: "",
    // Sheets other organizations shared with this account, read-only. The
    // detail (columns + rows) is fetched when its tab is first opened.
    sharedViews: [], activeShared: "", sharedCache: {},
    // "table" or "board"; remembered per organization in this browser.
    layout: "table", layoutOrg: "",
    // Which tab of the page is showing: contacts, inbox, or outreach.
    section: "contacts",
    // Investor deck links minted for contacts, with their view totals.
    decks: [], deckContactId: "",
    activity: { days: [], kinds: [], members: [] },
    teamPeriod: 30, teamMember: "", teamTable: false,
    // The contact whose detail card is open, and the posts and picture each
    // of its connected accounts answered with, keyed "service:handle".
    detailContactId: "", detailUpdates: {},
    nextSkipped: new Set(),
    ai: null,
    },
  };

  // The ForkMesh browser extension is its own Market package now
  // (marketplace/packages/browser-extension); its page installs and updates
  // it. This is the oldest build whose CRM panel speaks this package's API.
  const CRM_EXTENSION_MINIMUM = "2.0.0";
  const CRM_EXTENSION_PAGE = "/dashboard/extension";
  const CRM_ZOHO_MAIL_HOSTS = {
    com: "mail.zoho.com", eu: "mail.zoho.eu", in: "mail.zoho.in",
    "com.au": "mail.zoho.com.au", jp: "mail.zoho.jp", ca: "mail.zohocloud.ca",
  };
  // Flows past this many are dropped by the Worker when settings are saved.
  const CRM_MAX_FLOWS = 12;
  const CRM_PIPELINE_TAB_PREFIX = "pipeline:";
  const CRM_NO_PIPELINE_TAB = "no-pipeline";
  const CRM_STAGE_SHAPE = /^[a-z][a-z0-9-]*$/;
  const CRM_SHARE_SHOWN = 15;
  const CRM_STAGE_PALETTE = ["#58a6ff", "#e3b341", "#bc8cff", "#39c5cf", "#db61a2", "#3fb950", "#db6d28", "#f85149"];

  // A wedged Worker isolate answers in a few milliseconds with a 500
  // (Pyodide "NoGilError") or a "hung" cancel before any CRM code runs, and
  // the next request usually lands on a healthy isolate. Reads retry those
  // blips; writes are never replayed.
  const CRM_READ_RETRY_DELAYS_MS = [400, 1500];
  const CRM_LIST_VIEW_GAP_MS = 30 * 60 * 1000;
  const CRM_TOOT_SYNC_MS = 10 * 60 * 1000;

  function crmTransientStatus(status) {
    return status === 0 || status === 408 || status === 425 || status >= 500;
  }

  async function crmRequest(path, method = "GET", body = null) {
    const headers = { accept: "application/json" };
    const token = state.session?.sessionToken || "";
    if (token) headers.authorization = `Bearer ${token}`;
    if (body !== null) headers["content-type"] = "application/json";
    // A write may already show in the local rows; the next re-read must bring
    // the whole summary back even if the write fails.
    if (method !== "GET") state.crmView.revision = "";
    const retryDelays = method === "GET" ? CRM_READ_RETRY_DELAYS_MS : [];
    for (let attempt = 0; ; attempt += 1) {
      const last = attempt >= retryDelays.length;
      let response;
      try {
        response = await fetch(path, {
          method, headers, cache: "no-store",
          body: body === null ? undefined : JSON.stringify(body),
        });
      } catch (cause) {
        if (last) throw cause;
      }
      if (response) {
        const payload = await response.json().catch(() => ({}));
        if (response.ok && payload.ok !== false) return payload;
        if (last || !crmTransientStatus(response.status)) {
          const error = new Error(String(payload.error || `HTTP ${response.status}`).replaceAll("_", " "));
          error.status = response.status; error.payload = payload; throw error;
        }
      }
      await new Promise((resolve) => setTimeout(resolve, retryDelays[attempt]));
    }
  }

  function crmMessage(kind, message = "") {
    const box = $(`[data-crm-${kind}]`); if (!box) return;
    box.textContent = message; box.classList.toggle("hidden", !message);
    if (message && kind === "error") $(`[data-crm-notice]`)?.classList.add("hidden");
  }

  function crmOrganizationPath(suffix = "") {
    const org = state.crmView.organization;
    return org ? `/api/crm/${encodeURIComponent(org)}${suffix}` : "/api/crm";
  }

  function crmPluginPath(suffix = "") {
    return `/api/plugins/${encodeURIComponent(state.crmView.organization)}/crm${suffix}`;
  }

  function crmSelectedOrg() {
    return state.crmView.organizations.find((org) => org.name === state.crmView.organization) || null;
  }

  function crmCanManage() {
    return ["owner", "admin"].includes(crmSelectedOrg()?.role || "");
  }

  function renderCrmInstallState() {
    const installed = Boolean(crmSelectedOrg()?.installed);
    const canManage = crmCanManage();
    state.crmView.installed = installed;
    $(`[data-crm-install-panel]`)?.classList.toggle("hidden", installed);
    $(`[data-crm-app]`)?.classList.toggle("hidden", !installed);
    $$(`[data-crm-requires-install]`).forEach((button) =>
      button.classList.toggle("hidden", !installed));
    const orgLabel = $(`[data-crm-install-org]`);
    if (orgLabel) orgLabel.textContent = state.crmView.organization;
    $(`[data-crm-install]`)?.classList.toggle("hidden", !canManage);
    $(`[data-crm-install-denied]`)?.classList.toggle("hidden", canManage);
  }

  function downloadCrmJson(exportPayload, filename) {
    const url = URL.createObjectURL(new Blob(
      [JSON.stringify(exportPayload, null, 2)], { type: "application/json" }));
    const link = document.createElement("a"); link.href = url;
    link.download = filename || "forkmesh-crm-export.json"; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  async function installCrmPlugin() {
    const button = $(`[data-crm-install]`); if (button) button.disabled = true;
    try {
      await crmRequest(crmPluginPath("/install"), "POST", {});
      const record = crmSelectedOrg(); if (record) record.installed = true;
      crmMessage("notice", "CRM installed for this organization."); await loadCrm();
    } catch (error) { crmMessage("error", `Could not install the CRM plugin: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }

  async function exportCrmAll() {
    try {
      const payload = await crmRequest(crmPluginPath("/export"));
      downloadCrmJson(payload.export || {}, payload.filename);
      crmMessage("notice", "CRM data exported as JSON.");
    } catch (error) { crmMessage("error", `Export failed: ${error.message}`); }
  }

  async function uninstallCrmPlugin() {
    const org = state.crmView.organization;
    if (!confirm(`Uninstall the CRM plugin from ${org}?\n\nYour data is downloaded first, then every contact, email record, and setting is deleted for the whole organization. Once no organization keeps the plugin installed, its database tables are removed too.`)) return;
    try {
      // Download before deleting so the data always survives the uninstall.
      const snapshot = await crmRequest(crmPluginPath("/export"));
      if (snapshot.hasData) downloadCrmJson(snapshot.export || {}, snapshot.filename);
      await crmRequest(crmPluginPath("/uninstall"), "POST", {});
      const record = crmSelectedOrg(); if (record) record.installed = false;
      $(`[data-crm-settings-dialog]`)?.close();
      crmMessage("notice", snapshot.hasData
        ? "CRM uninstalled. Your data download has started."
        : "CRM uninstalled. There was no stored data.");
      await loadCrm();
    } catch (error) { crmMessage("error", `Could not uninstall: ${error.message}`); }
  }

  function crmContactById(id) {
    return state.crmView.contacts.find((contact) => contact.id === id) || null;
  }

  function crmStatusClass(status) {
    return ["failed", "reject", "bounce", "soft bounce", "suspension"].includes(status)
      ? "text-destructive" : ["delivery", "delivered", "open", "click"].includes(status)
      ? "text-primary" : "text-muted-foreground";
  }

  function crmFlows() {
    return Array.isArray(state.crmView.settings?.flows) ? state.crmView.settings.flows : [];
  }

  function crmFlow(id) {
    const shared = crmActiveShared();
    if (shared?.flow && shared.flow === id && shared.stages?.length) {
      return { id: shared.flow, name: shared.name, stages: shared.stages };
    }
    return crmFlows().find((flow) => flow.id === id) || crmFlows()[0] ||
      { id: "customers", name: "Customers", stages: ["lead"] };
  }

  // The kinds of pipeline a flow can be (the Worker's FLOW_ICONS), drawn the
  // way the desktop draws them: a glyph on a 16px grid in the kind's color.
  const CRM_FLOW_ICONS = [
    { id: "pipeline", label: "Pipeline", color: "#6e7f96",
      body: `<path d="M1.75 2.25h12.5L9.5 8.5v5.25l-3-1.5V8.5z"/>` },
    { id: "investors", label: "Investors", color: "#2da44e",
      body: `<circle cx="8" cy="8" r="6.25"/><path d="M10 5.5H7.25a1.25 1.25 0 0 0 0 2.5h1.5a1.25 1.25 0 0 1 0 2.5H6M8 4v1.5M8 10.5V12"/>` },
    { id: "hiring", label: "Hiring", color: "#8250df",
      body: `<circle cx="8" cy="4.5" r="2.75"/><path d="M2.25 14.25c0-5.5 11.5-5.5 11.5 0"/>` },
    { id: "partners", label: "Partnerships", color: "#1b9aaa",
      body: `<circle cx="5.5" cy="8" r="4"/><circle cx="10.5" cy="8" r="4"/>` },
    { id: "customers", label: "Customers", color: "#388bfd",
      body: `<path d="M8 2l1.7 3.9 4.2.4-3.2 2.8 1 4.1L8 11.1l-3.7 2.1 1-4.1-3.2-2.8 4.2-.4z"/>` },
    { id: "sales", label: "Sales", color: "#d29922",
      body: `<path d="M3.5 14V9.5M8 14V6.5M12.5 14V3" stroke-width="3" stroke-linecap="butt"/>` },
    { id: "support", label: "Support", color: "#db6158",
      body: `<rect x="1.75" y="2.25" width="12.5" height="8.75" rx="2.5"/><path d="M4.5 11L4 14l4-3"/>` },
    { id: "projects", label: "Projects", color: "#57606a",
      body: `<rect x="1.75" y="1.75" width="12.5" height="12.5" rx="2.5"/><path d="M4.75 8.25L7 10.5l4.25-5"/>` },
    { id: "events", label: "Events", color: "#e16f24",
      body: `<rect x="1.75" y="3" width="12.5" height="11.25" rx="2"/><path d="M1.75 6.75h12.5M5 1.5V4M11 1.5V4"/>` },
    { id: "grants", label: "Grants", color: "#bf8700",
      body: `<circle cx="8" cy="5.75" r="4.25"/><path d="M5.5 9.25l-.75 5.25L8 12.75l3.25 1.75-.75-5.25"/>` },
  ];

  function crmFlowIcon(icon, size = 14) {
    const kind = CRM_FLOW_ICONS.find((item) => item.id === icon) || CRM_FLOW_ICONS[0];
    return `<svg data-crm-flow-icon-glyph="${kind.id}" viewBox="0 0 16 16" width="${size}" height="${size}" fill="none" stroke="${kind.color}" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" style="flex-shrink:0" role="img" aria-label="${kind.label} pipeline">${kind.body}</svg>`;
  }

  function crmFlowIconOptions(selected) {
    return CRM_FLOW_ICONS.map((kind) =>
      `<option value="${kind.id}"${kind.id === selected ? " selected" : ""}>${kind.label}</option>`).join("");
  }

  function titleCaseSlug(value) {
    return String(value || "").replaceAll("-", " ").replace(/\b\w/g, (letter) => letter.toUpperCase());
  }

  function renderCrmFlowFilters() {
    const selectedFlow = String($(`[data-crm-flow]`)?.value || "");
    const flowSelect = $(`[data-crm-flow]`);
    if (flowSelect) {
      flowSelect.innerHTML = `<option value="">All flows</option>` + crmFlows().map((flow) =>
        `<option value="${escapeHtml(flow.id)}">${escapeHtml(flow.name)}</option>`).join("");
      flowSelect.value = crmFlows().some((flow) => flow.id === selectedFlow) ? selectedFlow : "";
    }
    const selectedStage = String($(`[data-crm-stage]`)?.value || "");
    const stages = [...new Set(crmFlows().flatMap((flow) => flow.stages || []))];
    const stageSelect = $(`[data-crm-stage]`);
    if (stageSelect) {
      stageSelect.innerHTML = `<option value="">All stages</option>` + stages.map((stage) =>
        `<option value="${escapeHtml(stage)}">${escapeHtml(titleCaseSlug(stage))}</option>`).join("");
      stageSelect.value = stages.includes(selectedStage) ? selectedStage : "";
    }
    const ownerSelect = $(`[data-crm-owner]`);
    if (ownerSelect) {
      const selectedOwner = String(ownerSelect.value || "");
      const owners = [...new Set([...(state.crmView.members || []),
        ...state.crmView.contacts.map((contact) => String(contact.owner || "").trim().toLowerCase())]
        .filter(Boolean))].sort();
      ownerSelect.innerHTML = `<option value="">Any owner</option><option value="@me">My contacts</option><option value="@none">Unassigned</option>`
        + owners.map((owner) => `<option value="${escapeHtml(owner)}">${escapeHtml(owner)}</option>`).join("");
      ownerSelect.value = ["@me", "@none", ...owners].includes(selectedOwner) ? selectedOwner : "";
    }
  }

  function crmOwnerMatches(contact, owner) {
    const current = String(contact.owner || "").trim().toLowerCase();
    if (owner === "@none") return !current;
    if (owner === "@me") return Boolean(current) && current === String(state.session?.name || "").trim().toLowerCase();
    return current === owner;
  }

  function crmWaitingOnUs(contact) {
    let latest = null;
    for (const email of state.crmView.emails) {
      if (email.contactId !== contact.id) continue;
      const at = Number(email.sentAt || email.createdAt || 0);
      if (!latest || at > latest.at) latest = { at, inbound: email.direction === "inbound" };
    }
    return Boolean(latest?.inbound) && !CRM_CLOSED_STAGES.has(String(contact.stage || ""));
  }

  function crmInMyQueue(contact) {
    if (!crmOwnerMatches(contact, "@me") || CRM_CLOSED_STAGES.has(String(contact.stage || ""))) return false;
    return crmWaitingOnUs(contact) || Boolean(String(contact.nextStep || "").trim());
  }

  async function loadCrmDuplicates() {
    if (state.crmView.duplicates?.organization === state.crmView.organization) return;
    try {
      const payload = await crmRequest(crmOrganizationPath("/contacts/duplicates"));
      const order = new Map(); const signals = new Map();
      (payload.groups || []).forEach((group, index) => group.contacts.forEach((id) => {
        order.set(id, index); signals.set(id, group.signals.join(", "));
      }));
      state.crmView.duplicates = { organization: state.crmView.organization, order, signals };
      crmMessage("notice", `${(payload.groups || []).length} possible duplicate group${(payload.groups || []).length === 1 ? "" : "s"}: right-click a row to merge it into the one to keep.`);
    } catch (error) {
      state.crmView.duplicates = null;
      crmMessage("error", `Could not look for duplicates: ${error.message}`);
    }
    renderCrmContacts();
  }

  function crmFocusChanged() {
    if ($(`[data-crm-focus]`)?.value === "duplicates") {
      state.crmView.duplicates = null;
      void loadCrmDuplicates();
    }
    renderCrmContacts();
  }

  function crmFocusMatches(contact, focus) {
    if (focus === "waiting") return crmWaitingOnUs(contact);
    if (focus === "my-queue") return crmInMyQueue(contact);
    if (focus === "duplicates") return Boolean(state.crmView.duplicates?.order.has(contact.id));
    if (focus === "paying") return Boolean(contact.account?.paid?.active);
    if (focus === "evidence-ahead") return Boolean(contact.account?.suggestedStage) && contact.account.suggestedStage !== contact.stage;
    if (focus === "next-step") return Boolean(String(contact.nextStep || "").trim());
    if (focus === "uncontacted") return !Number(contact.lastContactedAt);
    if (focus === "quiet") {
      const days = crmDaysSince(contact.lastContactedAt);
      return days !== null && days >= CRM_QUIET_DAYS && !CRM_CLOSED_STAGES.has(String(contact.stage || ""));
    }
    return true;
  }

  // One definition per editable contact column, shared by the table, the mobile
  // cards, and the inline-edit whitelist. Widths are inline styles because they
  // are data, one per column, rather than a fixed set of utility classes.
  const CRM_CONTACT_COLUMNS = [
    { field: "name", label: "Name", width: "13rem" },
    { field: "email", label: "Email", type: "email", width: "15rem" },
    { field: "company", label: "Company", width: "12rem" },
    { field: "title", label: "Title", width: "11rem" },
    { field: "phone", label: "Phone", type: "tel", width: "10rem" },
    { field: "twitter", label: "Twitter", width: "11rem", hint: "@handle or profile URL" },
    { field: "mastodon", label: "Mastodon", width: "14rem", hint: "@name@instance" },
    { field: "bluesky", label: "Bluesky", width: "13rem", hint: "name.bsky.social" },
    { field: "linkedin", label: "LinkedIn", width: "14rem", hint: "linkedin.com/in/name" },
    { field: "github", label: "GitHub", width: "10rem", hint: "@username" },
    { field: "website", label: "Website", type: "url", width: "13rem", hint: "https://example.com" },
    { field: "location", label: "Location", width: "11rem", hint: "City, country" },
    { field: "flow", label: "Flow", select: true, width: "10rem" },
    { field: "stage", label: "Stage", select: true, width: "12rem" },
    { field: "value", label: "Deal value", type: "number", width: "9rem", hint: "0" },
    { field: "nextStep", label: "Next step", width: "16rem", hint: "Send the deck" },
    { field: "owner", label: "Owner", width: "10rem" },
    { field: "source", label: "Source", width: "9rem" },
    { field: "tags", label: "Tags", width: "12rem" },
    { field: "notes", label: "Notes", width: "20rem" },
  ];
  const CRM_CONTACT_FIELDS = CRM_CONTACT_COLUMNS.map((column) => column.field);
  // Tags parse into an array, flow and stage are selects rebuilt per flow.
  const CRM_DIALOG_FIELDS = CRM_CONTACT_FIELDS.filter(
    (field) => !["tags", "flow", "stage"].includes(field));
  const CRM_SEARCH_FIELDS = ["name", "email", "company", "title", "phone", "twitter", "mastodon",
    "bluesky", "linkedin", "github", "website", "location", "owner", "source", "notes",
    "nextStep", "flow", "stage"];
  // Linking a message moves this date forward on its own, but it is also a
  // judgement call ("we spoke at the conference"), so it edits as a date in
  // place. It closes out the canonical column order the sheets use.
  const CRM_HISTORY_COLUMN = { field: "lastContactedAt", label: "Last contacted", width: "11rem", type: "date" };
  const CRM_BASE_COLUMNS = [...CRM_CONTACT_COLUMNS, CRM_HISTORY_COLUMN];
  const CRM_COMPUTED_COLUMNS = [
    { field: "mastodon-latest", label: "Mastodon last update", width: "20rem", computed: true },
    { field: "linked-emails", label: "Linked emails", width: "20rem", computed: true },
    { field: "linked-toots", label: "Linked toots", width: "20rem", computed: true },
  ];
  const CRM_KEPT_COLUMNS = new Set(["name", "email", "flow", "stage", "lastContactedAt"]);
  // What a layout shows before anyone has shaped it. Phone, profiles, source
  // and notes stay one click away in the Columns menu; profile handles still
  // show as small links under the name while their columns are hidden.
  const CRM_DEFAULT_COLUMNS = new Set(["name", "email", "company", "title", "flow",
    "stage", "owner", "tags", "lastContactedAt"]);
  const CRM_DEFAULT_VIEW_FIELDS = ["name", "email", "mastodon", "bluesky", "twitter", "linkedin", "github",
    "lastContactedAt"];
  const CRM_BUILTIN_TABLE_VIEWS = [{ id: "default", name: "Default" }, { id: "all", name: "All" }];
  const CRM_PROFILE_FIELDS = ["twitter", "mastodon", "bluesky", "linkedin", "github", "website"];
  const CRM_PROFILE_BADGES = { twitter: "X", mastodon: "Md", bluesky: "Bs", linkedin: "in", github: "gh", website: "www" };
  // Stage names that close a relationship out, one way or the other, across
  // the starter flows. They only pick a colour; any stage name still works.
  const CRM_WON_STAGES = new Set(["customer", "committed", "hired", "active", "awarded"]);
  const CRM_CLOSED_STAGES = new Set(["inactive", "passed", "rejected", "closed", "declined"]);
  const CRM_QUIET_DAYS = 30;
  const CRM_BULK_EDIT_LIMIT = 500;

  // --- sheets and their column layouts -----------------------------------
  // A sheet is a saved column layout over the shared contacts: a CSV import
  // creates one named after the file, ordered like the file.

  function crmActiveView() {
    return crmTabViews().find((view) => view.id === state.crmView.activeView) || null;
  }

  function crmVirtualViews() {
    const bound = new Set(state.crmView.views.map((view) => view.flow).filter(Boolean));
    return crmFlows().filter((flow) => flow.id && !bound.has(flow.id)).map((flow) => ({
      id: `${CRM_PIPELINE_TAB_PREFIX}${flow.id}`, name: flow.name || flow.id, flow: flow.id, virtual: true,
    }));
  }

  function crmNoPipelineView() {
    return { id: CRM_NO_PIPELINE_TAB, name: "No pipeline", virtual: true };
  }

  function crmPipelineIds() {
    return new Set([...state.crmView.views, ...crmVirtualViews()].map((view) => view.flow).filter(Boolean));
  }

  function crmContactOnTab(contact, view, pipelines = crmPipelineIds()) {
    return view.id === CRM_NO_PIPELINE_TAB
      ? !pipelines.has(String(contact.flow || "")) : crmContactInView(contact, view);
  }

  function crmTabViews() {
    const pipelines = crmPipelineIds();
    const noPipeline = crmNoPipelineView();
    const orphans = state.crmView.contacts.some((contact) => crmContactOnTab(contact, noPipeline, pipelines));
    return [...state.crmView.views, ...crmVirtualViews(), ...(orphans ? [noPipeline] : [])];
  }

  function crmTabView(id) {
    return id === CRM_NO_PIPELINE_TAB ? crmNoPipelineView()
      : crmTabViews().find((view) => view.id === id) || null;
  }

  function crmSyncActiveTab() {
    if (state.crmView.activeShared || state.crmView.sharedOnly) return;
    const tabs = crmTabViews();
    const previous = state.crmView.activeView;
    if (!tabs.some((view) => view.id === previous)) {
      state.crmView.activeView = tabs[0]?.id || "";
      if (previous && new URL(location.href).searchParams.get("list") === previous) crmSetListParam("");
    }
    selectCrmFirstStage();
  }

  function crmFirstStage(stages) {
    return stages.find((stage) => stage.toLowerCase() === "new") || stages[0] || "";
  }

  function selectCrmFirstStage() {
    const view = crmActiveView();
    if (!view || view.id === state.crmView.stageTab) return;
    const { stages } = crmStripStages(view, view.flow ? "" : String($(`[data-crm-flow]`)?.value || ""));
    if (!stages.length) return;
    state.crmView.stageTab = view.id;
    const select = $(`[data-crm-stage]`);
    if (!select) return;
    const first = crmFirstStage(stages);
    select.value = [...select.options].some((option) => option.value === first) ? first : "";
  }

  function crmFlowStages(flowId) {
    return flowId ? [...(crmFlows().find((flow) => flow.id === flowId)?.stages || [])] : [];
  }

  function crmStripStages(view, flowFilter = "") {
    const flow = view?.flow || flowFilter || "";
    const stages = crmFlowStages(flow);
    const counts = new Map();
    if (!view) return { stages: [], counts };
    const pipelines = crmPipelineIds();
    for (const contact of state.crmView.contacts) {
      const stage = String(contact.stage || "");
      if (!crmContactOnTab(contact, view, pipelines) || (flow && contact.flow !== flow) || !stage) continue;
      counts.set(stage, (counts.get(stage) || 0) + 1);
    }
    if (!flow) {
      const order = [...new Set(crmFlows().flatMap((item) => item.stages || []))];
      stages.push(...order.filter((stage) => counts.has(stage)));
      const others = [...counts.keys()].filter((stage) => !stages.includes(stage));
      others.sort((a, b) => (a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0));
      stages.push(...others);
    }
    return { stages: [...new Set(stages.filter(Boolean))], counts };
  }

  function crmStageLabel(name) {
    const label = String(name || "").replace(/[-_]+/g, " ").replace(/\s+/g, " ").trim();
    return label ? label[0].toUpperCase() + label.slice(1) : "No stage";
  }

  function crmStageColor(name, index) {
    const key = String(name || "").toLowerCase();
    const any = (words) => words.some((word) => key.includes(word));
    if (any(["lost", "churn", "reject", "dead", "disqualif", "cancel"])) return "#f85149";
    if (any(["won", "complete", "done", "customer", "closed", "signed", "paid", "success"])) return "#3fb950";
    if (any(["progress", "contacted", "working", "active", "follow", "ongoing"])) return "#e3b341";
    if (any(["qualif", "proposal", "negotiat", "demo", "meeting", "review", "trial"])) return "#bc8cff";
    if (any(["new", "lead", "prospect", "inbox", "todo", "open", "backlog"])) return "#58a6ff";
    return CRM_STAGE_PALETTE[Math.max(0, Number(index) || 0) % CRM_STAGE_PALETTE.length];
  }

  function crmStageInk(color) {
    const value = parseInt(String(color).slice(1), 16);
    const channel = (shift) => ((value >> shift) & 255) / 255;
    return 0.2126 * channel(16) + 0.7152 * channel(8) + 0.0722 * channel(0) > 0.55 ? "#1f2328" : "#ffffff";
  }

  function crmStageStripHtml(stages, counts, selected) {
    return stages.map((stage, index) => {
      const color = crmStageColor(stage, index);
      const count = counts.get(stage) || 0;
      const label = crmStageLabel(stage);
      const picked = stage === selected;
      const tip = `${count.toLocaleString()} ${count === 1 ? "contact" : "contacts"} at ${label}\n${picked ? "Click to show every stage" : "Click to show only this stage"}`;
      return `<button type="button" data-crm-stage-chip="${escapeHtml(stage)}" aria-pressed="${picked}" title="${escapeHtml(tip)}" style="--crm-chip:${color};--crm-chip-ink:${crmStageInk(color)}" class="crm-stage-chip${count ? "" : " crm-stage-empty"}"><span class="crm-stage-chip-label">${escapeHtml(label)}</span><b>${count.toLocaleString()}</b></button>`;
    }).join("");
  }

  function renderCrmStageStrip() {
    const host = $(`[data-crm-stage-strip]`); if (!host) return;
    const view = crmActiveShared() ? null : crmActiveView();
    const { stages, counts } = crmStripStages(view, view && !view.flow ? String($(`[data-crm-flow]`)?.value || "") : "");
    host.hidden = !stages.length;
    if (!stages.length) { host.innerHTML = ""; return; }
    const selected = String($(`[data-crm-stage]`)?.value || "");
    host.classList.toggle("crm-stage-picked", Boolean(selected));
    host.setAttribute("aria-description", stages.map((stage) => `${crmStageLabel(stage)} ${counts.get(stage) || 0}`).join(", "));
    host.innerHTML = crmStageStripHtml(stages, counts, selected)
      + (view.flow ? `<button type="button" data-crm-stage-strip-menu aria-label="Pipeline options" title="Edit this pipeline's stages" class="crm-stage-strip-more">⋯</button>` : "");
  }

  function pickCrmStage(stage) {
    const select = $(`[data-crm-stage]`); if (!select) return;
    const next = stage === select.value ? "" : stage;
    select.value = [...select.options].some((option) => option.value === next) ? next : "";
    renderCrmContacts();
  }

  function crmEditStagesItem(flowId) {
    const manages = Boolean(state.crmView.canManageSettings);
    return {
      label: `Edit “${crmFlowName(flowId)}” stages…`, disabled: !manages,
      title: manages ? "" : "Only organization owners and admins can edit pipelines",
      run: () => openCrmPipelineSettings(flowId),
    };
  }

  function openCrmStageStripMenu(event) {
    const flowId = crmActiveView()?.flow || "";
    if (!flowId) return;
    openCrmMenu(event, [crmEditStagesItem(flowId)]);
  }

  function crmFlowName(flowId) {
    return crmFlows().find((flow) => flow.id === flowId)?.name || flowId;
  }

  function crmPipelineLists(flowId) {
    return flowId ? state.crmView.views.filter((view) => view.flow === flowId).map((view) => view.id) : [];
  }

  function crmContactInView(contact, view) {
    const memberships = contact.views || [];
    if (!view.flow) return memberships.includes(view.id);
    if (contact.flow !== view.flow) return false;
    const lists = state.crmView.views.filter((item) => item.flow === view.flow).map((item) => item.id);
    const mine = lists.find((id) => memberships.includes(id));
    return (mine || lists[0] || view.id) === view.id;
  }

  // Version the extension's dashboard bridge (content/extension/dashboard.js)
  // stamped on the page; empty when it is not installed or predates 1.7.0.
  function crmInstalledExtensionVersion() {
    return String(document.documentElement.dataset.forkmeshCrmExtension || "");
  }

  function crmVersionBehind(installed, current) {
    const have = installed.split(".").map(Number); const want = current.split(".").map(Number);
    for (let index = 0; index < Math.max(have.length, want.length); index += 1) {
      if ((have[index] || 0) !== (want[index] || 0)) return (have[index] || 0) < (want[index] || 0);
    }
    return false;
  }

  function renderCrmExtensionState() {
    const link = $(`[data-crm-extension-install]`); if (!link) return;
    const label = $(`[data-crm-extension-install-label]`);
    const installed = crmInstalledExtensionVersion();
    const behind = Boolean(installed) && crmVersionBehind(installed, CRM_EXTENSION_MINIMUM);
    link.href = CRM_EXTENSION_PAGE;
    // The extension controls sit in the "More" menu; a dot on the menu button
    // is what tells someone with an old copy that an update is waiting.
    const badge = $(`[data-crm-actions-badge]`);
    if (badge) badge.hidden = !behind;
    if (!installed) {
      label.textContent = "Get the browser extension";
      link.title = "Install the ForkMesh browser extension for Chrome";
    } else if (behind) {
      label.textContent = "Update the browser extension";
      link.title = `v${installed} is installed; the CRM panel needs v${CRM_EXTENSION_MINIMUM} or newer`;
    } else {
      label.textContent = `Extension v${installed} installed`;
      link.title = "Open the browser extension's page";
    }
  }

  async function openCrmExtensionsPage() {
    if (crmInstalledExtensionVersion()) {
      window.postMessage({ source: "forkmesh-crm-page", type: "open-extensions" }, location.origin);
      return;
    }
    // Chrome will not open chrome:// addresses from a website. Without the
    // extension to open it, hand the address over for the address bar.
    const copied = await navigator.clipboard?.writeText("chrome://extensions")
      .then(() => true, () => false);
    crmMessage("notice", copied
      ? "Copied chrome://extensions - paste it into the address bar. Chrome does not let websites open it."
      : "Type chrome://extensions into the address bar. Chrome does not let websites open it.");
  }

  function renderCrmMailLinks() {
    const zoho = $(`[data-crm-open-zoho]`); if (!zoho) return;
    const region = state.crmView.settings?.zoho?.region || "com";
    zoho.href = `https://${CRM_ZOHO_MAIL_HOSTS[region] || CRM_ZOHO_MAIL_HOSTS.com}/`;
  }

  const CRM_NEXT_WON_STAGES = new Set(["won", "closed-won", "customer", "renewal", "committed", "signed", "paid", "active", "awarded"]);
  const CRM_NEXT_ENDED_STAGES = new Set(["passed", "rejected", "closed", "inactive", "hired", "lost", "closed-lost", "churned", "declined", "unsubscribed", "do-not-contact"]);
  const CRM_NEXT_UNSENT_STATUSES = new Set(["draft", "sending", "failed", "reject"]);
  const CRM_NEXT_REPLY_DAYS = 60;
  const CRM_NEXT_FOLLOW_UP_DAYS = 14;

  function crmZohoComposeUrl(address) {
    const region = state.crmView.settings?.zoho?.region || "com";
    return `https://${CRM_ZOHO_MAIL_HOSTS[region] || CRM_ZOHO_MAIL_HOSTS.com}/zm/comp.do?ct=${encodeURIComponent(`mailto:${address}`)}`;
  }

  function crmMailProvider() {
    return state.crmView.settings?.mailProvider === "gmail" ? "gmail" : "zoho";
  }

  function crmMailComposeUrl(address) {
    const to = String(address || "").trim();
    if (crmMailProvider() === "gmail") return `https://mail.google.com/mail/?view=cm&fs=1&to=${encodeURIComponent(to)}`;
    return crmZohoComposeUrl(to);
  }

  function crmNextChannels(contact, thread = "") {
    const channels = [];
    const email = String(contact.email || "").trim();
    const gmail = crmMailProvider() === "gmail";
    if (/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email))
      channels.push({ kind: gmail ? "gmail" : "zoho", label: gmail ? "Email with Gmail" : "Email with Zoho", title: `Open a ${gmail ? "Gmail" : "Zoho Mail"} compose window to ${email}`, url: crmMailComposeUrl(email) });
    const mastodon = crmProfileUrl("mastodon", contact.mastodon);
    if (mastodon)
      channels.push({ kind: "mastodon", label: "Message on Mastodon", title: thread ? `Open the conversation with ${contact.mastodon}` : `Open ${contact.mastodon} on their instance`, url: thread || mastodon });
    const twitter = crmProfileUrl("twitter", contact.twitter);
    if (twitter)
      channels.push({ kind: "twitter", label: "Message on X", title: `Open ${contact.twitter} on X`, url: twitter });
    return channels;
  }

  function crmAgo(at, now) {
    const days = Math.floor((now - at) / 86400000);
    return days <= 0 ? "today" : days === 1 ? "yesterday" : `${days} days ago`;
  }

  function crmKeyBefore(a, b) {
    for (let index = 0; index < a.length; index += 1) {
      if (a[index] < b[index]) return true;
      if (a[index] > b[index]) return false;
    }
    return false;
  }

  function crmLeadMarks(contact) {
    let rank = 0; let rankField = ""; let score = 0; let scoreField = "";
    for (const [key, raw] of Object.entries(contact.fields || {})) {
      const field = String(key).toLowerCase(); const number = Number(String(raw ?? "").trim());
      if (!Number.isFinite(number) || number <= 0 || String(raw ?? "").trim() === "") continue;
      if ((field === "rank" || field.endsWith("_rank")) && (!rank || number < rank)) { rank = number; rankField = key; }
      if ((field === "score" || field.endsWith("_score")) && number > score) { score = number; scoreField = key; }
    }
    return { rank, rankField, score, scoreField, value: Math.max(0, Number(contact.value) || 0) };
  }

  function crmNextContact(mode, now = Date.now()) {
    const day = 86400000;
    const linked = new Map();
    for (const message of state.crmView.emails || []) {
      if (!linked.has(message.contactId)) linked.set(message.contactId, []);
      linked.get(message.contactId).push(message);
    }
    const ranked = [];
    for (const contact of state.crmView.contacts || []) {
      if (state.crmView.nextSkipped.has(contact.id) || !crmNextChannels(contact).length) continue;
      const last = Number(contact.lastContactedAt || 0);
      let heardAt = 0; let wroteAt = 0; let heardSubject = "";
      for (const message of linked.get(contact.id) || []) {
        const at = Number(message.sentAt || message.createdAt || 0);
        if (message.direction === "inbound") {
          if (at > heardAt) { heardAt = at; heardSubject = String(message.subject || ""); }
        } else if (!CRM_NEXT_UNSENT_STATUSES.has(String(message.status || "").toLowerCase())) {
          wroteAt = Math.max(wroteAt, at);
        }
      }
      const stage = String(contact.stage || "").toLowerCase();
      const step = String(contact.nextStep || "").trim();
      const waiting = heardAt >= now - CRM_NEXT_REPLY_DAYS * day && heardAt > wroteAt && heardAt >= last;
      const closed = CRM_NEXT_WON_STAGES.has(stage) || CRM_NEXT_ENDED_STAGES.has(stage);
      let key; let reason;
      if (mode === "reach") {
        if (waiting || closed || last > 0) continue;
        const marks = crmLeadMarks(contact);
        const created = Number(contact.createdAt || 0);
        const email = /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(String(contact.email || "").trim());
        const posted = crmMastodonPostedAt(contact);
        const qualifies = [
          posted > 0 && `posted on Mastodon ${crmAgo(posted, now)}`,
          step && `next step on the record: ${step}`,
          marks.value && `deal value ${marks.value.toLocaleString()}`,
          marks.rank && `ranked ${marks.rank} (${marks.rankField})`,
          marks.score && `score ${marks.score} (${marks.scoreField})`,
          email && "an email address on file",
        ].filter(Boolean);
        reason = `${created > 0 ? `Added ${crmAgo(created, now)} and never contacted` : "Never contacted"}. ${qualifies.length
          ? `Best qualified new lead: ${qualifies.join("; ")}.`
          : "Nothing on the record sets a new lead apart, so the longest on the CRM goes first."}`;
        key = [posted > 0 ? 0 : 1, -posted, step ? 0 : 1, -marks.value, marks.rank || Infinity, -marks.score, email ? 0 : 1, created, String(contact.id)];
      } else if (waiting) {
        reason = `They wrote to us ${crmAgo(heardAt, now)}${heardSubject ? ` (“${heardSubject}”)` : ""} and have had no answer yet, so the next move is ours.`;
        key = [0, step ? 0 : 1, -heardAt, String(contact.id)];
      } else if (!closed && last > 0 && now - last >= CRM_NEXT_FOLLOW_UP_DAYS * day) {
        reason = `Last contacted ${crmAgo(last, now)} and nothing has come back since; a follow-up is due after ${CRM_NEXT_FOLLOW_UP_DAYS} quiet days.`;
        key = [1, step ? 0 : 1, last, String(contact.id)];
      } else {
        continue;
      }
      if (mode !== "reach" && step) reason += ` Next step on the record: ${step}.`;
      ranked.push({ contact, reason, key });
    }
    ranked.sort((a, b) => (crmKeyBefore(a.key, b.key) ? -1 : crmKeyBefore(b.key, a.key) ? 1 : 0));
    const best = ranked[0];
    return { contact: best?.contact || null, reason: best?.reason || "", due: ranked.length,
      upcoming: ranked.slice(1, 1 + CRM_NEXT_UPCOMING).map((entry) => entry.contact), ranked: ranked.map((entry) => entry.contact) };
  }

  function crmMastodonPostedAt(contact) {
    const key = crmMastodonKey(contact?.mastodon).toLowerCase();
    return key ? Number(crmMastodonLatest.latest.get(key)?.postedAt) || 0 : 0;
  }

  function crmNextPostHtml(contact, compact = false) {
    const key = crmMastodonKey(contact?.mastodon).toLowerCase();
    if (!key) return "";
    const latest = crmMastodonLatest.latest.get(key);
    const running = crmMastodonLatest.pending.has(key) || crmMastodonLatest.queue.includes(key);
    const size = compact ? "text-xs" : "text-sm";
    if (!latest || latest.error || latest.postedAt <= 0) {
      const note = !latest ? (running ? `Reading @${key}…` : "")
        : latest.error ? `Mastodon post unavailable: ${latest.error}` : "No public Mastodon posts";
      return note ? `<p data-crm-next-post="${escapeHtml(key)}" class="mt-1 truncate text-xs text-muted-foreground" title="${escapeHtml(note)}">${escapeHtml(note)}</p>` : "";
    }
    const when = `<span class="whitespace-nowrap text-muted-foreground">${escapeHtml(crmAgo(latest.postedAt, Date.now()))} · </span>`;
    const excerpt = escapeHtml(compact ? crmPlainText(latest.excerpt, 90) : latest.excerpt);
    const title = escapeHtml(`${crmTimestamp(latest.postedAt)}\n${latest.detail}`);
    const text = latest.url
      ? `<a href="${escapeHtml(latest.url)}" target="_blank" rel="noopener noreferrer" class="crm-link">${excerpt}</a>`
      : `<span class="text-foreground">${excerpt}</span>`;
    return `<p data-crm-next-post="${escapeHtml(key)}" class="mt-1 ${compact ? "truncate" : ""} ${size} text-foreground" title="${title}">${when}${text}</p>`;
  }

  function crmQueueReachMastodon(ranked) {
    let queued = 0; const now = Date.now();
    for (const contact of ranked) {
      if (queued >= CRM_REACH_MASTODON_READS) break;
      const key = crmMastodonKey(contact.mastodon).toLowerCase();
      if (!key) continue;
      queued += 1;
      const cached = crmMastodonLatest.latest.get(key);
      if ((cached && now - cached.fetchedAt < CRM_MASTODON_FRESH_MS)
          || crmMastodonLatest.pending.has(key) || crmMastodonLatest.queue.includes(key)) continue;
      crmMastodonLatest.queue.push(key);
    }
    crmPumpMastodonLatest();
  }

  function crmScheduleNextRender() {
    if (crmMastodonLatest.renderTimer) return;
    crmMastodonLatest.renderTimer = setTimeout(() => {
      crmMastodonLatest.renderTimer = 0;
      renderCrmNextContact();
    }, CRM_NEXT_RENDER_DELAY_MS);
  }

  function crmNextThread(contactId) {
    return (state.crmView.emails || [])
      .filter((message) => message.contactId === contactId && message.provider === "mastodon" && crmHttpsUrl(message.providerUrl))
      .sort((a, b) => Number(b.sentAt || b.createdAt || 0) - Number(a.sentAt || a.createdAt || 0))[0]?.providerUrl || "";
  }

  const CRM_NEXT_MODES = [
    { mode: "reach", title: "Reach out", note: "Best new lead to contact", count: (n) => `${n.toLocaleString()} never contacted`,
      empty: "No new leads: everyone reachable has been contacted, or the rest are at a won or ended stage." },
    { mode: "follow", title: "Follow up", note: "Someone already in conversation", count: (n) => `${n.toLocaleString()} due · replies first, then the longest quiet`,
      empty: `Nobody to follow up: no unanswered replies and nobody quiet for ${CRM_NEXT_FOLLOW_UP_DAYS}+ days.` },
  ];

  function crmNextCardHtml({ mode, title, note, count, empty }) {
    const next = crmNextContact(mode);
    const head = `<div class="mb-2 flex flex-wrap items-baseline justify-between gap-2"><h2 class="text-sm font-semibold text-foreground">${title} <span class="font-normal text-muted-foreground">· ${note}</span></h2><span data-crm-next-due class="text-xs text-muted-foreground">${next.due ? escapeHtml(count(next.due)) : ""}</span></div>`;
    const frame = (inner) => `<section data-crm-next-mode="${mode}" aria-label="${title}" class="min-w-0 rounded-lg border border-border bg-card p-3">${head}${inner}</section>`;
    if (!next.contact) {
      const skipped = state.crmView.nextSkipped.size;
      return frame(`<div class="flex flex-wrap items-center justify-between gap-2 text-xs text-muted-foreground"><span>${skipped
        ? "Everyone here has been skipped for now."
        : escapeHtml(empty)}</span>${skipped
        ? `<button data-crm-next-unskip type="button" class="rounded-md border border-border px-3 py-1.5 text-xs font-semibold text-foreground hover:bg-secondary">Show skipped again</button>` : ""}</div>`);
    }
    const contact = next.contact; const id = escapeHtml(contact.id);
    const about = [contact.title, contact.company, contact.stage && titleCaseSlug(contact.stage)].filter(Boolean).map(escapeHtml).join(" · ");
    const channels = crmNextChannels(contact, contact.mastodon ? crmNextThread(contact.id) : "");
    return frame(`<div class="flex min-w-0 items-start gap-3"><button data-crm-contact-detail="${id}" type="button" title="Open the detail card" aria-label="Open ${escapeHtml(crmContactLabel(contact))}" class="crm-photo-button">${crmPicture(contact, "lg")}</button><div class="min-w-0">
        <button data-crm-contact-detail="${id}" type="button" class="text-left text-base font-semibold text-foreground hover:underline">${escapeHtml(crmContactLabel(contact))}</button>
        ${about ? `<p class="text-xs text-muted-foreground">${about}</p>` : ""}
        <div class="mt-1">${crmLastContacted(contact)}</div>
        ${mode === "reach" ? crmNextPostHtml(contact) : ""}
        <p data-crm-next-reason class="mt-2 text-sm text-foreground"><b>Why them:</b> ${escapeHtml(next.reason)}</p>
      </div></div>
      <div class="mt-3 flex flex-wrap items-center gap-2">
        ${channels.map((channel, index) => `<a data-crm-next-channel="${channel.kind}" href="${escapeHtml(channel.url)}" target="_blank" rel="noopener noreferrer" title="${escapeHtml(channel.title)}" class="${index === 0 ? "bg-primary text-primary-foreground" : "border border-border text-foreground hover:bg-secondary"} whitespace-nowrap rounded-md px-3 py-2 text-xs font-semibold">${escapeHtml(channel.label)} &#8599;</a>`).join("")}
        <button data-crm-next-ai="${id}" data-crm-next-ai-mode="${mode}" type="button" title="Gather everything about them and us into a prompt for ChatGPT or Claude to write the message" class="whitespace-nowrap rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Write with AI</button>
        ${crmEmailAddress(contact) ? `<button data-crm-next-auto="${id}" data-crm-next-auto-mode="${mode}" type="button" title="${escapeHtml(CRM_AUTO_EMAIL_TITLE)}" class="whitespace-nowrap rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Send AI email</button>` : ""}
        <button data-crm-next-contacted="${id}" type="button" title="Stamp Last contacted now and show the next person" class="whitespace-nowrap rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Mark contacted</button>
        <button data-crm-next-skip="${id}" type="button" title="Show the next person; nothing is saved" class="whitespace-nowrap rounded-md border border-border px-3 py-2 text-xs text-muted-foreground hover:bg-secondary">Skip</button>
      </div>${mode === "reach" ? crmNextUpcomingHtml(next.upcoming) : ""}`);
  }

  function crmNextUpcomingHtml(upcoming) {
    if (!upcoming?.length) return "";
    return `<div class="mt-3 border-t border-border pt-2"><h3 class="text-xs font-semibold text-muted-foreground">Next in line · newest Mastodon post first</h3><ul data-crm-next-upcoming class="mt-1 divide-y divide-border">${upcoming.map((contact) => {
      const id = escapeHtml(contact.id);
      const about = [contact.title, contact.company].filter(Boolean).map(escapeHtml).join(" · ");
      return `<li data-crm-next-upcoming-id="${id}" class="min-w-0 py-1.5"><div class="flex min-w-0 items-baseline gap-2"><button data-crm-contact-detail="${id}" type="button" class="truncate text-left text-sm font-medium text-foreground hover:underline">${escapeHtml(crmContactLabel(contact))}</button>${about ? `<span class="truncate text-xs text-muted-foreground">${about}</span>` : ""}</div>${crmNextPostHtml(contact, true)}</li>`;
    }).join("")}</ul></div>`;
  }

  function renderCrmNextContact() {
    const card = $(`[data-crm-next]`); if (!card) return;
    if (crmActiveShared() || !state.crmView.contacts.length) { card.hidden = true; return; }
    card.hidden = false;
    if (crmNextCollapsed()) {
      const who = (mode) => crmNextContact(mode).contact;
      const line = CRM_NEXT_MODES.map(({ mode, title }) => {
        const contact = who(mode);
        return `<span class="min-w-0 truncate"><b class="text-foreground">${escapeHtml(title)}</b> <span class="text-muted-foreground">${contact ? escapeHtml(crmContactLabel(contact)) : "nobody"}</span></span>`;
      }).join(`<span class="text-muted-foreground">·</span>`);
      card.innerHTML = `<div class="flex min-w-0 items-center gap-2 rounded-lg border border-border bg-card px-3 py-1.5 text-xs md:col-span-2">${line}<button data-crm-next-toggle type="button" aria-expanded="false" class="ml-auto shrink-0 rounded px-1.5 text-muted-foreground hover:bg-secondary hover:text-foreground">Show</button></div>`;
      return;
    }
    card.innerHTML = CRM_NEXT_MODES.map(crmNextCardHtml).join("")
      + `<button data-crm-next-toggle type="button" aria-expanded="true" class="justify-self-end rounded px-1.5 text-xs text-muted-foreground hover:bg-secondary hover:text-foreground md:col-span-2" style="margin-top:-.25rem">Hide suggestions</button>`;
    crmWatchPictures();
    crmQueueReachMastodon(crmNextContact("reach").ranked);
  }

  const CRM_NEXT_COLLAPSED_KEY = "forkmesh-crm-next-collapsed";

  function crmNextCollapsed() {
    try { return localStorage.getItem(CRM_NEXT_COLLAPSED_KEY) === "1"; } catch { return Boolean(state.crmView.nextCollapsed); }
  }

  function toggleCrmNextCollapsed() {
    const collapsed = !crmNextCollapsed();
    state.crmView.nextCollapsed = collapsed;
    try { localStorage.setItem(CRM_NEXT_COLLAPSED_KEY, collapsed ? "1" : "0"); } catch {}
    renderCrmNextContact();
  }

  async function markCrmNextContacted(contactId) {
    try {
      await crmRequest(crmOrganizationPath(`/contacts/${contactId}`), "PATCH", { lastContactedAt: Date.now() });
      crmMessage("notice", "Marked contacted.");
    } catch (error) { crmMessage("error", `Could not mark contacted: ${error.message}`); return; }
    await loadCrm();
  }

  function handleCrmNextClick(event) {
    if (event.target.closest("[data-crm-next-toggle]")) { toggleCrmNextCollapsed(); return; }
    const skip = event.target.closest("[data-crm-next-skip]");
    if (skip) { state.crmView.nextSkipped.add(skip.dataset.crmNextSkip); renderCrmNextContact(); return; }
    if (event.target.closest("[data-crm-next-unskip]")) { state.crmView.nextSkipped.clear(); renderCrmNextContact(); return; }
    const contacted = event.target.closest("[data-crm-next-contacted]");
    if (contacted) { contacted.disabled = true; void markCrmNextContacted(contacted.dataset.crmNextContacted); return; }
    const auto = event.target.closest("[data-crm-next-auto]");
    if (auto) {
      const mode = auto.dataset.crmNextAutoMode === "reach" ? "reach" : "follow";
      const next = crmNextContact(mode);
      void sendCrmAutoEmail(auto.dataset.crmNextAuto, mode, next.contact?.id === auto.dataset.crmNextAuto ? next.reason : "", auto);
      return;
    }
    const ai = event.target.closest("[data-crm-next-ai]");
    if (ai) {
      const mode = ai.dataset.crmNextAiMode === "reach" ? "reach" : "follow";
      const next = crmNextContact(mode);
      openCrmAi(ai.dataset.crmNextAi, mode, next.contact?.id === ai.dataset.crmNextAi ? next.reason : "");
      return;
    }
    const detail = event.target.closest("[data-crm-contact-detail]");
    if (detail) openCrmContactDetail(detail.dataset.crmContactDetail);
  }

  const CRM_AUTO_EMAIL_TITLE = "A model writes the email from their record, our past emails and what got opened or clicked, then sends it at once from the outreach sender. It carries the whole investor deck in the email (investor pipelines) or your referral link, and you get a ping when they open it or click.";
  const CRM_AUTO_EMAIL_ERRORS = {
    outreach_forbidden: "Only owners, admins and the outreach team can send automated emails.",
    recently_emailed: "They already got an email from the CRM in the last three days.",
    contact_closed: "They are at an ended stage, so nothing was sent.",
    contact_has_no_email: "They have no email address on record.",
    daily_limit_reached: "You reached today's outreach limit.",
    ai_unavailable: "This host has no AI model for writing emails. Use Write with AI, or the desktop app's agent.",
    draft_unusable: "The model's draft had placeholders or links we could not check, so nothing was sent. Try again.",
    rate_limited: "The AI model is busy. Try again in a minute.",
    send_failed: "The email provider refused the send.",
    zoho_send_not_allowed: "Zoho Mail did not let ForkMesh send from your mailbox: its sign-in only allows reading, and syncing does not change that. Use Reconnect Zoho Mail at the top of the CRM, or under My Zoho Mail, and allow sending.",
  };

  function crmEmailAddress(contact) {
    return /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(String(contact?.email || "").trim());
  }

  async function sendCrmAutoEmail(contactId, mode, reason, button) {
    const contact = crmContactById(contactId);
    if (!contact || !crmEmailAddress(contact)) return;
    const investor = crmFlow(contact.flow)?.icon === "investors" || ["investors", "investment"].includes(contact.flow);
    const kind = investor ? "the whole investor deck right in the email" : "your referral link";
    const activeZoho = crmZohoActive();
    const from = activeZoho.zoho?.connected ? `${activeZoho.mine ? "your own" : "the organization's"} Zoho Mail mailbox, so it lands in its Sent folder` : "the outreach sender";
    if (!window.confirm(`Write and send an email to ${crmContactLabel(contact)} <${contact.email}> now?\n\nA model writes it from their record and what our past emails got opened or clicked for, with ${kind}. It goes out straight away from ${from}.`)) return;
    if (button) { button.disabled = true; button.textContent = "Sending…"; }
    crmMessage("notice", `Writing an email to ${crmContactLabel(contact)}…`);
    try {
      const sent = await crmRequest(crmOrganizationPath("/outreach/auto"), "POST", {
        contactId, mode, reason, list: crmActiveView()?.name || "",
      });
      const learned = sent.learning?.sent ? ` Learned from ${sent.learning.sent} past emails (${sent.learning.openRate}% opened, ${sent.learning.clickRate}% clicked).` : "";
      const via = sent.via === "zoho" ? ` from ${sent.fromEmail || "your mailbox"} through Zoho Mail` : "";
      crmMessage("notice", `Sent “${sent.subject}” to ${crmContactLabel(contact)}${via}. You get a ping when they open it or click.${learned}`);
    } catch (error) {
      const known = CRM_AUTO_EMAIL_ERRORS[error.payload?.error || ""];
      crmMessage("error", `Nothing was sent: ${known || error.message}`);
      if (button) { button.disabled = false; button.textContent = "Send AI email"; }
      if (error.payload?.error === "zoho_send_not_allowed") await loadCrm();
      return;
    }
    await loadCrm();
  }

  const CRM_AI_TARGETS = {
    chatgpt: { label: "ChatGPT", url: "https://chatgpt.com/" },
    claude: { label: "Claude", url: "https://claude.ai/new" },
  };
  const CRM_AI_FORMATS = [
    { id: "email", label: "Email" },
    { id: "message", label: "Direct message" },
    { id: "toot", label: "Toot" },
    { id: "application", label: "Grant application" },
  ];
  const CRM_AI_URL_LIMIT = 14000;
  const CRM_AI_MESSAGE_LIMIT = 6;
  const CRM_AI_MESSAGE_CHARS = 900;
  const CRM_AI_NOTES_CHARS = 2500;
  const CRM_AI_FIELD_LIMIT = 40;
  const CRM_AI_BRIEF_LIMIT = 8000;
  const CRM_FORKMESH_BRIEF = [
    "ForkMesh (https://forkmesh.com) lets developers collaborate on, mirror and preserve source-code repositories across independent hosts, without relying on any single hosting provider. It is open source.",
    "- Git stays the source of truth. Repository bytes live on developer-operated hosts and mirrors; clones are served from a live, signed, integrity-checked mirror, so a repository stays reachable when its source host goes offline.",
    "- Collaboration travels with the code: issues, discussions, pull requests, reviews, projects and releases are signed records stored in the repository, not only in a hosted database.",
    "- Stock Git works with no account: git clone https://forkmesh.com/<owner>/<repo>. ForkMesh's own source is https://forkmesh.com/forkmesh/forkmesh.",
    "- The desktop app (Linux, macOS, Windows) is a full node: it hosts and mirrors repositories and runs Codex or Claude Code agents in isolated worktrees whose work becomes pull requests. iPhone and Android apps are access clients.",
    "- The protocol, self-hosting, public read and clone, and portable Git data are free; only bounded hosted services are paid.",
    "- The public proof uses ordinary developer actions: collaborate, add a mirror, take the source host offline, and clone again.",
    "Links:",
    "- Website: https://forkmesh.com",
    "- Web app: https://app.forkmesh.com",
    "- 3D ForkMesh World: https://world.forkmesh.com",
    "- Docs: https://forkmesh.com/docs/",
    "- Blog: https://forkmesh.com/blog",
    "- Changelog: https://forkmesh.com/changelog",
    "- Press kit: https://forkmesh.com/press",
    "- Status: https://forkmesh.com/status",
    "- Install the desktop node on Linux or macOS: curl -fsSL https://forkmesh.com/install.sh | bash",
    "Recent updates: read the blog and the changelog above for what shipped lately.",
  ].join("\n");

  function crmAiBrief() {
    const saved = String(state.crmView.settings?.messageBrief || "").trim();
    if (saved) return saved;
    return state.crmView.organization.toLowerCase() === "forkmesh" ? CRM_FORKMESH_BRIEF : "";
  }

  function crmAiDefaultFormat(contact) {
    if (crmFlow(contact.flow)?.icon === "grants") return "application";
    if (/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(String(contact.email || "").trim())) return "email";
    return crmMastodonKey(contact.mastodon) ? "toot" : "message";
  }

  function crmAiMode(contact, messages) {
    return Number(contact.lastContactedAt || 0) > 0 || messages.some((message) =>
      message.direction === "inbound" || !CRM_NEXT_UNSENT_STATUSES.has(String(message.status || "").toLowerCase()))
      ? "follow" : "reach";
  }

  function crmAiMessageText(text) {
    const lines = [];
    for (const line of String(text || "").replace(/\r\n?/g, "\n").split("\n")) {
      if (/^On .{4,200}wrote:\s*$/.test(line.trim()) || /^-{2,}\s*Original Message\s*-{2,}$/i.test(line.trim())) break;
      if (!line.trimStart().startsWith(">")) lines.push(line);
    }
    const flat = lines.join("\n").replace(/\n{3,}/g, "\n\n").trim();
    return flat.length > CRM_AI_MESSAGE_CHARS ? `${flat.slice(0, CRM_AI_MESSAGE_CHARS - 1)}…` : flat;
  }

  const CRM_AI_ANGLES = [
    { words: ["grant"],
      angle: (group) => `They are on our "${group}" list as a grant funder, so write to their published priorities and evaluation criteria: problem, approach, impact, team, budget and milestones. Never invent traction, numbers, partners or names.` },
    { words: ["journalist", "press", "media", "reporter", "writer", "editor", "podcast", "newsletter", "blog"],
      angle: (group) => `They are on our "${group}" list as a journalist, so pitch a story: why it matters now and what is new, and offer an interview or a demo. No sales talk.` },
    { words: ["investor", "angel", "vc", "fund", "capital"],
      angle: (group) => `They are on our "${group}" list as an investor, so lead with the opportunity and ask for a short call; never invent traction, numbers or names.` },
    { words: ["partner", "integration", "alliance"],
      angle: (group) => `They are on our "${group}" list as a potential partner, so show how we fit what they build and suggest exploring working together.` },
    { words: ["customer", "lead", "user", "developer", "maintainer", "community", "open source"],
      angle: (group) => `They are on our "${group}" list as a potential user, so give one concrete way we help their projects and invite them to try it.` },
  ];

  function crmAiAngle(group, flowName) {
    const name = String(group || flowName || "").trim();
    if (!name) return "";
    const audience = `${group || ""} ${flowName || ""}`.toLowerCase();
    const match = CRM_AI_ANGLES.find((item) => item.words.some((word) => audience.includes(word)));
    return match ? match.angle(name) : `They are on our "${name}" list, so pitch it the way that suits that group.`;
  }

  const CRM_AI_MESSAGE_RULES = [
    "- Make it personal. Open with something specific to them, taken from their recent posts, work or links below, never a compliment that would fit anyone.",
    "- Say plainly who we are, why we are writing to them in particular, and why now.",
    "- End with one clear, low-effort ask (a reply, a short call, or trying one thing).",
    "- Plain, warm words, short paragraphs, no hype, no emoji, no [placeholders]. Use only facts given here or found on the linked pages; leave out anything you cannot back up.",
    "- If you can browse, open their profiles and posts and our site, blog and changelog below first, and use anything newer than what is written here.",
    "- Reply with the finished message, ready to paste, then two alternative opening lines.",
  ];
  const CRM_AI_APPLICATION_RULES = [
    "- If you can browse, open their program page and application form (links below) first, and use their current questions, limits, deadline and criteria; say so if anything on our record is out of date.",
    "- Answer each question under its own heading, with its word or character count after it.",
    "- Fit the request to their range and to what they fund; break the budget into milestones with dates and deliverables.",
    "- Plain, specific words, no hype, no emoji. Use only facts given here or found on the linked pages; leave out anything you cannot back up.",
    "- Where the form needs something only we can supply (legal entity, registration numbers, bank details, CVs, letters of support), never invent it: list it at the end instead.",
    "- Reply with the finished application, ready to paste, then a checklist of every document and fact we still have to supply, then three ways to make it stronger.",
  ];

  function crmAiPrompt(input) {
    const { contact, format, mode } = input;
    const name = crmContactLabel(contact);
    const organization = input.organization || "our organization";
    const sender = input.sender || `the ${organization} team`;
    const day = (at) => crmDay(at) || "an unknown date";
    const mastodon = crmMastodonKey(contact.mastodon);
    const ask = {
      email: `an email to ${name}. Start with a "Subject:" line, then the body, signed by ${sender}. Keep the body under 150 words.`,
      message: `a short direct message to ${name} for LinkedIn, X or Bluesky: under 80 words, no subject line, signed by ${sender}.`,
      application: `a complete draft of our grant application to ${name}. Follow their form: answer every question they ask, in their order and under their headings, within their word or character limits, with a budget in their currency that fits the amount they award.`,
      toot: `a Mastodon post (toot) to ${name}${mastodon ? ` that mentions @${mastodon} at the start` : ""}, at most 500 characters including the mention and any link, with no more than two hashtags.${input.thread ? ` Write it as a reply in our existing thread: ${input.thread}` : ""}`,
    }[format] || "";
    const goal = format === "application"
      ? `We are applying to ${name} for funding: make the case that funding ${organization} advances their mission, and propose one concrete, fundable piece of ${organization} work with milestones and deliverables.`
      : mode === "reach"
      ? `This is our first contact with ${name}: introduce ${organization} in a sentence and give them one concrete reason it matters to them in particular.`
      : `We are already in touch with ${name}: pick up where the conversation below left off, answer anything they asked, and do not repeat the first pitch.`;
    const angle = crmAiAngle(input.group, input.flowName);
    const facts = [
      ["Title", contact.title], ["Company", contact.company], ["Location", contact.location],
      ["Pipeline", [input.flowName, contact.stage && titleCaseSlug(contact.stage)].filter(Boolean).join(", stage ")],
      ["Deal value", Number(contact.value) > 0 ? Number(contact.value).toLocaleString("en-US") : ""],
      ["Next step on our record", contact.nextStep], ["Owner", contact.owner], ["Source", contact.source],
      ["Tags", (contact.tags || []).join(", ")],
      ["Last contacted", contact.lastContactedAt ? day(contact.lastContactedAt) : "never"],
      ["Added to our CRM", contact.createdAt ? `${day(contact.createdAt)}${contact.createdBy ? ` by ${contact.createdBy}` : ""}` : ""],
      ["ForkMesh account", crmAccountProof(contact)],
      ["Email", contact.email],
      ...CRM_PROFILE_FIELDS.map((field) => [crmColumnLabel(field), crmProfileUrl(field, contact[field]) || contact[field]]),
      ["Phone", contact.phone],
      ["Their investor deck link", input.deckUrl],
      ...Object.entries(contact.fields || {}).slice(0, CRM_AI_FIELD_LIMIT)
        .map(([key, value]) => [crmColumnLabel(key), crmPlainText(value, 200)]),
    ].filter(([, value]) => String(value ?? "").trim()).map(([label, value]) => `- ${label}: ${String(value).trim()}`);
    const notes = String(contact.notes || "").trim();
    const posts = (input.updates || []).map((update) => {
      const head = `${update.label} ${update.handle}${update.url ? ` (${update.url})` : ""}`;
      if (update.error) return `${head}: could not be read (${update.error}); open the profile yourself.`;
      if (!(update.posts || []).length) return `${head}: no public posts.`;
      return [`${head}:`, ...update.posts.map((post) => `- ${day(post.at)}: ${post.text}${post.url ? ` (${post.url})` : ""}`)].join("\n");
    });
    const messages = [...(input.messages || [])].sort((a, b) => (a.at || 0) - (b.at || 0)).map((message) => {
      const who = message.direction === "inbound" ? "They wrote" : `We wrote${message.sender ? ` (${message.sender})` : ""}`;
      const unsent = message.direction !== "inbound" && CRM_NEXT_UNSENT_STATUSES.has(String(message.status || "").toLowerCase())
        ? ` [${String(message.status).toLowerCase()}, not delivered]` : "";
      const head = `- ${day(message.at)} · ${who} via ${message.via}${unsent}${message.subject ? ` · Subject: ${message.subject}` : ""}${message.url ? ` · ${message.url}` : ""}`;
      const text = crmAiMessageText(message.text);
      return text ? `${head}\n${text.split("\n").map((line) => `  > ${line}`).join("\n")}` : head;
    });
    return [
      `You are helping ${sender} at ${organization} write ${ask}`,
      "",
      goal,
      ...(angle ? [angle] : []),
      "",
      "How to write it:",
      ...(format === "application" ? CRM_AI_APPLICATION_RULES : CRM_AI_MESSAGE_RULES),
      "",
      `## About us: ${organization}`,
      String(input.brief || "").trim() || `(No description saved yet. Look ${organization} up before writing.)`,
      "",
      `## About ${name}`,
      ...(facts.length ? facts : ["- Nothing beyond the name is on record."]),
      ...(notes ? ["", "Our notes on them:", notes.length > CRM_AI_NOTES_CHARS ? `…${notes.slice(-CRM_AI_NOTES_CHARS)}` : notes] : []),
      "",
      "## Their recent public posts",
      ...(input.updates == null ? ["Not read yet: open their profiles linked above."]
        : posts.length ? posts : ["No Mastodon, Bluesky or GitHub account on record; look at the links above."]),
      "",
      "## Our conversation so far (oldest first)",
      ...(messages.length ? messages : ["Nothing on record yet: this is the first message."]),
      ...(input.reason ? ["", "## Why now", input.reason] : []),
    ].join("\n");
  }

  function crmAiTargetUrl(target, prompt) {
    const base = CRM_AI_TARGETS[target]?.url || CRM_AI_TARGETS.chatgpt.url;
    const url = `${base}?q=${encodeURIComponent(prompt)}`;
    return url.length <= CRM_AI_URL_LIMIT ? { url, filled: true } : { url: base, filled: false };
  }

  function crmAiMessageStubs(contact) {
    return crmLinkedEmails(contact.id)
      .map((email) => ({
        id: email.id, at: Number(email.sentAt || email.createdAt || 0), direction: email.direction,
        status: email.status, subject: email.subject, sender: email.sender,
        via: crmProviderLabel(email), url: crmHttpsUrl(email.providerUrl), text: "",
      }))
      .sort((a, b) => b.at - a.at).slice(0, CRM_AI_MESSAGE_LIMIT);
  }

  async function crmAiMessages(stubs) {
    return Promise.all(stubs.map(async (stub) => {
      try {
        const payload = await crmRequest(crmOrganizationPath(`/emails/${encodeURIComponent(stub.id)}`));
        return { ...stub, text: String(payload.email?.text || "").trim() || crmMastodonText(payload.email?.html) };
      } catch { return stub; }
    }));
  }

  async function crmAiUpdates(contact) {
    return Promise.all(crmLiveAccounts(contact).map(async (account) => {
      const update = { label: account.label, handle: account.handle, url: account.url, posts: [] };
      try {
        update.posts = (await CRM_UPDATE_READERS[account.service](account)).posts || [];
      } catch (error) {
        update.error = error.name === "TimeoutError" ? "it did not answer in time" : error.message;
      }
      return update;
    }));
  }

  function renderCrmAiPrompt(force = false) {
    const ai = state.crmView.ai; const contact = crmContactById(ai?.contactId);
    const box = $(`[data-crm-ai-prompt]`); if (!ai || !contact || !box) return;
    if (ai.edited && !force) return;
    ai.edited = false;
    box.value = crmAiPrompt({
      contact, format: ai.format, mode: ai.mode, reason: ai.reason,
      brief: $(`[data-crm-ai-brief]`)?.value ?? crmAiBrief(),
      organization: state.crmView.organization, sender: String(state.session?.name || ""),
      flowName: crmFlow(contact.flow)?.name || "", group: crmActiveView()?.name || "",
      messages: ai.messages, updates: ai.updates,
      deckUrl: crmDeckFor(contact.id)?.url || "", thread: crmNextThread(contact.id), now: Date.now(),
    });
  }

  function renderCrmAiStatus(text) {
    const status = $(`[data-crm-ai-status]`); if (status) status.textContent = text;
  }

  function openCrmAi(contactId, mode = "", reason = "") {
    const contact = crmContactById(contactId);
    const dialog = $(`[data-crm-ai-dialog]`);
    if (!contact || !dialog) return;
    const token = {};
    const stubs = crmAiMessageStubs(contact);
    const accounts = crmLiveAccounts(contact).length; const linked = stubs.length;
    state.crmView.ai = {
      contactId: contact.id, token, reason, edited: false, messages: stubs, updates: accounts ? null : [],
      mode: mode || crmAiMode(contact, stubs), format: crmAiDefaultFormat(contact),
    };
    $(`[data-crm-ai-title]`).textContent = `Write to ${crmContactLabel(contact)} with AI`;
    $(`[data-crm-ai-formats]`).innerHTML = CRM_AI_FORMATS.map((format) => `<label class="inline-flex items-center gap-1 rounded-md border border-border px-2 py-1 text-xs text-foreground"><input type="radio" name="crm-ai-format" value="${format.id}"${format.id === state.crmView.ai.format ? " checked" : ""} />${escapeHtml(format.label)}</label>`).join("");
    const brief = $(`[data-crm-ai-brief]`);
    brief.value = crmAiBrief();
    brief.placeholder = `What ${state.crmView.organization} does, who it is for, links, recent news`;
    $(`[data-crm-ai-brief-save]`).hidden = !crmCanManage();
    renderCrmAiPrompt(true);
    renderCrmAiStatus(accounts || linked
      ? `Reading ${[linked && `${linked} past message${linked === 1 ? "" : "s"}`, accounts && `${accounts} public account${accounts === 1 ? "" : "s"}`].filter(Boolean).join(" and ")}…`
      : "Nothing to read: the prompt holds the record, our notes and the brief.");
    if (!dialog.open) dialog.showModal();
    if (!accounts && !linked) return;
    void Promise.all([crmAiMessages(stubs), crmAiUpdates(contact)]).then(([messages, updates]) => {
      const ai = state.crmView.ai;
      if (ai?.token !== token) return;
      ai.messages = messages; ai.updates = updates;
      const posts = updates.reduce((total, update) => total + (update.posts || []).length, 0);
      renderCrmAiStatus(`${ai.edited ? "Read, but your edits to the prompt were kept" : "Ready"}: ${messages.length} past message${messages.length === 1 ? "" : "s"} and ${posts} recent post${posts === 1 ? "" : "s"} are in the prompt.`);
      renderCrmAiPrompt();
    });
  }

  function sendCrmAi(target) {
    const prompt = String($(`[data-crm-ai-prompt]`)?.value || "");
    const label = CRM_AI_TARGETS[target]?.label || "ChatGPT";
    const { url, filled } = crmAiTargetUrl(target, prompt);
    window.open(url, "_blank", "noopener,noreferrer");
    const copied = navigator.clipboard?.writeText(prompt) || Promise.reject(new Error("no clipboard"));
    void copied.then(
      () => renderCrmAiStatus(filled
        ? `Opened ${label} with the prompt filled in. It is on your clipboard too.`
        : `The prompt is too long for a link, so it is on your clipboard: paste it into ${label}.`),
      () => renderCrmAiStatus(filled
        ? `Opened ${label} with the prompt filled in.`
        : `The prompt is too long for a link and could not be copied: select it here, copy it and paste it into ${label}.`));
  }

  async function saveCrmAiBrief(button) {
    const brief = String($(`[data-crm-ai-brief]`)?.value || "").trim().slice(0, CRM_AI_BRIEF_LIMIT);
    button.disabled = true;
    try {
      const payload = await crmRequest(crmOrganizationPath("/settings"), "PUT", { messageBrief: brief });
      state.crmView.settings = { ...state.crmView.settings, ...(payload.settings || { messageBrief: brief }) };
      renderCrmAiStatus(`Saved as ${state.crmView.organization}'s brief for every member.`);
    } catch (error) { renderCrmAiStatus(`Could not save the brief: ${error.message}`); }
    button.disabled = false;
  }

  function handleCrmAiClick(event) {
    const send = event.target.closest("[data-crm-ai-open]");
    if (send) { sendCrmAi(send.dataset.crmAiOpen); return; }
    if (event.target.closest("[data-crm-ai-copy]")) {
      const prompt = String($(`[data-crm-ai-prompt]`)?.value || "");
      const copied = navigator.clipboard?.writeText(prompt) || Promise.reject(new Error("no clipboard"));
      void copied.then(() => renderCrmAiStatus("Prompt copied."),
        () => renderCrmAiStatus("Could not copy the prompt: select it here and copy it."));
      return;
    }
    const save = event.target.closest("[data-crm-ai-brief-save]");
    if (save) void saveCrmAiBrief(save);
  }

  function handleCrmAiChange(event) {
    const ai = state.crmView.ai; if (!ai) return;
    if (event.target.name === "crm-ai-format") { ai.format = event.target.value; renderCrmAiPrompt(true); return; }
    if (event.target.matches("[data-crm-ai-brief]")) renderCrmAiPrompt(true);
  }

  function crmActiveShared() {
    const id = state.crmView.activeShared;
    if (!id) return null;
    const cached = state.crmView.sharedCache[id];
    return cached?.view || state.crmView.sharedViews.find((view) => view.id === id) || null;
  }

  function crmCustomFieldKeys() {
    const keys = [];
    for (const contact of state.crmView.contacts) {
      for (const key of Object.keys(contact.fields || {})) {
        if (!keys.includes(key)) keys.push(key);
      }
    }
    for (const view of state.crmView.views) {
      for (const column of view.columns || []) {
        if (column.custom && !keys.includes(column.field)) keys.push(column.field);
      }
    }
    for (const key of crmSessionColumnKeys()) {
      if (!keys.includes(key)) keys.push(key);
    }
    return keys;
  }

  function crmColumnLabel(field) {
    const base = [...CRM_BASE_COLUMNS, ...CRM_COMPUTED_COLUMNS].find((column) => column.field === field);
    if (base) return base.label;
    if (crmSessionColumns.organization === state.crmView.organization && crmSessionColumns.labels.has(field)) {
      return crmSessionColumns.labels.get(field);
    }
    for (const view of state.crmView.views) {
      const column = (view.columns || []).find((item) => item.field === field);
      if (column?.label) return column.label;
    }
    return titleCaseSlug(field.replaceAll("_", "-"));
  }

  function crmStoredColumns() {
    const view = crmActiveShared() || crmActiveView();
    if (view?.virtual) return state.crmView.virtualColumns[view.id] || [];
    return view && Array.isArray(view.columns) ? view.columns : [];
  }

  // Stored layouts only decide order and visibility. Any column the layout has
  // never seen - a new CRM release, or a custom field another sheet imported -
  // is appended so it can still be switched on from the column menu.
  function crmListColumns() {
    const stored = crmStoredColumns().filter((column) => column && column.field);
    const known = new Set(stored.map((column) => column.field));
    // A layout with nothing visible - none saved yet, or a new sheet the
    // worker seeds with every column hidden - starts from the default set.
    const fresh = !stored.some((column) => !column.hidden);
    const extra = crmActiveShared() ? [] : [
      ...CRM_BASE_COLUMNS.filter((column) => !known.has(column.field))
        .map((column) => ({ field: column.field, hidden: true, extra: true })),
      ...crmCustomFieldKeys().filter((field) => !known.has(field))
        .map((field) => ({ field, hidden: true, extra: true })),
      ...CRM_COMPUTED_COLUMNS.filter((column) => !known.has(column.field))
        .map((column) => ({ field: column.field, hidden: true, extra: true })),
    ];
    const removed = new Set(state.crmView.settings?.removedColumns || []);
    return [...stored, ...extra].filter((column) => !removed.has(column.field)).map((column) => {
      const base = [...CRM_BASE_COLUMNS, ...CRM_COMPUTED_COLUMNS].find((item) => item.field === column.field);
      return {
        ...(base || { custom: true, width: "12rem" }),
        field: column.field,
        label: column.label || crmColumnLabel(column.field),
        custom: !base,
        extra: Boolean(column.extra),
        hidden: fresh ? !CRM_DEFAULT_COLUMNS.has(column.field) : Boolean(column.hidden),
      };
    });
  }

  function crmTableViewKey() {
    return `forkmesh-crm-table-view:${state.crmView.organization}`;
  }

  function crmTableViews() {
    return Array.isArray(state.crmView.settings?.tableViews) ? state.crmView.settings.tableViews : [];
  }

  let crmTableViewChoice = "";
  let crmTableViewDraft = null;

  function crmTableViewId() {
    let id = crmTableViewChoice;
    try { id = localStorage.getItem(crmTableViewKey()) || id; }
    catch { /* a browser with no storage keeps the choice for this page */ }
    return id === "all" || crmTableViews().some((view) => view.id === id) ? id : "default";
  }

  function crmBuiltinTableView() {
    return CRM_BUILTIN_TABLE_VIEWS.some((view) => view.id === crmTableViewId());
  }

  function crmTableViewDraftFields() {
    const draft = crmTableViewDraft;
    return draft && crmBuiltinTableView() && draft.organization === state.crmView.organization
      && draft.id === crmTableViewId() ? draft.fields : null;
  }

  function crmTableViewFields(id) {
    const view = crmTableViews().find((item) => item.id === id);
    return view ? (Array.isArray(view.columns) ? view.columns : []) : CRM_DEFAULT_VIEW_FIELDS;
  }

  function crmColumns(draft = crmTableViewDraftFields()) {
    const columns = crmListColumns();
    if (crmActiveShared()) return columns;
    const id = crmTableViewId();
    if (id === "all" && !draft) {
      const list = Boolean(crmActiveView());
      return columns.map((column) => ({ ...column, hidden: list && column.custom && column.extra }));
    }
    const fields = (draft || crmTableViewFields(id)).filter((field) => columns.some((column) => column.field === field));
    return [
      ...fields.map((field) => ({ ...columns.find((column) => column.field === field), hidden: false })),
      ...columns.filter((column) => !fields.includes(column.field)).map((column) => ({ ...column, hidden: true })),
    ];
  }

  function selectCrmTableView(id) {
    if (id !== crmTableViewId()) crmTableViewDraft = null;
    crmTableViewChoice = id;
    try { localStorage.setItem(crmTableViewKey(), id); }
    catch { /* a browser with no storage keeps the choice for this page */ }
    renderCrmTableViews();
    renderCrmContacts();
  }

  function renderCrmTableViews() {
    const select = $(`[data-crm-table-view]`); if (!select) return;
    const views = [...CRM_BUILTIN_TABLE_VIEWS, ...crmTableViews()];
    const id = crmTableViewId();
    const modified = Boolean(crmTableViewDraftFields());
    select.innerHTML = views.map((view) => `<option value="${escapeHtml(view.id)}">${escapeHtml(view.name)}${modified && view.id === id ? " (modified)" : ""}</option>`).join("");
    select.value = id;
    select.disabled = Boolean(crmActiveShared()) || state.crmView.layout === "board";
    const save = $(`[data-crm-table-view-save]`);
    if (save) {
      save.classList.toggle("sm:inline-flex", modified);
      save.disabled = select.disabled;
    }
  }

  async function saveCrmTableViews(views, done) {
    const previous = crmTableViews();
    state.crmView.settings = { ...state.crmView.settings, tableViews: views };
    renderCrmTableViews();
    renderCrmContacts();
    try {
      const payload = await crmRequest(crmOrganizationPath("/settings"), "PUT", { tableViews: views });
      state.crmView.settings = { ...state.crmView.settings, tableViews: payload.settings?.tableViews || views };
      renderCrmTableViews();
      renderCrmContacts();
      if (done) crmMessage("notice", done);
    } catch (error) {
      state.crmView.settings = { ...state.crmView.settings, tableViews: previous };
      renderCrmTableViews();
      renderCrmContacts();
      crmMessage("error", `Could not save the view: ${error.message}`);
    }
  }

  async function createCrmTableView(fields) {
    const name = (prompt("Save these columns as a view named") || "").trim().slice(0, 60);
    if (!name) { renderCrmContacts(); return; }
    await addCrmTableView(name, fields, `View "${name}" saved.`);
  }

  async function saveCrmTableViewDraft() {
    const fields = crmTableViewDraftFields();
    if (!fields) return;
    const names = new Set(crmTableViews().map((view) => String(view.name || "").toLowerCase()));
    let number = 1;
    while (names.has(`view ${number}`)) number += 1;
    const name = `View ${number}`;
    await addCrmTableView(name, fields, `Saved as "${name}". Rename it from the Columns menu.`);
  }

  async function addCrmTableView(name, fields, done) {
    const taken = new Set([...CRM_BUILTIN_TABLE_VIEWS, ...crmTableViews()].map((view) => view.id));
    let stamp = Date.now();
    while (taken.has(`view-${stamp.toString(36)}`)) stamp += 1;
    const id = `view-${stamp.toString(36)}`;
    crmTableViewDraft = null;
    crmTableViewChoice = id;
    try { localStorage.setItem(crmTableViewKey(), id); }
    catch { /* a browser with no storage keeps the choice for this page */ }
    await saveCrmTableViews([...crmTableViews(), { id, name, columns: fields }], done);
  }

  async function renameCrmTableView() {
    const id = crmTableViewId();
    const view = crmTableViews().find((item) => item.id === id);
    if (!view) return;
    const name = (prompt("View name", view.name) || "").trim().slice(0, 60);
    if (!name || name === view.name) return;
    await saveCrmTableViews(crmTableViews().map((item) => item.id === id ? { ...item, name } : item));
  }

  async function deleteCrmTableView() {
    const id = crmTableViewId();
    const view = crmTableViews().find((item) => item.id === id);
    if (!view || !confirm(`Delete the view "${view.name}" for everyone in the organization? The contacts and their fields stay.`)) return;
    selectCrmTableView("default");
    await saveCrmTableViews(crmTableViews().filter((item) => item.id !== id), `View "${view.name}" deleted.`);
  }

  function crmTableViewMenuItems() {
    const custom = !crmBuiltinTableView();
    return [
      { heading: "View" },
      { label: "New view…", run: () => void createCrmTableView(crmVisibleColumns().map((column) => column.field)) },
      { label: "Rename view…", disabled: !custom, run: () => void renameCrmTableView() },
      { label: "Delete view…", disabled: !custom, danger: true, run: () => void deleteCrmTableView() },
    ];
  }

  function crmBaseColumn(field) {
    return CRM_BASE_COLUMNS.find((column) => column.field === field)
      || { field, label: crmColumnLabel(field), custom: true, width: "12rem" };
  }

  function crmVisibleColumns() {
    const columns = crmColumns();
    const visible = columns.filter((column) => !column.hidden);
    // A layout that hides every column leaves no heading to right-click, so the
    // tab could never be repaired from the table itself. Sheets an earlier
    // release opened that way fall back to the built-in columns.
    return visible.length ? visible : columns.filter((column) => !column.custom);
  }

  async function saveCrmColumns(columns) {
    if (crmActiveShared()) return;
    const fields = columns.filter((column) => !column.hidden).map((column) => column.field);
    const id = crmTableViewId();
    if (crmBuiltinTableView()) {
      const unchanged = crmColumns(null).filter((column) => !column.hidden).map((column) => column.field);
      crmTableViewDraft = fields.join("\n") === unchanged.join("\n") ? null
        : { organization: state.crmView.organization, id, fields };
      renderCrmTableViews();
      renderCrmContacts();
      return;
    }
    const known = new Set(columns.map((column) => column.field));
    const kept = crmTableViewFields(id).filter((field) => !known.has(field));
    await saveCrmTableViews(crmTableViews().map((view) => view.id === id
      ? { ...view, columns: [...fields, ...kept] } : view));
  }

  function crmShowColumn(field) {
    if (crmTableViewId() === "default" && !crmTableViewDraftFields()) selectCrmTableView("all");
    if (crmColumns().find((column) => column.field === field)?.hidden) crmSetColumnHidden(field, false);
  }

  function crmMoveColumn(field, offset) {
    const columns = crmColumns();
    const from = columns.findIndex((column) => column.field === field);
    if (from < 0) return;
    // Moving is done against the visible order: a hidden neighbour would make
    // "move left" look like it did nothing at all.
    const visible = columns.filter((column) => !column.hidden);
    const at = visible.findIndex((column) => column.field === field);
    const target = visible[at + offset];
    if (!target) return;
    const to = columns.findIndex((column) => column.field === target.field);
    columns.splice(to, 0, columns.splice(from, 1)[0]);
    void saveCrmColumns(columns);
  }

  function crmSetColumnHidden(field, hidden) {
    const columns = crmColumns();
    const column = columns.find((item) => item.field === field);
    if (!column) return;
    if (hidden && columns.filter((item) => !item.hidden).length <= 1) {
      crmMessage("error", "Keep at least one column in the view.");
      return;
    }
    column.hidden = hidden;
    void saveCrmColumns(columns);
  }

  function crmDropColumn(field, target) {
    if (!field || !target || field === target) return;
    const columns = crmColumns();
    const from = columns.findIndex((column) => column.field === field);
    const to = columns.findIndex((column) => column.field === target);
    if (from < 0 || to < 0) return;
    columns.splice(to, 0, columns.splice(from, 1)[0]);
    void saveCrmColumns(columns);
  }

  // --- right-click menus --------------------------------------------------
  // One floating menu serves both surfaces that answer a right-click: a column
  // heading and a sheet tab.

  function closeCrmMenu() {
    const menu = $(`[data-crm-menu]`);
    if (menu) { menu.hidden = true; menu.innerHTML = ""; menu.onclick = null; }
  }

  function openCrmMenu(event, items) {
    const menu = $(`[data-crm-menu]`);
    const entries = items.filter(Boolean);
    if (!menu || !entries.length) return;
    event.preventDefault();
    // The click that opens the menu must not reach the document listener that
    // closes it again.
    event.stopPropagation();
    menu.innerHTML = entries.map((item, index) => (item.separator
      ? `<div style="margin:.25rem 0" class="border-t border-border"></div>`
      : item.heading !== undefined
      ? `<p class="truncate px-3 py-1 text-[10px] font-semibold uppercase text-muted-foreground">${escapeHtml(item.heading)}</p>`
      : `<button data-crm-menu-item="${index}" type="button" role="menuitem"${item.disabled ? " disabled" : ""}${item.title ? ` title="${escapeHtml(item.title)}"` : ""} class="block w-full truncate px-3 py-1.5 text-left text-xs ${item.danger ? "text-destructive" : "text-foreground"} hover:bg-secondary disabled:cursor-not-allowed disabled:opacity-40">${escapeHtml(item.label)}</button>`)).join("");
    menu.hidden = false;
    // Clamp into the viewport so a heading near an edge still shows the whole
    // menu rather than pushing the page sideways.
    const bounds = menu.getBoundingClientRect();
    menu.style.left = `${Math.max(4, Math.min(event.clientX, window.innerWidth - bounds.width - 4))}px`;
    menu.style.top = `${Math.max(4, Math.min(event.clientY, window.innerHeight - bounds.height - 4))}px`;
    menu.onclick = (click) => {
      const button = click.target.closest("[data-crm-menu-item]");
      if (!button) return;
      const entry = entries[Number(button.dataset.crmMenuItem)];
      closeCrmMenu();
      entry?.run?.();
    };
  }

  // Every field of the sheet on screen, ticked while it is shown. The same
  // entries hang off the tab menu, which stays reachable even when the heading
  // row is empty, so a hidden field can always be brought back.
  function crmFieldMenuItems() {
    const columns = crmColumns();
    const shown = columns.filter((column) => !column.hidden).length;
    return columns.map((column) => ({
      label: `${column.hidden ? "\u2002\u2002" : "\u2713 "}${column.label}`,
      disabled: !column.hidden && shown <= 1,
      run: () => crmSetColumnHidden(column.field, !column.hidden),
    }));
  }

  function openCrmColumnMenu(event, field) {
    if (crmActiveShared()) return;
    const columns = crmColumns();
    const visible = columns.filter((column) => !column.hidden);
    const at = visible.findIndex((column) => column.field === field);
    openCrmMenu(event, [
      { heading: crmColumnLabel(field) },
      { label: "Move left", disabled: at <= 0, run: () => crmMoveColumn(field, -1) },
      { label: "Move right", disabled: at < 0 || at >= visible.length - 1, run: () => crmMoveColumn(field, 1) },
      { label: "Remove from this view", disabled: visible.length <= 1, run: () => crmSetColumnHidden(field, true) },
      ...crmDeleteColumnItems(columns.find((column) => column.field === field)),
      { separator: true },
      { heading: "Fields" },
      ...crmFieldMenuItems(),
      { label: "Add column…", run: () => void addCrmCustomColumn() },
      ...crmRestoreColumnItems(),
      { separator: true },
      ...crmTableViewMenuItems(),
    ]);
  }

  function crmDeleteColumnItems(column) {
    if (!column || column.computed || CRM_KEPT_COLUMNS.has(column.field)) return [];
    if (column.custom) return [{ label: "Delete column…", run: () => void deleteCrmColumn(column.field) }];
    const manages = Boolean(state.crmView.canManageSettings);
    return [{
      label: manages ? "Delete column…" : "Delete column… (owners and admins)",
      disabled: !manages,
      run: () => void deleteCrmColumn(column.field),
    }];
  }

  function crmRestoreColumnItems() {
    const removed = new Set(state.crmView.settings?.removedColumns || []);
    const columns = CRM_BASE_COLUMNS.filter((column) => removed.has(column.field));
    if (!columns.length) return [];
    const manages = Boolean(state.crmView.canManageSettings);
    return [
      { separator: true },
      { heading: "Restore deleted column" },
      ...columns.map((column) => ({
        label: column.label, disabled: !manages,
        run: () => void restoreCrmColumn(column.field),
      })),
    ];
  }

  async function restoreCrmColumn(field) {
    const removed = (state.crmView.settings?.removedColumns || []).filter((item) => item !== field);
    try {
      const payload = await crmRequest(crmOrganizationPath("/settings"), "PUT", { removedColumns: removed });
      state.crmView.settings = payload.settings || { ...state.crmView.settings, removedColumns: removed };
      crmShowColumn(field);
      renderCrmContacts();
      crmMessage("notice", `Column "${crmColumnLabel(field)}" restored.`);
    } catch (error) { crmMessage("error", `Could not restore the column: ${error.message}`); }
  }

  // Erases a column's value from every contact and drops it from every
  // sheet's layout, for the whole organization.
  async function deleteCrmColumn(field) {
    const label = crmColumnLabel(field);
    const builtin = CRM_BASE_COLUMNS.some((column) => column.field === field);
    const filled = state.crmView.contacts.filter((contact) => {
      const value = builtin ? contact[field] : (contact.fields || {})[field];
      return Array.isArray(value) ? value.length > 0 : typeof value === "number" ? value > 0 : Boolean(String(value ?? "").trim());
    }).length;
    if (!confirm(`Delete the "${label}" column?\n\nIts value is erased from ${filled === 1 ? "1 contact" : `${filled} contacts`} for the whole organization and the column is removed from every sheet.${builtin ? " It can be added back later, empty." : ""} This cannot be undone.`)) return;
    try {
      await crmRequest(crmOrganizationPath("/contacts/delete-column"), "POST", { field });
      crmMessage("notice", `Column "${label}" deleted.`);
      await loadCrm();
    } catch (error) { crmMessage("error", `Could not delete the column: ${error.message}`); }
  }

  // The same field list as the tab and heading menus, opened from the
  // Columns button, so it drops below the button rather than at the pointer.
  function openCrmColumnsMenu(event) {
    if (crmActiveShared()) return;
    const bounds = event.currentTarget.getBoundingClientRect();
    openCrmMenu({
      preventDefault: () => event.preventDefault(), stopPropagation: () => event.stopPropagation(),
      clientX: bounds.left, clientY: bounds.bottom + 4,
    }, [...crmTableViewMenuItems(), { separator: true }, { heading: "Fields" }, ...crmFieldMenuItems(),
      { label: "Add column…", run: () => void addCrmCustomColumn() }]);
  }

  function openCrmSharedMenu(event, id) {
    const view = state.crmView.sharedViews.find((item) => item.id === id) || null;
    if (!view) return;
    openCrmMenu(event, [
      { heading: `${view.name} · shared by ${view.owner || view.organization}` },
      { label: `Copy into ${state.crmView.organization}…`, run: () => void copyCrmSharedSheet(view) },
      { label: "Copy link", run: () => void copyCrmListLink(view) },
    ]);
  }

  // --- the contact right-click menu ---------------------------------------
  // openCrmMenu draws a flat list, so the groups are headings rather
  // than submenus, and a long group is cut short with the tab menu as the way
  // to reach the rest.
  const CRM_MENU_GROUP = 8;

  // The worker's membership verbs, applied to the page's copy of a contact so
  // a move shows before the reply: flow and stage replace, `setViews` replaces
  // the lists (and wins over `views`, which adds), `removeViews` drops.
  function crmWithContactEdit(contact, body) {
    const replace = "setViews" in body;
    const views = replace ? [] : [...(contact.views || [])];
    for (const view of (replace ? body.setViews : body.views) || []) {
      if (!views.includes(view)) views.push(view);
    }
    const dropped = new Set(body.removeViews || []);
    return {
      ...contact,
      ..."flow" in body ? { flow: body.flow } : {},
      ..."stage" in body ? { stage: body.stage } : {},
      ..."lastContactedAt" in body ? { lastContactedAt: body.lastContactedAt } : {},
      ..."owner" in body ? { owner: body.owner } : {},
      views: views.filter((view) => !dropped.has(view)),
    };
  }

  // A move shows at once: the row changes on the page, one PATCH goes out, and
  // the reply's copy replaces it. Only a failure reloads the organization, to
  // put back what the worker still has.
  async function crmPatchContact(id, body, note, failure) {
    const replace = (contact) => {
      const at = state.crmView.contacts.findIndex((item) => item.id === id);
      if (at >= 0 && contact) { state.crmView.contacts[at] = contact; renderCrm(); }
    };
    const current = crmContactById(id);
    if (current) replace(crmWithContactEdit(current, body));
    try {
      const payload = await crmRequest(crmOrganizationPath(`/contacts/${id}`), "PATCH", body);
      replace(payload.contact);
      crmMessage("notice", note);
    } catch (error) {
      crmMessage("error", `${failure}: ${error.message}`);
      await loadCrm();
    }
  }

  function crmMenuGroup(entries) {
    return entries.length > CRM_MENU_GROUP
      ? [...entries.slice(0, CRM_MENU_GROUP),
         { label: `…and ${entries.length - CRM_MENU_GROUP} more`, disabled: true }]
      : entries;
  }

  function openCrmContactMenu(event, id) {
    const contact = crmContactById(id);
    if (!contact || crmActiveShared()) return;
    const ids = crmMenuSelection(id);
    const contacts = ids.map(crmContactById).filter(Boolean);
    const many = contacts.length > 1;
    const name = contact.name || contact.email || "Contact";
    const quoted = (value) => `“${value}”`;
    // Belonging to a pipeline list is having its flow, so pipelines are moved
    // to by flow and only plain lists are list memberships.
    const lists = state.crmView.views.filter((view) => !view.flow);
    const only = (one, view) => (one.views || []).length === 1 && one.views[0] === view.id;
    const flow = contacts.every((one) => one.flow === contact.flow)
      ? crmFlows().find((item) => item.id === contact.flow) || null : null;
    const open = crmActiveView();
    const pipelines = crmMenuGroup(crmPipelineMoveItems(ids));
    const stages = crmMenuGroup((flow?.stages || [])
      .map((stage) => ({
        label: titleCaseSlug(stage),
        disabled: contacts.every((one) => one.stage === stage),
        run: () => void crmPatchContacts(contacts.filter((one) => one.stage !== stage)
          .map((one) => ({ id: one.id, stage })), "moved", `to the ${quoted(stage)} stage`),
      })));
    // "Move to" is the target list and no other; "Add to" leaves the rest.
    const moves = crmMenuGroup(lists
      .filter((view) => contacts.some((one) => !only(one, view)))
      .map((view) => ({
        label: view.name,
        run: () => void crmPatchContacts(contacts.filter((one) => !only(one, view))
          .map((one) => ({ id: one.id, setViews: [view.id] })), "moved", `to ${quoted(view.name)} only`),
      })));
    const adds = crmMenuGroup(lists
      .filter((view) => contacts.some((one) => !(one.views || []).includes(view.id)))
      .map((view) => ({
        label: view.name,
        run: () => void crmPatchContacts(contacts.filter((one) => !(one.views || []).includes(view.id))
          .map((one) => ({ id: one.id, views: [view.id] })), "added", `to ${quoted(view.name)}`),
      })));
    const keeps = crmMenuGroup(contacts.map((keep) => ({
      label: crmMergeLabel(keep),
      run: () => void mergeCrmContacts(ids.filter((other) => other !== keep.id), keep.id),
    })));
    openCrmMenu(event, [
      { heading: many ? `${contacts.length} contacts` : name },
      { label: "Mark contacted now", run: () => void crmMarkContacted(ids, Date.now()) },
      ...(many ? [] : [
        { label: "Edit…", run: () => openCrmContact(contact) },
        { label: "Email", disabled: !contact.email, run: () => openCrmEmail(contact) },
        { label: "Copy prompt", run: () => void copyCrmOutreachPrompt(contact) },
        { label: "Copy link", run: () => void copyCrmContactLink(id) },
      ]),
      ...(pipelines.length ? [{ separator: true }, { heading: "Move to pipeline list" }, ...pipelines] : []),
      ...(stages.length ? [{ separator: true }, { heading: `Set stage in ${flow.name}` }, ...stages] : []),
      ...(moves.length ? [{ separator: true }, { heading: "Move to list" }, ...moves] : []),
      ...(adds.length ? [{ separator: true }, { heading: "Add to list" }, ...adds] : []),
      ...(open && !open.virtual && (!open.flow || crmPipelineLists(open.flow).length > 1)
        && contacts.some((one) => (one.views || []).includes(open.id)) ? [
        { separator: true },
        { label: `Remove from ${quoted(open.name)}`, run: () => removeCrmContactsFromSheet(ids, open.id) },
      ] : []),
      { separator: true },
      { label: "Assign owner…", run: () => void assignCrmContactsOwner(ids) },
      { label: "New pipeline…", run: openCrmPipelineDialog },
      { label: "Manage pipelines…", run: openCrmPipelineSettings },
      { separator: true },
      ...(many ? [
        { heading: "Merge selected into" }, ...keeps,
        { separator: true },
        { label: `Delete ${contacts.length} contacts…`, danger: true, run: () => void deleteCrmContacts(ids) },
      ] : [
        { label: "Merge into…", disabled: state.crmView.contacts.length < 2, run: () => openCrmMergePicker(id) },
        { label: "Delete contact", danger: true, run: () => void deleteCrmContact(id) },
      ]),
    ]);
  }

  function crmMergeLabel(contact) {
    const name = String(contact?.name || "").trim();
    const email = String(contact?.email || "").trim();
    if (!name) return email || "(unnamed contact)";
    return email ? `${name} — ${email}` : name;
  }

  function crmFlowLabel(flowId) {
    return crmFlows().find((flow) => flow.id === flowId)?.name || String(flowId || "");
  }

  function crmMergeTargets(duplicate, contacts = state.crmView.contacts) {
    const words = (value) => new Set(String(value || "").toLowerCase()
      .split(/[^\p{L}\p{N}_]+/u).filter((word) => word.length > 1));
    const names = words(duplicate.name);
    const email = String(duplicate.email || "").trim().toLowerCase();
    const company = String(duplicate.company || "").trim().toLowerCase();
    const fold = (value) => value.toLowerCase();
    return contacts
      .filter((other) => other.id && other.id !== duplicate.id)
      .map((other) => ({
        id: other.id,
        score: (email && String(other.email || "").trim().toLowerCase() === email ? 4 : 0)
          + 2 * [...words(other.name)].filter((word) => names.has(word)).length
          + (company && String(other.company || "").trim().toLowerCase() === company ? 1 : 0),
        label: crmMergeLabel(other),
        detail: [String(other.company || "").trim(), other.flow ? crmFlowLabel(other.flow) : ""]
          .filter((part) => /\S/.test(part)).join(" · "),
      }))
      .sort((a, b) => b.score - a.score
        || (fold(a.label) < fold(b.label) ? -1 : fold(a.label) > fold(b.label) ? 1 : 0));
  }

  // Folds a box made twice for one person into the one kept: the kept record's
  // details win, the duplicate fills its blanks, brings its tags, lists, notes
  // and email history, and is deleted.
  async function bulkEditShownCrmContacts(field) {
    closeCrmMenu();
    if (crmActiveShared()) return;
    const contacts = crmFilteredContacts();
    if (!contacts.length) { crmMessage("error", "No contacts are shown to edit."); return; }
    const count = `${contacts.length} shown contact${contacts.length === 1 ? "" : "s"}`;
    const ask = {
      owner: `Assign the ${count} to which account? Leave empty to unassign.`,
      addTags: `Tags to add to the ${count}, separated by commas:`,
      removeTags: `Tags to remove from the ${count}, separated by commas:`,
    }[field];
    const answer = window.prompt(ask, field === "owner" ? String(state.session?.name || "") : "");
    if (answer === null) return;
    const value = field === "owner" ? answer.trim()
      : answer.split(/[,;]/).map((tag) => tag.trim()).filter(Boolean);
    if (field !== "owner" && !value.length) return;
    try {
      for (let start = 0; start < contacts.length; start += CRM_BULK_EDIT_LIMIT) {
        await crmRequest(crmOrganizationPath("/contacts/bulk"), "POST", {
          edits: contacts.slice(start, start + CRM_BULK_EDIT_LIMIT).map((contact) => ({ id: contact.id, [field]: value })),
        });
      }
      crmMessage("notice", `Updated ${count}.`);
    } catch (error) { crmMessage("error", `Could not update the contacts: ${error.message}`); }
    await loadCrm();
  }

  async function mergeCrmContacts(duplicateIds, keepId) {
    const keep = crmContactById(keepId);
    const pending = duplicateIds.filter((id) => id !== keepId && crmContactById(id));
    if (!keep || !pending.length) return;
    const names = pending.map((id) => `“${crmMergeLabel(crmContactById(id))}”`).join(", ");
    const kept = `“${crmMergeLabel(keep)}”`;
    const one = pending.length === 1;
    if (!confirm(`Merge ${names} into ${kept}?\n\n${kept} keeps what it has, fills its blanks from ${names} and gains ${one ? "its" : "their"} tags, lists, notes and email history. ${names} ${one ? "is" : "are"} then deleted.`)) return;
    crmMessage("notice", `Merging ${names} into ${kept}…`);
    try {
      for (const id of pending) {
        await crmRequest(crmOrganizationPath("/contacts/merge"), "POST", { from: id, into: keepId });
      }
      crmMessage("notice", `Merged ${pending.length} ${one ? "contact" : "contacts"} into ${kept}.`);
    } catch (error) { crmMessage("error", `Contacts were not merged into ${kept}: ${error.message}`); }
    crmSelection.ids = new Set([keepId]);
    state.crmView.duplicates = null;
    await loadCrm();
    if ($(`[data-crm-focus]`)?.value === "duplicates") await loadCrmDuplicates();
  }

  function crmPipelineMoveItems(ids) {
    const contacts = ids.map(crmContactById).filter(Boolean);
    const pipelines = crmPipelineIds();
    return crmTabViews().filter((view) => view.flow).map((view) => {
      const allThere = contacts.every((contact) => crmContactOnTab(contact, view, pipelines));
      return {
        label: crmPipelineLists(view.flow).length > 1 ? `${view.name} · ${crmFlowName(view.flow)}` : view.name,
        disabled: allThere, title: allThere ? "Already in this list" : "",
        run: () => moveCrmContactsToSheet(ids, view.id),
      };
    });
  }

  function crmMoveToSheetEdits(ids, sheetId) {
    const sheet = crmTabView(sheetId);
    if (!sheet || sheet.id === CRM_NO_PIPELINE_TAB) return [];
    const flowId = sheet.flow || "";
    const stages = crmFlowStages(flowId);
    const siblings = crmPipelineLists(flowId).filter((id) => id !== sheetId);
    const pipelines = crmPipelineIds();
    const edits = [];
    for (const id of ids) {
      const contact = crmContactById(id);
      if (!contact || crmContactOnTab(contact, sheet, pipelines)) continue;
      const edit = { id };
      if (flowId && contact.flow !== flowId) {
        const stage = String(contact.stage || "");
        edit.flow = flowId;
        edit.stage = stages.includes(stage) || !stages.length ? stage : stages[0];
      }
      if (!sheet.virtual) {
        edit.views = [sheetId];
        if (flowId && siblings.length) edit.removeViews = siblings;
      }
      edits.push(edit);
    }
    return edits;
  }

  async function crmBulkMoveContacts(edits, gerund, past, target) {
    if (!edits.length) return;
    const byId = new Map(edits.map((edit) => [edit.id, edit]));
    state.crmView.contacts = state.crmView.contacts.map((contact) =>
      byId.has(contact.id) ? crmWithContactEdit(contact, byId.get(contact.id)) : contact);
    renderCrm();
    const count = edits.length;
    const noun = count === 1 ? "contact" : "contacts";
    crmMessage("notice", `${gerund} ${count} ${noun} ${target}…`);
    try {
      const payload = await crmRequest(crmOrganizationPath("/contacts/bulk"), "POST", { edits });
      const saved = new Map((payload.contacts || []).map((contact) => [contact.id, contact]));
      state.crmView.contacts = state.crmView.contacts.map((contact) => saved.get(contact.id) || contact);
      renderCrm();
      const missing = (payload.missing || []).length;
      if (missing) {
        crmMessage("error", `${missing} of ${count} contacts were not ${past} ${target}: they no longer exist.`);
        await loadCrm();
        return;
      }
      crmMessage("notice", `${count} ${noun} ${past} ${target}.`);
    } catch (error) {
      crmMessage("error", `${count} ${noun} were not ${past} ${target}: ${error.message}`);
      await loadCrm();
    }
  }

  function moveCrmContactsToSheet(ids, sheetId) {
    const sheet = crmTabView(sheetId);
    if (!sheet || !ids.length) return;
    const edits = crmMoveToSheetEdits(ids, sheetId);
    if (!edits.length) { crmMessage("notice", `Every selected contact is already in “${sheet.name}”.`); return; }
    const pipeline = Boolean(sheet.flow);
    void crmBulkMoveContacts(edits, pipeline ? "Moving" : "Adding", pipeline ? "moved" : "added", `to “${sheet.name}”`);
  }

  function removeCrmContactsFromSheet(ids, sheetId) {
    const sheet = crmTabView(sheetId);
    if (!sheet || !ids.length) return;
    const edits = ids.map(crmContactById)
      .filter((contact) => contact && (contact.views || []).includes(sheetId))
      .map((contact) => ({ id: contact.id, removeViews: [sheetId] }));
    if (!edits.length) { crmMessage("notice", `No selected contact is on “${sheet.name}”.`); return; }
    void crmBulkMoveContacts(edits, "Removing", "removed", `from “${sheet.name}”`);
  }

  function crmSheetShareSummary(view) {
    const names = crmSheetShareNames(view);
    const picked = new Set(view?.sharedWith || []);
    const count = names.length === 1 ? "1 person" : `${names.length} people`;
    return [
      !names.length ? "Private to its owner"
        : view?.visibility === "private" ? `Shared with ${count}`
        : `Open to ${count} in ${state.crmView.organization}`,
      ...names.map((name) => (picked.has(name) ? name : `${name} · member`)),
      `Owner: ${view?.owner || state.crmView.organization}`,
    ];
  }

  function crmSheetShareItems(view) {
    const lines = crmSheetShareSummary(view);
    const people = lines.slice(1, -1);
    return [
      { heading: lines[0] },
      ...people.slice(0, CRM_SHARE_SHOWN).map((line) => ({ label: line, disabled: true })),
      ...(people.length > CRM_SHARE_SHOWN ? [{ label: `…and ${people.length - CRM_SHARE_SHOWN} more`, disabled: true }] : []),
      { label: lines[lines.length - 1], disabled: true },
    ];
  }

  function crmCanDeleteList(view) {
    return Boolean(view && !view.virtual && view.canManage
      && (!view.flow || crmPipelineLists(view.flow).length > 1));
  }

  function openCrmSheetMenu(event, id) {
    const view = crmTabView(id);
    if (!view) return;
    const stored = !view.virtual;
    const manages = Boolean(state.crmView.canManageSettings);
    const flowId = view.flow || "";
    const onScreen = !state.crmView.activeShared && state.crmView.activeView === view.id;
    const fields = onScreen
      ? [{ separator: true }, { heading: "Fields" }, ...crmFieldMenuItems()] : [];
    const at = state.crmView.views.findIndex((item) => item.id === id);
    const canShare = !stored || Boolean(view.canManage);
    const targets = state.crmView.views.filter((other) => other.id !== id && other.canManage);
    const flows = crmFlows();
    const lastPipeline = flows.length === 1 && flows[0].id === flowId;
    const items = [...crmSheetShareItems(view), { separator: true }];
    if (id !== CRM_NO_PIPELINE_TAB) {
      items.push(
        { label: "Rename…", disabled: !stored && !manages,
          title: stored || manages ? "" : "Only organization owners and admins can rename pipelines",
          run: () => void renameCrmSheet(view) },
        { label: "Add members…", disabled: !canShare,
          title: canShare ? "" : "Only the list's owner and organization owners and admins can add members",
          run: () => openCrmMembersDialog(view) });
    }
    if (stored) {
      items.push(
        { label: "Rename or share…", run: () => openCrmSheetDialog(view) },
        { label: "Copy link", run: () => void copyCrmListLink(view) },
        { label: "Activity history…", run: () => void openCrmListHistory(view) },
        { label: "Move left", disabled: at <= 0, run: () => crmMoveSheet(view.id, -1) },
        { label: "Move right", disabled: at < 0 || at >= state.crmView.views.length - 1,
          run: () => crmMoveSheet(view.id, 1) },
        { label: "Import a CSV into this list…", run: () => crmPickCsv(view.id) },
        { label: "Import Mastodon followers into this list…", run: () => openCrmMastodonDialog(view.id) },
        ...(targets.length && view.canManage ? [
          { separator: true },
          { heading: "Merge into" },
          ...crmMenuGroup(targets.map((other) => ({
            label: other.flow ? `${other.name} · ${crmFlowName(other.flow)}` : other.name,
            title: "Put every contact on this list onto that one, carry its fields over, and delete this list",
            run: () => void mergeCrmSheetInto(view.id, other.id),
          }))),
        ] : [{ label: "Merge into…", disabled: true,
          title: view.canManage ? "There is no other list you can merge it into"
            : "Only the list's owner and organization owners and admins can merge it" }]),
        { separator: true },
        { heading: "Pipeline type" },
        { label: `${view.flow ? "\u2002\u2002" : "\u2713 "}No pipeline (plain list)`,
          disabled: !manages || !view.flow,
          run: () => void crmSetSheetFlow(view, "") },
        ...flows.map((flow) => ({
          label: `${view.flow === flow.id ? "\u2713 " : "\u2002\u2002"}${flow.name}`,
          disabled: !manages || view.flow === flow.id,
          run: () => void crmSetSheetFlow(view, flow.id),
        })),
        { label: "New list…", run: () => openCrmSheetDialog(null) },
        { label: "New pipeline…", disabled: !manages, run: openCrmPipelineDialog },
        { label: "Manage pipelines…", disabled: !manages, run: () => openCrmPipelineSettings() },
        ...(flowId ? [crmEditStagesItem(flowId)] : []));
    } else {
      items.push(
        ...(flowId ? [crmEditStagesItem(flowId)] : []),
        { label: "New list…", run: () => openCrmSheetDialog(null) },
        { label: "New pipeline…", disabled: !manages, run: openCrmPipelineDialog },
        { label: "Manage pipelines…", disabled: !manages, run: () => openCrmPipelineSettings() },
        { label: "Import a CSV as a new list…", run: () => crmPickCsv("") },
        { label: "Import Mastodon followers as a new list…", run: () => openCrmMastodonDialog("") });
    }
    items.push(...fields);
    if (stored && flowId) {
      const deletable = crmCanDeleteList(view);
      items.push({ separator: true }, {
        label: "Delete list…", danger: true, disabled: !deletable,
        title: deletable ? "" : !view.canManage
          ? "Only the list's owner and organization owners and admins can delete it"
          : `The only list on “${crmFlowName(flowId)}”: use Delete pipeline… to remove it`,
        run: () => void deleteCrmSheet(view),
      });
    }
    if (flowId) {
      items.push({ separator: true }, {
        label: "Delete pipeline…", danger: true, disabled: !manages || lastPipeline,
        title: !manages ? "Only organization owners and admins can delete pipelines"
          : lastPipeline ? "An organization keeps at least one pipeline" : "",
        run: () => void deleteCrmPipeline(view),
      });
    } else if (stored) {
      items.push({ separator: true }, {
        label: "Delete list…", danger: true, disabled: !view.canManage,
        run: () => void deleteCrmSheet(view),
      });
    }
    openCrmMenu(event, items);
  }

  function crmMergePlan(sourceId, targetId) {
    const source = state.crmView.views.find((view) => view.id === sourceId);
    const target = state.crmView.views.find((view) => view.id === targetId);
    if (!source || !target || sourceId === targetId) return null;
    const flowId = target.flow || "";
    const stages = crmFlowStages(flowId);
    const dropped = crmPipelineLists(flowId).filter((id) => id !== targetId);
    if (!dropped.includes(sourceId)) dropped.push(sourceId);
    const pipelines = crmPipelineIds();
    const edits = state.crmView.contacts.filter((contact) => crmContactOnTab(contact, source, pipelines)).map((contact) => {
      const edit = { id: contact.id, views: [targetId], removeViews: [...dropped] };
      if (flowId && contact.flow !== flowId) {
        const stage = String(contact.stage || "");
        edit.flow = flowId;
        edit.stage = stages.includes(stage) || !stages.length ? stage : stages[0];
      }
      return edit;
    });
    const columns = (Array.isArray(target.columns) ? target.columns : []).map((column) => ({ ...column }));
    const shownIn = (field) => columns.some((column) => column.field === field && !column.hidden);
    let sourceColumns = Array.isArray(source.columns) ? source.columns : [];
    if (!columns.some((column) => !column.hidden)) {
      sourceColumns = [...CRM_CONTACT_COLUMNS.map((column) => ({
        field: column.field, label: column.label, custom: false, hidden: false,
      })), ...sourceColumns];
    }
    let columnsChanged = false;
    for (const column of sourceColumns) {
      const field = String(column?.field || "");
      if (!field || column.hidden || shownIn(field)) continue;
      const kept = columns.filter((item) => item.field === field);
      if (kept.length) kept.forEach((item) => { item.hidden = false; });
      else columns.push({ ...column, hidden: false });
      columnsChanged = true;
    }
    return { source, target, edits, columns, columnsChanged };
  }

  async function mergeCrmSheetInto(sourceId, targetId) {
    const plan = crmMergePlan(sourceId, targetId);
    if (!plan) return;
    const { source, target, edits, columns, columnsChanged } = plan;
    const moving = edits.length === 1 ? "1 contact" : `${edits.length} contacts`;
    const joins = target.flow
      ? ` They join the ${crmFlowName(target.flow)} pipeline, keeping a stage it shares and starting the rest at its first stage.` : "";
    if (!confirm(`Merge “${source.name}” (${moving}) into “${target.name}”?\n\nIts contacts move onto “${target.name}”, every field it shows is shown there too, and “${source.name}” is then deleted.${joins}`)) return;
    crmMessage("notice", `Merging “${source.name}” into “${target.name}”…`);
    try {
      if (edits.length) await crmRequest(crmOrganizationPath("/contacts/bulk"), "POST", { edits });
    } catch (error) {
      crmMessage("error", `“${source.name}” was not merged into “${target.name}”: ${error.message}`);
      await loadCrm();
      return;
    }
    try {
      if (columnsChanged) await crmRequest(crmOrganizationPath(`/views/${target.id}`), "PATCH", { columns });
    } catch (error) {
      crmMessage("error", `The fields on “${target.name}” were not saved: ${error.message}`);
      await loadCrm();
      return;
    }
    let failure = "";
    try { await crmRequest(crmOrganizationPath(`/views/${source.id}`), "DELETE", {}); }
    catch (error) { failure = error.message; }
    state.crmView.activeView = target.id;
    crmSetListParam(target.id);
    await loadCrm();
    if (failure) crmMessage("error", `The contacts moved to “${target.name}” but “${source.name}” was not deleted: ${failure}`);
    else crmMessage("notice", `“${source.name}” merged into “${target.name}”.`);
  }

  async function deleteCrmPipeline(view) {
    const flowId = view?.flow || "";
    if (!flowId) return;
    closeCrmMenu();
    if (!state.crmView.canManageSettings) {
      crmMessage("error", "Only organization owners and admins can delete pipelines.");
      return;
    }
    const flows = crmFlows();
    const remaining = flows.filter((flow) => flow.id !== flowId);
    if (!remaining.length) {
      crmMessage("error", "An organization keeps at least one pipeline. Add another before deleting this one.");
      return;
    }
    const lists = crmPipelineLists(flowId);
    const name = crmFlowName(flowId);
    const contacts = state.crmView.contacts.filter((contact) => contact.flow === flowId).length;
    const people = contacts === 0 ? "No contact is in it."
      : contacts === 1 ? "Its contact stays in the organization, under No pipeline, until it is moved into another pipeline."
      : `Its ${contacts} contacts stay in the organization, under No pipeline, until they are moved into another pipeline.`;
    const bound = !lists.length ? "" : lists.length === 1 ? " The list bound to it is deleted too."
      : ` The ${lists.length} lists bound to it are deleted too.`;
    if (!confirm(`Delete the “${name}” pipeline for everyone in ${state.crmView.organization}?\n\n${people}${bound}`)) return;
    crmMessage("notice", `Deleting the “${name}” pipeline…`);
    try {
      if (remaining.length !== flows.length) {
        await crmRequest(crmOrganizationPath("/settings"), "PUT", { flows: remaining });
      }
    } catch (error) {
      crmMessage("error", `The “${name}” pipeline was not deleted: ${error.message}`);
      await loadCrm();
      return;
    }
    let failure = "";
    for (const listId of lists) {
      try { await crmRequest(crmOrganizationPath(`/views/${listId}`), "DELETE", {}); }
      catch (error) { failure = failure || error.message; }
    }
    await loadCrm();
    if (failure) crmMessage("error", `The “${name}” pipeline was deleted, but a list bound to it was not: ${failure}`);
    else crmMessage("notice", `Pipeline “${name}” deleted.`);
  }

  async function crmSetSheetFlow(view, flow) {
    if (!view || view.flow === flow) return;
    closeCrmMenu();
    try {
      await crmRequest(crmOrganizationPath(`/views/${view.id}`), "PATCH", { flow });
      crmMessage("notice", flow
        ? `“${view.name}” now shows the “${crmFlow(flow).name}” pipeline.`
        : `“${view.name}” is a plain list again; it shows only the contacts added to it.`);
      await loadCrm();
    } catch (error) {
      crmMessage("error", `Could not change the pipeline type: ${error.message}`);
    }
  }

  // --- renaming and moving a tab -----------------------------------------

  async function renameCrmSheet(view) {
    if (!view || view.id === CRM_NO_PIPELINE_TAB) return;
    closeCrmMenu();
    const pipeline = Boolean(view.virtual);
    if (pipeline && !state.crmView.canManageSettings) {
      crmMessage("error", "Only organization owners and admins can rename pipelines.");
      return;
    }
    const typed = prompt(`Rename the “${view.name}” ${pipeline ? "pipeline" : "list"}`, view.name);
    if (typed === null) return;
    const name = typed.trim().slice(0, 60);
    if (!name || name === view.name) return;
    if (pipeline) {
      const flows = crmFlows().map((flow) => (flow.id === view.flow ? { ...flow, name } : flow));
      state.crmView.settings = { ...state.crmView.settings, flows };
      renderCrm();
      try {
        await crmRequest(crmOrganizationPath("/settings"), "PUT", { flows });
        crmMessage("notice", `Pipeline renamed to “${name}”.`);
      } catch (error) {
        crmMessage("error", `Pipelines were not saved: ${error.message}`);
      }
      await loadCrm();
      return;
    }
    const previous = view.name;
    view.name = name;
    renderCrmTabs();
    try {
      await crmRequest(crmOrganizationPath(`/views/${view.id}`), "PATCH", { name });
      crmMessage("notice", `List renamed to “${name}”.`);
    } catch (error) {
      view.name = previous;
      crmMessage("error", `The list was not renamed: ${error.message}`);
      await loadCrm();
    }
  }

  // Tab order is the organization's, not this browser's, so a member who drags
  // a tab moves it for everyone the sheets reach.
  async function saveCrmSheetOrder(order) {
    const before = state.crmView.views;
    state.crmView.views = order.map((id) =>
      before.find((view) => view.id === id)).filter(Boolean);
    renderCrmTabs();
    try {
      const payload = await crmRequest(
        crmOrganizationPath("/views/order"), "POST", { order });
      if (Array.isArray(payload.views)) { state.crmView.views = payload.views; renderCrmTabs(); }
    } catch (error) {
      state.crmView.views = before;
      crmMessage("error", `Could not move the tab: ${error.message}`);
      await loadCrm();
    }
  }

  function crmMoveSheet(id, offset) {
    const order = state.crmView.views.map((view) => view.id);
    const from = order.indexOf(String(id || ""));
    const to = from + offset;
    if (from < 0 || to < 0 || to >= order.length) return;
    order.splice(to, 0, order.splice(from, 1)[0]);
    void saveCrmSheetOrder(order);
  }

  function crmDropSheet(id, target) {
    if (!id || !target || id === target) return;
    const order = state.crmView.views.map((view) => view.id);
    const from = order.indexOf(id); const to = order.indexOf(target);
    if (from < 0 || to < 0) return;
    order.splice(to, 0, order.splice(from, 1)[0]);
    void saveCrmSheetOrder(order);
  }

  // --- sheet settings -----------------------------------------------------

  function selectCrmSheet(id) {
    state.crmView.activeView = String(id || "");
    state.crmView.activeShared = "";
    crmSetListParam(state.crmView.activeView);
    if (state.crmView.section !== "contacts") selectCrmSection("contacts");
    renderCrmContacts();
    crmNoteListViewed();
  }

  function crmNoteListViewed() {
    const id = state.crmView.activeView;
    const org = state.crmView.organization;
    if (!id || !org || state.crmView.activeShared || document.hidden) return;
    if (!state.crmView.views.some((view) => view.id === id)) return;
    const seen = state.crmView.listViewedAt || (state.crmView.listViewedAt = {});
    const key = `${org}/${id}`;
    const now = Date.now();
    if (now - (seen[key] || 0) < CRM_LIST_VIEW_GAP_MS) return;
    seen[key] = now;
    const headers = { accept: "application/json", "content-type": "application/json" };
    const token = state.session?.sessionToken || "";
    if (token) headers.authorization = `Bearer ${token}`;
    fetch(crmOrganizationPath("/views/activity"), {
      method: "POST", headers, cache: "no-store", body: JSON.stringify({ view: id }),
    }).catch(() => {});
  }

  function crmHistoryRows(entries) {
    return entries.map((entry) => {
      const when = new Date(Number(entry.at) || 0);
      const viewed = entry.action === "list.viewed";
      return `<tr class="border-t border-border align-top"><td class="whitespace-nowrap px-2 py-1.5 text-muted-foreground" title="${escapeHtml(when.toLocaleString())}">${escapeHtml(relativeTimeLabel(Number(entry.at) || 0))}</td><td class="whitespace-nowrap px-2 py-1.5 font-medium">${escapeHtml(entry.actor || "someone")}</td><td class="px-2 py-1.5${viewed ? " text-muted-foreground" : ""}">${escapeHtml(entry.text || entry.action || "")}</td></tr>`;
    }).join("");
  }

  async function openCrmListHistory(view) {
    const dialog = $(`[data-crm-history-dialog]`);
    if (!view || !dialog) return;
    state.crmView.historyView = view.id;
    $(`[data-crm-history-title]`).textContent = `Activity history · ${view.name}`;
    const body = $(`[data-crm-history-body]`);
    body.innerHTML = `<p class="text-sm text-muted-foreground">Loading…</p>`;
    if (!dialog.open) dialog.showModal();
    try {
      const payload = await crmRequest(crmOrganizationPath(`/views/activity?view=${encodeURIComponent(view.id)}`));
      if (state.crmView.historyView !== view.id) return;
      const entries = Array.isArray(payload.entries) ? payload.entries : [];
      if (!entries.length) {
        body.innerHTML = `<p class="text-sm text-muted-foreground">Nothing has happened on this list yet.</p>`;
        return;
      }
      const viewers = [...new Set(entries.filter((entry) => entry.action === "list.viewed").map((entry) => entry.actor))].sort();
      body.innerHTML = `<p class="mb-2 text-xs text-muted-foreground">${entries.length.toLocaleString()} event${entries.length === 1 ? "" : "s"}; opened by ${viewers.length ? escapeHtml(viewers.join(", ")) : "no one"}.${payload.more ? " Older events are not shown." : ""}</p>
        <table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="px-2 py-1">When</th><th class="px-2 py-1">Who</th><th class="px-2 py-1">What</th></tr></thead><tbody>${crmHistoryRows(entries)}</tbody></table>`;
    } catch (error) {
      body.innerHTML = `<p class="text-sm text-destructive">${escapeHtml(error.message)}</p>`;
    }
  }

  async function selectCrmSharedSheet(id) {
    state.crmView.activeView = "";
    state.crmView.activeShared = String(id || "");
    if (state.crmView.stageTab) {
      state.crmView.stageTab = "";
      const stage = $(`[data-crm-stage]`); if (stage) stage.value = "";
    }
    crmSetListParam(state.crmView.activeShared);
    if (state.crmView.section !== "contacts") selectCrmSection("contacts");
    renderCrmContacts();
    if (!id || state.crmView.sharedCache[id]) return;
    try {
      const payload = await crmRequest(`/api/crm/-/shared/${encodeURIComponent(id)}`);
      state.crmView.sharedCache[id] = { view: payload.view || null, contacts: payload.contacts || [] };
    } catch (error) {
      crmMessage("error", `Could not open the shared list: ${error.message}`);
      if (error.status === 404) {
        state.crmView.sharedViews = state.crmView.sharedViews.filter((view) => view.id !== id);
        state.crmView.activeShared = "";
      }
    }
    if (state.crmView.activeShared === id || !state.crmView.activeShared) renderCrmContacts();
  }

  async function copyCrmSharedSheet(view) {
    if (!view) return;
    const org = state.crmView.organization;
    if (!confirm(`Copy "${view.name}" from ${view.organization} into ${org}?\n\nIt becomes a new tab of your own; its rows are added to ${org} or merged into contacts you already have by email.`)) return;
    try {
      const payload = await crmRequest(crmOrganizationPath("/views/copy"), "POST", { viewId: view.id });
      state.crmView.activeShared = "";
      if (payload.viewId) state.crmView.activeView = payload.viewId;
      crmMessage("notice", `${payload.view?.name || view.name}: ${payload.created || 0} added, ${payload.updated || 0} merged, ${payload.skipped || 0} skipped.`);
      await loadCrm();
    } catch (error) { crmMessage("error", `Could not copy the list: ${error.message}`); }
  }

  function renderCrmSheetShare() {
    $(`[data-crm-sheet-share-panel]`)?.classList.toggle(
      "hidden", $(`[data-crm-sheet-visibility]`)?.value !== "private");
  }

  function crmListUrl(viewId) {
    const url = new URL(location.pathname, location.origin);
    url.searchParams.set("list", viewId);
    return url.toString();
  }

  function crmSetLinkParam(name, id) {
    const url = new URL(location.href);
    if (id) {
      url.searchParams.delete("org");
      url.searchParams.set(name, id);
    } else {
      url.searchParams.delete(name);
      if (state.crmView.organization && !url.searchParams.has("list") && !url.searchParams.has("contact")) {
        url.searchParams.set("org", state.crmView.organization);
      }
    }
    history.replaceState(history.state, "", url);
  }

  function crmSetListParam(viewId) {
    crmSetLinkParam("list", viewId);
  }

  async function copyCrmListLink(view, button = null) {
    if (!view) return;
    closeCrmMenu();
    try {
      await navigator.clipboard.writeText(crmListUrl(view.id));
    } catch {
      crmMessage("error", "Could not copy the link.");
      return;
    }
    const reach = view.shared
      ? `It opens for the people ${view.owner || view.organization} shared it with.`
      : view.visibility === "private" && !(view.sharedWith || []).length
      ? "The list is private, so the link opens only for you until you share it from Rename or share…."
      : `It opens for everyone the list reaches: ${crmSheetShareLabel(view).replace(/^Shared with /, "")}.`;
    crmMessage("notice", `Link to “${view.name}” copied. ${reach}`);
    if (!button) return;
    button.textContent = "Link copied";
    setTimeout(() => { if (button.isConnected) button.textContent = "Copy link"; }, 2000);
  }

  async function crmResolveLink() {
    const params = new URL(location.href).searchParams;
    const id = params.get("list") || params.get("contact") || "";
    if (!/^[0-9a-f]{32}$/.test(id)) return null;
    try {
      return { id, ...(await crmRequest(`/api/crm/-/open/${id}`)) };
    } catch (error) { return { id, missing: true }; }
  }

  async function openCrmSharedOnly(listId) {
    try {
      state.crmView.sharedViews = (await crmRequest("/api/crm/-/shared")).views || [];
    } catch (error) { state.crmView.sharedViews = []; }
    if (!state.crmView.sharedViews.length) return false;
    state.crmView.sharedOnly = true; state.crmView.installed = true;
    state.crmView.organization = ""; state.crmView.section = "contacts";
    $(`[data-crm-install-panel]`)?.classList.add("hidden");
    $(`[data-crm-app]`)?.classList.remove("hidden");
    $$(`[data-crm-org-only]`).forEach((node) => { node.hidden = true; node.style.display = "none"; });
    $(`[data-crm-contact-new]`)?.classList.remove("hidden");
    crmMessage("error", "");
    renderCrm();
    const id = state.crmView.sharedViews.some((view) => view.id === listId) ? listId : state.crmView.sharedViews[0].id;
    await selectCrmSharedSheet(id);
    return true;
  }

  function openCrmListFromUrl() {
    const id = new URL(location.href).searchParams.get("list") || "";
    if (!id || !state.crmView.installed) return;
    if (crmTabViews().some((view) => view.id === id)) { selectCrmSheet(id); return; }
    if (state.crmView.sharedViews.some((view) => view.id === id)) { void selectCrmSharedSheet(id); return; }
    crmSetListParam("");
    crmMessage("error", `That link points to a list you cannot open in ${state.crmView.organization}. It may be private, deleted, or in an organization you are not in.`);
  }

  function openCrmSheetDialog(view = null) {
    const dialog = $(`[data-crm-sheet-dialog]`); if (!dialog) return;
    closeCrmMenu();
    $(`[data-crm-sheet-dialog-title]`).textContent = view ? "List settings" : "New list";
    $(`[data-crm-sheet-id]`).value = view?.id || "";
    $(`[data-crm-sheet-name]`).value = view?.name || "";
    const flow = $(`[data-crm-sheet-flow]`);
    if (flow) {
      const open = crmActiveShared() ? "" : crmActiveView()?.flow || "";
      flow.innerHTML = `<option value="">No pipeline (plain list)</option>` + crmFlows().map((item) =>
        `<option value="${escapeHtml(item.id)}">${escapeHtml(item.name)}</option>`).join("");
      flow.value = crmFlows().some((item) => item.id === open) ? open : "";
      flow.disabled = !state.crmView.canManageSettings;
      flow.title = state.crmView.canManageSettings ? "" : "Only organization owners and admins can put a list on a pipeline";
      $(`[data-crm-sheet-flow-row]`)?.classList.toggle("hidden", Boolean(view));
    }
    // Everyone the sheet reaches may rename it and shape its columns; who else
    // can open it stays with its owner and the organization's owners/admins.
    const manages = !view || Boolean(view.canManage);
    const visibility = $(`[data-crm-sheet-visibility]`);
    visibility.value = view?.visibility === "private" ? "private" : "organization";
    visibility.disabled = !manages;
    const shared = new Set(view?.sharedWith || []);
    $(`[data-crm-sheet-members]`).innerHTML = state.crmView.members.length
      ? state.crmView.members.map((name) => `<label class="flex items-center gap-2 text-xs text-foreground"><input data-crm-sheet-member type="checkbox" value="${escapeHtml(name)}"${shared.has(name) ? " checked" : ""}${manages ? "" : " disabled"} />${escapeHtml(name)}</label>`).join("")
      : `<p class="text-xs text-muted-foreground">No other organization members yet.</p>`;
    // Names outside the roster are ForkMesh users in other organizations.
    const users = $(`[data-crm-sheet-users]`);
    users.value = [...shared].filter((name) => !state.crmView.members.includes(name)).join(", ");
    users.disabled = !manages;
    const editable = $(`[data-crm-sheet-share-edit]`);
    if (editable) { editable.checked = view?.shareAccess === "edit"; editable.disabled = !manages; }
    $(`[data-crm-sheet-delete]`)?.classList.toggle("hidden", !crmCanDeleteList(view));
    const link = $(`[data-crm-sheet-link]`);
    if (link) { link.classList.toggle("hidden", !view); link.textContent = "Copy link"; }
    renderCrmSheetShare();
    dialog.showModal();
  }

  async function saveCrmSheet(event) {
    event.preventDefault();
    const id = $(`[data-crm-sheet-id]`).value;
    const body = {
      name: $(`[data-crm-sheet-name]`).value,
      visibility: $(`[data-crm-sheet-visibility]`).value,
      sharedWith: [
        ...$$(`[data-crm-sheet-member]`).filter((box) => box.checked).map((box) => box.value),
        ...String($(`[data-crm-sheet-users]`)?.value || "").split(/[\s,;]+/)
          .map((name) => name.trim().replace(/^@/, "").toLowerCase()).filter(Boolean),
      ],
      shareAccess: $(`[data-crm-sheet-share-edit]`)?.checked ? "edit" : "view",
    };
    const flow = String($(`[data-crm-sheet-flow]`)?.value || "");
    if (!id && flow && state.crmView.canManageSettings) body.flow = flow;
    try {
      const payload = await crmRequest(
        crmOrganizationPath(`/views${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
      $(`[data-crm-sheet-dialog]`).close();
      if (payload.view?.id) { state.crmView.activeView = payload.view.id; state.crmView.activeShared = ""; }
      crmMessage("notice", id ? "List updated." : `List “${payload.view?.name || body.name}” created.`);
      await loadCrm();
    } catch (error) { crmMessage("error", `Could not save the list: ${error.message}`); }
  }

  async function deleteCrmSheet(view) {
    if (!crmCanDeleteList(view)) return;
    closeCrmMenu();
    if (!confirm(`Delete the “${view.name}” list? Its contacts stay in the organization and on their pipelines.`)) return;
    try {
      await crmRequest(crmOrganizationPath(`/views/${view.id}`), "DELETE", {});
      if (state.crmView.activeView === view.id) { state.crmView.activeView = ""; crmSetListParam(""); }
      $(`[data-crm-sheet-dialog]`)?.close();
      crmMessage("notice", `List “${view.name}” deleted.`);
      await loadCrm();
    } catch (error) { crmMessage("error", `The “${view.name}” list was not deleted: ${error.message}`); }
  }

  function openCrmMembersDialog(view) {
    const dialog = $(`[data-crm-members-dialog]`);
    if (!dialog || !view || view.id === CRM_NO_PIPELINE_TAB) return;
    closeCrmMenu();
    state.crmView.membersView = view.id;
    const owner = String(view.owner || "").toLowerCase();
    const shared = new Set(view.sharedWith || []);
    $(`[data-crm-members-intro]`).textContent = `Share “${view.name}” with more people. ${view.visibility === "private"
      ? "Only the people it is shared with can open it."
      : `Everyone in ${state.crmView.organization} can already open it; people added here keep it if it is made private.`}`;
    const roster = state.crmView.members.filter((name) => name && name !== owner && !shared.has(name));
    $(`[data-crm-members-roster]`).innerHTML = roster.length
      ? roster.map((name) => `<label data-crm-members-person="${escapeHtml(name)}" class="flex items-center gap-2 text-xs text-foreground"><input data-crm-members-pick type="checkbox" value="${escapeHtml(name)}" />${escapeHtml(name)}</label>`).join("")
      : `<p class="text-xs text-muted-foreground">Every organization member is already on it.</p>`;
    $(`[data-crm-members-search]`).value = "";
    $(`[data-crm-members-others]`).value = "";
    dialog.showModal();
  }

  function filterCrmMembersRoster() {
    const query = String($(`[data-crm-members-search]`)?.value || "").trim().toLowerCase();
    $$(`[data-crm-members-person]`).forEach((row) => {
      row.hidden = Boolean(query) && !row.dataset.crmMembersPerson.toLowerCase().includes(query);
    });
  }

  function crmMembersAdded(view, picked, typed) {
    const owner = String(view.owner || "").toLowerCase();
    const shared = view.sharedWith || [];
    const added = [...picked];
    for (const raw of String(typed || "").split(/[\s,;]+/)) {
      const person = raw.replace(/^@/, "").toLowerCase().slice(0, 64);
      if (person && person !== owner && !shared.includes(person) && !added.includes(person)) added.push(person);
    }
    return added;
  }

  async function saveCrmMembers(event) {
    event.preventDefault();
    const view = crmTabView(state.crmView.membersView);
    if (!view) return;
    const added = crmMembersAdded(view,
      $$(`[data-crm-members-pick]`).filter((box) => box.checked).map((box) => box.value),
      $(`[data-crm-members-others]`)?.value);
    $(`[data-crm-members-dialog]`)?.close();
    if (!added.length) return;
    const wanted = [...(view.sharedWith || []), ...added];
    let payload;
    try {
      payload = view.virtual
        ? await crmRequest(crmOrganizationPath("/views"), "POST", {
          name: view.name, flow: view.flow, visibility: "organization", sharedWith: wanted })
        : await crmRequest(crmOrganizationPath(`/views/${view.id}`), "PATCH", { sharedWith: wanted });
    } catch (error) {
      crmMessage("error", `Members were not added to “${view.name}”: ${error.message}`);
      await loadCrm();
      return;
    }
    const saved = payload.view || null;
    if (saved?.id) {
      const at = state.crmView.views.findIndex((item) => item.id === saved.id);
      if (at >= 0) state.crmView.views[at] = saved; else state.crmView.views.push(saved);
      if (view.virtual && state.crmView.activeView === view.id) state.crmView.activeView = saved.id;
      renderCrmContacts();
    }
    const kept = new Set(saved?.sharedWith || []);
    const missing = added.filter((person) => !kept.has(person));
    const count = added.length - missing.length;
    crmMessage(missing.length ? "error" : "notice",
      `${count === 1 ? `Added 1 member to “${view.name}”.` : `Added ${count} members to “${view.name}”.`}${missing.length ? ` No ForkMesh account: ${missing.join(", ")}.` : ""}`);
  }

  function crmInitials(contact) {
    const words = String(contact.name || String(contact.email || "").split("@")[0] || "?")
      .split(/[\s._-]+/).filter(Boolean);
    return [...(words[0] || "?")].slice(0, words.length > 1 ? 1 : 2).join("").toUpperCase()
      + (words.length > 1 ? [...words[words.length - 1]][0].toUpperCase() : "");
  }

  // Six muted tones, picked from the email (or name) so a person keeps the
  // same colour on every render and every teammate's screen.
  function crmAvatar(contact, size = "") {
    let hash = 0;
    for (const char of String(contact.email || contact.name || "")) hash = (hash * 31 + char.codePointAt(0)) % 9973;
    return `<span aria-hidden="true" class="crm-avatar crm-hue-${hash % 6 + 1}${size ? ` crm-avatar-${size}` : ""}">${escapeHtml(crmInitials(contact))}</span>`;
  }

  const CRM_PICTURE_CACHE_KEY = "forkmesh-crm-pictures";
  const CRM_PICTURE_CACHE_MS = 7 * 24 * 60 * 60 * 1000;
  const CRM_PICTURE_CACHE_LIMIT = 4000;
  const CRM_PICTURE_SIZE = 112;
  const CRM_PICTURES_IN_FLIGHT = 4;
  const CRM_FORKMESH_AVATARS_MS = 15 * 60 * 1000;
  const crmPictures = {
    known: null, queue: [], queued: new Set(), inFlight: 0, observer: null, saveTimer: 0,
    users: new Map(), usersList: [], usersAt: 0, usersRead: null, usersEmails: false,
  };

  function crmPictureSources(contact) {
    const sources = [];
    const email = String(contact?.email || "").trim().toLowerCase();
    if (email.includes("@") && crmPictures.users.has(email)) sources.push(`forkmesh:${email}`);
    const mastodon = crmMastodonKey(contact?.mastodon).toLowerCase();
    if (mastodon) sources.push(`mastodon:${mastodon}`);
    const bluesky = crmBlueskyActor(contact?.bluesky);
    if (bluesky) sources.push(`bluesky:${bluesky}`);
    const github = crmGithubUser(contact?.github).toLowerCase();
    if (github) sources.push(`github:${github}`);
    if (email.includes("@")) sources.push(`gravatar:${email}`);
    return sources;
  }

  function crmPictureCache() {
    if (crmPictures.known) return crmPictures.known;
    crmPictures.known = new Map();
    try {
      const saved = JSON.parse(localStorage.getItem(CRM_PICTURE_CACHE_KEY) || "{}");
      const now = Date.now();
      for (const [source, entry] of Object.entries(saved && typeof saved === "object" ? saved : {})) {
        const [url, at] = Array.isArray(entry) ? entry : [];
        if (typeof url === "string" && now - Number(at) < CRM_PICTURE_CACHE_MS && (!url || crmPictureUrl(url))) {
          crmPictures.known.set(source, { url, at: Number(at) });
        }
      }
    } catch { crmPictures.known.clear(); }
    return crmPictures.known;
  }

  function crmSavePictureCache() {
    if (crmPictures.saveTimer) return;
    crmPictures.saveTimer = setTimeout(() => {
      crmPictures.saveTimer = 0;
      const now = Date.now();
      const kept = [...crmPictureCache().entries()]
        .filter(([source, entry]) => !source.startsWith("forkmesh:") && now - entry.at < CRM_PICTURE_CACHE_MS)
        .sort((a, b) => b[1].at - a[1].at).slice(0, CRM_PICTURE_CACHE_LIMIT);
      try {
        localStorage.setItem(CRM_PICTURE_CACHE_KEY, JSON.stringify(Object.fromEntries(
          kept.map(([source, entry]) => [source, [entry.url, entry.at]]))));
      } catch { crmPictures.saveTimer = 0; }
    }, 1000);
  }

  function crmPictureFor(sources) {
    const known = crmPictureCache();
    const now = Date.now();
    for (const source of sources) {
      if (source.startsWith("forkmesh:")) {
        const url = crmPictures.users.get(source.slice(9));
        if (url) return { url, done: true, source };
        continue;
      }
      const entry = known.get(source);
      if (!entry || now - entry.at >= CRM_PICTURE_CACHE_MS) return { url: "", done: false, source };
      if (entry.url) return { url: entry.url, done: true, source };
    }
    return { url: "", done: true, source: "" };
  }

  function crmPicture(contact, size = "") {
    const sources = crmPictureSources(contact);
    if (!sources.length) return crmAvatar(contact, size);
    const picture = crmPictureFor(sources);
    return `<span aria-hidden="true" class="crm-pic${size ? ` crm-pic-${size}` : ""}" data-crm-picture="${escapeHtml(sources.join(" "))}"${picture.done ? "" : " data-crm-picture-wait"}>${crmAvatar(contact, size)}${picture.url ? `<img src="${escapeHtml(picture.url)}" alt="" draggable="false" referrerpolicy="no-referrer" />` : ""}</span>`;
  }

  function crmWatchPictures() {
    if (typeof document === "undefined") return;
    const waiting = $$(`[data-crm-picture-wait]`);
    crmPictures.observer?.disconnect();
    if (!waiting.length) return;
    if (typeof IntersectionObserver !== "function") {
      waiting.forEach((node) => crmQueuePictureSources(node.dataset.crmPicture.split(" ")));
      return;
    }
    crmPictures.observer ||= new IntersectionObserver((entries) => {
      for (const entry of entries) {
        if (!entry.isIntersecting) continue;
        crmPictures.observer.unobserve(entry.target);
        crmQueuePictureSources(String(entry.target.dataset.crmPicture || "").split(" "));
      }
    }, { rootMargin: "200px" });
    waiting.forEach((node) => crmPictures.observer.observe(node));
  }

  function crmQueuePictureSources(sources) {
    const picture = crmPictureFor(sources.filter(Boolean));
    if (picture.done || !picture.source || crmPictures.queued.has(picture.source)) return;
    crmPictures.queued.add(picture.source);
    crmPictures.queue.push(picture.source);
    crmPumpPictures();
  }

  function crmPumpPictures() {
    while (crmPictures.queue.length && crmPictures.inFlight < CRM_PICTURES_IN_FLIGHT) {
      const source = crmPictures.queue.shift();
      crmPictures.inFlight += 1;
      void crmReadPicture(source).catch(() => "").then((url) => {
        crmPictureCache().set(source, { url, at: Date.now() });
        crmPictures.inFlight -= 1;
        crmPictures.queued.delete(source);
        crmSavePictureCache();
        crmPictureLanded(source);
        crmPumpPictures();
      });
    }
  }

  function crmPictureLanded(source) {
    for (const node of $$(`[data-crm-picture]`)) {
      const sources = String(node.dataset.crmPicture || "").split(" ");
      if (!sources.includes(source)) continue;
      const picture = crmPictureFor(sources);
      if (!picture.done) { crmQueuePictureSources(sources); continue; }
      node.removeAttribute("data-crm-picture-wait");
      if (picture.url && !node.querySelector("img")) {
        const image = document.createElement("img");
        image.alt = ""; image.draggable = false; image.referrerPolicy = "no-referrer"; image.src = picture.url;
        node.appendChild(image);
      }
    }
    crmPeopleLater();
  }

  function crmImageLoads(url) {
    return new Promise((resolve) => {
      const image = new Image();
      const timer = setTimeout(() => resolve(false), CRM_UPDATE_TIMEOUT_MS);
      image.referrerPolicy = "no-referrer";
      image.onload = () => { clearTimeout(timer); resolve(image.naturalWidth > 0); };
      image.onerror = () => { clearTimeout(timer); resolve(false); };
      image.src = url;
    });
  }

  async function crmSha256Hex(text) {
    const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
    return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
  }

  async function crmReadPicture(source) {
    const split = source.indexOf(":");
    const kind = source.slice(0, split);
    const account = source.slice(split + 1);
    let url = "";
    if (kind === "github") {
      url = `https://avatars.githubusercontent.com/${encodeURIComponent(account)}?s=${CRM_PICTURE_SIZE}`;
    } else if (kind === "gravatar") {
      url = `https://gravatar.com/avatar/${await crmSha256Hex(account)}?s=${CRM_PICTURE_SIZE}&d=404`;
    } else if (kind === "mastodon") {
      const at = account.lastIndexOf("@");
      const profile = await crmReadPublicJson(
        `https://${account.slice(at + 1)}/api/v1/accounts/lookup?acct=${encodeURIComponent(account.slice(0, at))}`);
      url = crmPictureUrl(profile?.avatar_static || profile?.avatar);
    } else if (kind === "bluesky") {
      const profile = await crmReadPublicJson(
        `https://public.api.bsky.app/xrpc/app.bsky.actor.getProfile?actor=${encodeURIComponent(account)}`);
      url = crmPictureUrl(profile?.avatar);
    }
    return url && await crmImageLoads(url) ? url : "";
  }

  function crmAdmin() {
    return state.session?.isAdmin === true;
  }

  function crmForkMeshUsers(force = false) {
    if (!crmAdmin()) return Promise.resolve(null);
    if (!force && crmPictures.usersAt && Date.now() - crmPictures.usersAt < CRM_FORKMESH_AVATARS_MS) {
      return Promise.resolve(crmPictures.usersEmails ? crmPictures.usersList : null);
    }
    if (crmPictures.usersRead) return crmPictures.usersRead;
    crmPictures.usersRead = crmRequest("/api/accounts/users").then((payload) => {
      const users = Array.isArray(payload?.users) ? payload.users : [];
      const avatars = new Map();
      for (const user of users) {
        const email = String(user?.email || "").trim().toLowerCase();
        const png = String(user?.avatarPng || "").replace(/\s+/g, "");
        if (email.includes("@") && /^[A-Za-z0-9+/]+={0,2}$/.test(png)) avatars.set(email, `data:image/png;base64,${png}`);
      }
      crmPictures.users = avatars;
      crmPictures.usersList = users;
      crmPictures.usersEmails = users.some((user) => String(user?.email || "").includes("@"));
      crmPictures.usersAt = Date.now();
      return crmPictures.usersEmails ? users : null;
    }, (error) => {
      crmPictures.usersAt = Date.now();
      throw error;
    }).finally(() => { crmPictures.usersRead = null; });
    return crmPictures.usersRead;
  }

  function crmRefreshForkMeshAvatars() {
    if (!crmAdmin() || crmPictures.usersRead
      || (crmPictures.usersAt && Date.now() - crmPictures.usersAt < CRM_FORKMESH_AVATARS_MS)) return;
    void crmForkMeshUsers().then(() => { renderCrmContacts(); renderCrmUsersImport(); }, () => {});
  }

  function crmStageTone(contact) {
    return crmToneOf(contact.flow, contact.stage);
  }

  function crmToneOf(flowId, value) {
    const stage = String(value || "");
    if (CRM_WON_STAGES.has(stage)) return "won";
    if (CRM_CLOSED_STAGES.has(stage)) return "closed";
    return (crmFlow(flowId).stages || []).indexOf(stage) > 0 ? "open" : "new";
  }

  function crmDaysSince(ms) {
    return Number(ms) ? Math.max(0, Math.floor((Date.now() - Number(ms)) / 86400000)) : null;
  }

  // Five dots for how recently anyone spoke to the contact: easier to scan
  // down a column than a stack of "n days ago" labels.
  function crmWarmth(ms) {
    const days = crmDaysSince(ms);
    const level = days === null ? 0 : days < 1 ? 5 : days < 3 ? 4 : days < 7 ? 3 : days < CRM_QUIET_DAYS ? 2 : 1;
    return `<span class="crm-warmth" role="img" aria-label="Warmth ${level} of 5">${[1, 2, 3, 4, 5].map((dot) => `<i${dot <= level ? ' class="on"' : ""}></i>`).join("")}</span>`;
  }

  function crmQuietTag(contact) {
    const days = crmDaysSince(contact.lastContactedAt);
    return days !== null && days >= CRM_QUIET_DAYS && !CRM_CLOSED_STAGES.has(String(contact.stage || ""))
      ? `<span class="crm-quiet" title="No contact for ${days} days">Quiet ${days}d</span>` : "";
  }

  // A fediverse handle is name@instance and its profile lives on the instance,
  // so "@grace@mastodon.social" opens https://mastodon.social/@grace. A profile
  // path typed without its scheme (mastodon.social/@grace) keeps its order.
  function crmMastodonUrl(text) {
    const host = "([a-z0-9-]{1,63}(?:\\.[a-z0-9-]{1,63})+)";
    const value = String(text || "").trim().replace(/\/+$/, "");
    const path = value.match(new RegExp(`^${host}/@([\\w.-]{1,64})(?:@${host})?$`, "i"));
    if (path) return `https://${path[1]}/@${path[2]}${path[3] ? `@${path[3]}` : ""}`;
    const handle = value.match(new RegExp(`^@?([\\w.-]{1,64})@${host}$`, "i"));
    return handle ? `https://${handle[2]}/@${handle[1]}` : "";
  }

  // Handles are stored as typed (@name, name@instance, a bare domain). Only an
  // https URL built from them becomes a link.
  function crmProfileUrl(field, value) {
    const text = String(value || "").trim();
    if (!text) return "";
    const handle = text.replace(/^@/, "");
    let url = /^https:\/\//i.test(text) ? text : /^http:/i.test(text) ? "" : ({
      twitter: () => `https://x.com/${encodeURIComponent(handle)}`,
      mastodon: () => crmMastodonUrl(text),
      bluesky: () => `https://bsky.app/profile/${encodeURIComponent(handle)}`,
      linkedin: () => /linkedin\.com\//i.test(text) ? `https://${text.replace(/^(www\.)?/i, "www.")}` : `https://www.linkedin.com/in/${encodeURIComponent(handle)}`,
      github: () => `https://github.com/${encodeURIComponent(handle)}`,
      website: () => `https://${text}`,
    }[field]?.() || "");
    try { url = new URL(url).protocol === "https:" ? new URL(url).href : ""; } catch { url = ""; }
    return url;
  }

  // Profile handles whose columns are not on screen, as small links.
  function crmProfileTiles(contact, columns) {
    if (crmActiveShared()) return "";
    const shown = new Set(columns.map((column) => column.field));
    return CRM_PROFILE_FIELDS.filter((field) => contact[field] && !shown.has(field)).map((field) => {
      const url = crmProfileUrl(field, contact[field]);
      const title = escapeHtml(`${crmColumnLabel(field)}: ${contact[field]}`);
      return url
        ? `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer" title="${title}" class="crm-tile">${CRM_PROFILE_BADGES[field]}</a>`
        : `<span title="${title}" class="crm-tile">${CRM_PROFILE_BADGES[field]}</span>`;
    }).join("");
  }

  function crmLastContacted(contact) {
    const text = contact.lastContactedAt ? relativeTimeLabel(contact.lastContactedAt) : "Never";
    return `<span class="flex items-center whitespace-nowrap text-xs text-muted-foreground" title="${escapeHtml(text)}">${crmWarmth(contact.lastContactedAt)}${escapeHtml(text)}${crmQuietTag(contact)}</span>`;
  }

  function crmProviderLabel(email) {
    return { gmail: "Gmail", zoho: "Zoho Mail", mastodon: "Mastodon" }[email?.provider]
      || (email?.transport === "mailtrap" ? "Mailtrap" : "Email");
  }

  function crmLinkedEmails(contactId) {
    return state.crmView.emails.filter((email) => email.contactId === contactId);
  }

  // The messages tied to a contact, newest first: each chip reopens the
  // message in the mailbox it was linked from.
  // The chips beside the name on a single-line table row; the mobile cards
  // stack them under it (crmEmailChips).
  function crmEmailChipsInline(contact) {
    const chips = crmEmailChipMarkup(contact);
    return chips ? `<span class="inline-flex shrink-0 flex-nowrap items-center gap-1 whitespace-nowrap">${chips}</span>` : "";
  }

  function crmEmailChips(contact) {
    const chips = crmEmailChipMarkup(contact);
    return chips ? `<div class="mt-1 flex flex-wrap items-center gap-1">${chips}</div>` : "";
  }

  function crmEmailChipMarkup(contact) {
    if (crmActiveShared()) return "";
    const emails = crmLinkedEmails(contact.id);
    if (!emails.length) return "";
    const chip = (email) => {
      const label = `${email.direction === "inbound" ? "↙" : "↗"} ${crmProviderLabel(email)}`;
      const title = `${email.subject || "No subject"} · ${email.createdAt ? relativeTimeLabel(email.sentAt || email.createdAt) : ""}`;
      return email.providerUrl && String(email.providerUrl).startsWith("https://")
        ? `<a data-crm-email-open href="${escapeHtml(email.providerUrl)}" target="_blank" rel="noopener noreferrer" title="${escapeHtml(title)}" class="rounded-full border border-border px-1.5 text-[10px] text-primary hover:underline">${escapeHtml(label)}</a>`
        : `<span title="${escapeHtml(title)}" class="rounded-full border border-border px-1.5 text-[10px] text-muted-foreground">${escapeHtml(label)}</span>`;
    };
    const shown = emails.slice(0, 3).map(chip).join("");
    const more = emails.length > 3 ? `<span class="text-[10px] text-muted-foreground">+${emails.length - 3}</span>` : "";
    return `${shown}${more}`;
  }

  // Everything under a contact's name: linked mail, then hidden profiles.
  function crmNameMeta(contact, columns) {
    const meta = crmEmailChips(contact) + crmProfileTiles(contact, columns) + crmDeckChip(contact) + crmAccountChip(contact);
    return meta ? `<div class="mt-1 flex flex-wrap items-center gap-1">${meta}</div>` : "";
  }

  function crmDay(ms) {
    return ms ? new Date(Number(ms)).toISOString().slice(0, 10) : "";
  }

  function crmAccountProof(contact) {
    const account = contact.account || {};
    if (!account.account) return "";
    const tier = (window) => window?.tier === "pro" ? "Pro" : "Supporter";
    const parts = [`ForkMesh account ${account.account}, signed up ${crmDay(account.signedUpAt) || "at an unknown date"}`];
    if (account.paid) parts.push(`${tier(account.paid)} membership paid since ${crmDay(account.paid.since)}, ${account.paid.active ? "runs" : "ended"} ${crmDay(account.paid.until)}`);
    if (account.trial) parts.push(`trial ${crmDay(account.trial.startedAt)} to ${crmDay(account.trial.endsAt)}`);
    if (account.promotion) parts.push(`${tier(account.promotion)} prize membership until ${crmDay(account.promotion.until)}`);
    return parts.join("; ");
  }

  function crmAccountChip(contact) {
    const account = contact.account || {};
    if (!account.account) return "";
    const label = account.paid?.active ? `${account.paid.tier === "pro" ? "Pro" : "Supporter"} member`
      : account.trial?.active || account.promotion?.active ? "On trial"
      : account.paid ? `Membership ended ${crmDay(account.paid.until)}`
      : `Signed up ${crmDay(account.signedUpAt)}`;
    const title = `${crmAccountProof(contact)}. Checked ${crmDay(account.recordedAt)}.`;
    const advance = account.suggestedStage && account.suggestedStage !== contact.stage
      ? `<button type="button" data-crm-account-advance="${escapeHtml(contact.id)}" title="Move to ${escapeHtml(account.suggestedStage)} and note the evidence" class="rounded-full border border-primary px-2 py-0.5 text-[10px] font-semibold text-primary hover:bg-secondary">→ ${escapeHtml(account.suggestedStage)}</button>`
      : "";
    return `<span class="rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold ${account.paid?.active ? "text-primary" : "text-muted-foreground"}" title="${escapeHtml(title)}">${escapeHtml(label)}</span>${advance}`;
  }

  async function advanceCrmFromAccount(contactId) {
    const contact = crmContactById(contactId);
    const stage = contact?.account?.suggestedStage;
    if (!contact || !stage) return;
    const proof = crmAccountProof(contact);
    if (!confirm(`Move ${contact.name || contact.email || "this contact"} to ${stage}?\n\nEvidence: ${proof}.`)) return;
    const line = `[${crmDay(Date.now())} ${state.session?.name || "member"}] Moved to ${stage}: ${proof} (ForkMesh account check of ${crmDay(contact.account.recordedAt)}).`;
    const notes = String(contact.notes || "").trim();
    try {
      await crmRequest(crmOrganizationPath(`/contacts/${contact.id}`), "PATCH", { stage, notes: notes ? `${notes}\n\n${line}` : line });
      crmMessage("notice", `${contact.name || contact.email} moved to ${stage}.`);
    } catch (error) { crmMessage("error", `Could not move the contact: ${error.message}`); }
    await loadCrm();
  }

  async function checkCrmAccounts() {
    closeCrmMenu();
    crmMessage("notice", "Checking contacts against ForkMesh accounts…");
    try {
      const result = await crmRequest(crmOrganizationPath("/contacts/evidence"), "POST", {});
      crmMessage("notice", `Checked ${result.checked} contacts: ${result.accounts} have a ForkMesh account, ${result.paying} pay, ${result.trial} are on a trial, and ${result.ahead} sit in an earlier stage than their account shows (filter: Stage behind evidence).`);
    } catch (error) {
      crmMessage("error", error.payload?.error === "forkmesh_admin_required"
        ? "Only a ForkMesh administrator who owns or administers this organization can check contacts against ForkMesh accounts."
        : `Could not check accounts: ${error.message}`);
      return;
    }
    await loadCrm();
  }

  // --- investor deck links ---------------------------------------------------
  // Each contact can hold one private link to the deck; the page it reports
  // views to is served by the platform, so the dialog shows who opened it,
  // for how long, and which pages held their attention.

  function crmDeckFor(contactId) {
    return (state.crmView.decks || []).find((item) => item.contactId === contactId && item.active) || null;
  }

  function crmDuration(seconds) {
    const total = Math.max(0, Math.round(Number(seconds) || 0));
    if (total < 60) return `${total}s`;
    const minutes = Math.floor(total / 60);
    if (minutes < 60) return `${minutes}m ${total % 60}s`;
    return `${Math.floor(minutes / 60)}h ${minutes % 60}m`;
  }

  function crmDeckChip(contact) {
    const link = crmDeckFor(contact.id);
    if (!link) return "";
    const label = link.views
      ? `Deck · ${link.views} view${link.views === 1 ? "" : "s"} · ${crmDuration(link.seconds)}`
      : "Deck · not opened yet";
    return `<button type="button" data-crm-contact-deck="${escapeHtml(contact.id)}" class="crm-deck-chip${link.views ? " crm-deck-chip-warm" : ""}" title="${link.views ? `Last opened ${escapeHtml(relativeTimeLabel(link.lastViewedAt))}` : "The link has not been opened"}">${escapeHtml(label)}</button>`;
  }

  async function openCrmDeck(contactId) {
    const contact = crmContactById(contactId);
    const dialog = $(`[data-crm-deck-dialog]`);
    if (!contact || !dialog || crmActiveShared()) return;
    if (!crmInvestorFlow(contact.flow) && !crmDeckFor(contactId)) {
      crmMessage("error", `${contact.name || contact.email || "This contact"} is in ${crmFlow(contact.flow).name}, not an investor pipeline. Move them to an investor pipeline first if they should see the deck.`);
      return;
    }
    state.crmView.deckContactId = contactId;
    $(`[data-crm-deck-title]`).textContent = `Pitch deck for ${contact.name || contact.email || "this contact"}`;
    const body = $(`[data-crm-deck-body]`);
    body.innerHTML = `<p class="text-sm text-muted-foreground">Preparing the link…</p>`;
    if (!dialog.open) dialog.showModal();
    try {
      let link = crmDeckFor(contactId);
      if (!link) {
        const payload = await crmRequest(crmOrganizationPath("/decks"), "POST", { contactId });
        link = payload.link;
        state.crmView.decks = [link, ...(state.crmView.decks || []).filter((item) => item.token !== link.token)];
        renderCrmContacts();
      }
      const detail = (await crmRequest(crmOrganizationPath(`/decks/${link.token}`))).deck;
      renderCrmDeck(contact, detail);
    } catch (error) {
      body.innerHTML = `<p class="text-sm text-destructive">${escapeHtml(error.payload?.detail || error.message)}</p>`;
    }
  }

  function crmInvestorFlow(flowId) {
    const flow = crmFlow(flowId);
    return flow.icon === "investors" || ["investors", "investment"].includes(flow.id);
  }

  function renderCrmDeck(contact, deck) {
    const body = $(`[data-crm-deck-body]`);
    if (!body || !deck) return;
    const maxSeconds = Math.max(1, ...deck.pages.map((page) => page.seconds));
    const pages = deck.pages.map((page) => `
      <div class="crm-deck-bar" title="${page.viewers} viewer${page.viewers === 1 ? "" : "s"}">
        <span class="crm-deck-bar-label"><span class="text-muted-foreground">${page.page}.</span> ${escapeHtml(page.title)}</span>
        <span class="crm-deck-bar-track"><span class="crm-deck-bar-fill" style="width:${Math.round((page.seconds / maxSeconds) * 100)}%"></span></span>
        <span class="crm-deck-bar-value">${page.seconds ? crmDuration(page.seconds) : "–"}</span>
      </div>`).join("");
    const sessions = deck.sessions.length
      ? deck.sessions.map((visit) => `<tr><td class="py-1.5 pr-3">${escapeHtml(relativeTimeLabel(visit.startedAt))}</td><td class="py-1.5 pr-3">${crmDuration(visit.seconds)}</td><td class="py-1.5 pr-3">${visit.pages} of ${deck.pageCount}</td><td class="py-1.5 text-muted-foreground">${escapeHtml([visit.country, visit.agent].filter(Boolean).join(" · "))}</td></tr>`).join("")
      : `<tr><td colspan="4" class="py-2 text-muted-foreground">Nobody has opened this link yet.</td></tr>`;
    body.innerHTML = `
      <div class="grid gap-4">
        <div class="grid gap-2 rounded-md border border-border bg-background p-3">
          <span class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">Private link · only ${escapeHtml(contact.name || "this contact")} should have it</span>
          <div class="flex flex-wrap items-center gap-2">
            <input data-crm-deck-url readonly value="${escapeHtml(deck.url)}" aria-label="Deck link" class="min-w-0 flex-1 rounded-md border border-border bg-card px-3 py-2 font-mono text-xs text-foreground" />
            <button type="button" data-crm-deck-copy class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Copy</button>
            <a href="${escapeHtml(deck.url)}" target="_blank" rel="noopener noreferrer" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Open</a>
            <button type="button" data-crm-deck-email class="rounded-md bg-primary px-3 py-2 text-xs font-semibold text-primary-foreground hover:opacity-90">Email the link</button>
          </div>
          <p class="text-xs text-muted-foreground">Created ${escapeHtml(relativeTimeLabel(deck.createdAt))} by ${escapeHtml(deck.createdBy || "a teammate")}. Every open is recorded against this contact${deck.active ? "" : " (link revoked)"}.</p>
        </div>
        <div class="grid grid-cols-2 gap-2 sm:grid-cols-4">
          <div class="rounded-md border border-border p-3"><span class="block text-xl font-semibold text-foreground">${deck.views}</span><span class="text-xs uppercase tracking-wide text-muted-foreground">Opens</span></div>
          <div class="rounded-md border border-border p-3"><span class="block text-xl font-semibold text-foreground">${crmDuration(deck.seconds)}</span><span class="text-xs uppercase tracking-wide text-muted-foreground">Total time</span></div>
          <div class="rounded-md border border-border p-3"><span class="block text-xl font-semibold text-foreground">${deck.pagesViewed} / ${deck.pageCount}</span><span class="text-xs uppercase tracking-wide text-muted-foreground">Pages seen</span></div>
          <div class="rounded-md border border-border p-3"><span class="block truncate text-xl font-semibold text-foreground">${deck.lastViewedAt ? escapeHtml(relativeTimeLabel(deck.lastViewedAt)) : "–"}</span><span class="text-xs uppercase tracking-wide text-muted-foreground">Last opened</span></div>
        </div>
        <div>
          <h3 class="mb-2 text-xs font-semibold uppercase tracking-wide text-muted-foreground">Time on each page</h3>
          <div class="grid gap-1.5">${pages}</div>
        </div>
        <div>
          <h3 class="mb-2 text-xs font-semibold uppercase tracking-wide text-muted-foreground">Visits</h3>
          <div class="overflow-x-auto"><table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="py-1 pr-3 font-semibold">When</th><th class="py-1 pr-3 font-semibold">Time</th><th class="py-1 pr-3 font-semibold">Pages</th><th class="py-1 font-semibold">Where</th></tr></thead><tbody data-crm-deck-sessions>${sessions}</tbody></table></div>
        </div>
        <div class="flex flex-wrap items-center justify-between gap-2 border-t border-border pt-3">
          <p class="text-xs text-muted-foreground">Revoking stops this link working; a new one can be minted afterwards.</p>
          ${deck.active
            ? `<button type="button" data-crm-deck-revoke="${escapeHtml(deck.token)}" class="rounded-md border border-destructive px-3 py-2 text-xs font-semibold text-destructive hover:bg-secondary">Revoke link</button>`
            : `<button type="button" data-crm-deck-new class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Mint a new link</button>`}
        </div>
      </div>`;
  }

  async function revokeCrmDeck(token) {
    const contact = crmContactById(state.crmView.deckContactId);
    if (!contact || !window.confirm(`Revoke the deck link for ${contact.name || contact.email || "this contact"}? Anyone holding it loses access.`)) return;
    try {
      await crmRequest(crmOrganizationPath(`/decks/${token}`), "DELETE", {});
      state.crmView.decks = (state.crmView.decks || []).map((item) => item.token === token ? { ...item, active: false, revokedAt: Date.now() } : item);
      renderCrmContacts();
      const detail = (await crmRequest(crmOrganizationPath(`/decks/${token}`))).deck;
      renderCrmDeck(contact, detail);
      crmMessage("notice", "Deck link revoked.");
    } catch (error) { crmMessage("error", `Could not revoke the link: ${error.message}`); }
  }

  function emailCrmDeck() {
    const contact = crmContactById(state.crmView.deckContactId);
    const link = crmDeckFor(state.crmView.deckContactId);
    if (!contact || !link) return;
    $(`[data-crm-deck-dialog]`)?.close();
    openCrmEmail(contact);
    const first = String(contact.name || "").trim().split(" ")[0];
    $(`[data-crm-email-subject]`).value = "ForkMesh investor deck";
    $(`[data-crm-email-text]`).value = `Hi${first ? " " + first : ""},\n\nHere is the ForkMesh investor deck, prepared for you:\n${link.url}\n\nIt is a private link, so please do not forward it. Happy to walk through any page on a call.\n\nBest,\n${state.session?.name || "The ForkMesh founders"}`;
  }

  // --- the contact detail card ---------------------------------------------
  // A contact's picture or name opens everything the organization knows about
  // that person in one place: every field, still edited through the same
  // inline path the table saves through, the record's own history, the
  // messages linked to it, and the newest posts on the accounts it names.
  // The posts and the picture are read from those services by this browser
  // when the card opens, and nothing about them is stored.

  const CRM_UPDATE_LIMIT = 5;
  const CRM_UPDATE_TIMEOUT_MS = 8000;
  // The services a record can be read live from, in the order the desktop
  // tries them for a picture: the instance the contact names, then Bluesky's
  // public API, then GitHub.
  const CRM_LIVE_SERVICES = ["mastodon", "bluesky", "github"];

  function crmContactLabel(contact) {
    return String(contact?.name || contact?.email || "Unnamed contact");
  }

  // "@grace@mastodon.social", "grace@mastodon.social" or the profile URL, as
  // the "user@host" pair an instance is asked about (crmMastodonAccount, the
  // followers import's reader, decides what a handle names).
  function crmMastodonKey(value) {
    const account = crmMastodonAccount(value);
    return account ? `${account.user}@${account.host}` : "";
  }

  // A Bluesky handle, a bsky.app profile link, or the account's DID.
  function crmBlueskyActor(value) {
    const link = String(value || "").match(/bsky\.app\/profile\/([^/?#\s]+)/i);
    const actor = (link ? link[1] : String(value || "")).trim().replace(/^@/, "").toLowerCase();
    const shape = /^(?:(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z][a-z0-9-]*[a-z0-9]|did:[a-z]+:[a-z0-9._:%-]+)$/;
    return shape.test(actor) ? actor : "";
  }

  // A GitHub username, "@username", or a github.com profile link.
  function crmGithubUser(value) {
    const link = String(value || "").match(/github\.com\/([^/?#\s]+)/i);
    const user = (link ? link[1] : String(value || "")).trim().replace(/^@/, "");
    return /^[A-Za-z0-9][A-Za-z0-9-]{0,38}$/.test(user) ? user : "";
  }

  // The accounts on a record that can be read live, best first. GitHub serves
  // a picture by username, so that one is known before anything is read.
  function crmLiveAccounts(contact) {
    return CRM_LIVE_SERVICES.map((service) => {
      const key = {
        mastodon: crmMastodonKey, bluesky: crmBlueskyActor, github: crmGithubUser,
      }[service](contact[service]);
      if (!key) return null;
      return {
        service, key, label: crmColumnLabel(service),
        handle: service === "bluesky" ? key : `@${key}`,
        url: crmProfileUrl(service, contact[service]),
        picture: service === "github"
          ? `https://avatars.githubusercontent.com/${encodeURIComponent(key)}?s=192` : "",
      };
    }).filter(Boolean);
  }

  function crmHttpsUrl(value) {
    try {
      const url = new URL(String(value || ""));
      return url.protocol === "https:" ? url.href : "";
    } catch { return ""; }
  }

  // A picture the card can show: https only, and never the placeholder a
  // Mastodon instance hands out for an account without one.
  function crmPictureUrl(value) {
    const url = crmHttpsUrl(value);
    return url.endsWith("/missing.png") ? "" : url;
  }

  // A post as one line: its whitespace collapsed and, past the width a card
  // shows, cut with an ellipsis. A Mastodon status carries HTML, which
  // crmMastodonText reduces to its words first.
  function crmPlainText(value, limit = 220) {
    const flat = String(value || "").replace(/\s+/g, " ").trim();
    return flat.length > limit ? `${flat.slice(0, limit - 1)}…` : flat;
  }

  // Public, unauthenticated reads of another service, made by the browser the
  // card is open in. No credentials and no referrer go with them. The
  // followers import has crmMastodonFetch instead: it pages through one
  // instance with the Link header and names that instance in its errors,
  // where a card reads three different services once each.
  async function crmReadPublicJson(url) {
    const response = await fetch(url, {
      headers: { Accept: "application/json" },
      credentials: "omit", referrerPolicy: "no-referrer", cache: "no-store",
      signal: AbortSignal.timeout?.(CRM_UPDATE_TIMEOUT_MS),
    });
    if (!response.ok) {
      const error = new Error(`the service answered ${response.status}`);
      error.status = response.status;
      error.retryAfter = crmRetryAfterMs(response.headers);
      throw error;
    }
    return response.json();
  }

  // Two calls against the contact's own instance: the account lookup, which
  // also names the picture, then that account's newest original posts.
  function crmRetryAfterMs(headers, now = Date.now()) {
    const retry = String(headers?.get?.("Retry-After") || "").trim();
    if (/^\d+$/.test(retry)) return Number(retry) * 1000;
    const reset = Date.parse(retry || String(headers?.get?.("X-RateLimit-Reset") || ""));
    return Number.isFinite(reset) && reset > now ? reset - now : 0;
  }

  async function crmMastodonUpdates(account, limit = CRM_UPDATE_LIMIT) {
    const at = account.key.lastIndexOf("@");
    const [user, host] = [account.key.slice(0, at), account.key.slice(at + 1)];
    const known = crmMastodonLatest.ids.get(account.key.toLowerCase());
    const profile = known && account.reuseId ? { id: known } : await crmReadPublicJson(
      `https://${host}/api/v1/accounts/lookup?acct=${encodeURIComponent(user)}`);
    if (!profile?.id) throw new Error("the instance knows no such account");
    crmMastodonLatest.ids.set(account.key.toLowerCase(), String(profile.id));
    const statuses = await crmReadPublicJson(
      `https://${host}/api/v1/accounts/${encodeURIComponent(profile.id)}/statuses`
      + `?limit=${limit}&exclude_replies=true&exclude_reblogs=true`);
    return {
      picture: crmPictureUrl(profile.avatar_static || profile.avatar),
      posts: (Array.isArray(statuses) ? statuses : []).map((status) => ({
        at: Date.parse(status?.created_at) || 0,
        text: crmPlainText(crmMastodonText(status?.content)) || "A post with no text",
        html: String(status?.content || ""),
        url: crmHttpsUrl(status?.url || status?.uri),
      })),
    };
  }

  // Bluesky's public API: the profile for the picture, then the author feed
  // without the replies.
  async function crmBlueskyUpdates(account) {
    const actor = encodeURIComponent(account.key);
    const profile = await crmReadPublicJson(
      `https://public.api.bsky.app/xrpc/app.bsky.actor.getProfile?actor=${actor}`);
    const feed = await crmReadPublicJson(
      `https://public.api.bsky.app/xrpc/app.bsky.feed.getAuthorFeed?actor=${actor}`
      + `&limit=${CRM_UPDATE_LIMIT}&filter=posts_no_replies`);
    const handle = encodeURIComponent(profile?.handle || account.key);
    return {
      picture: crmPictureUrl(profile?.avatar),
      posts: (Array.isArray(feed?.feed) ? feed.feed : []).map((item) => {
        const rkey = String(item?.post?.uri || "").split("/").pop();
        return {
          at: Date.parse(item?.post?.record?.createdAt) || 0,
          text: crmPlainText(item?.post?.record?.text) || "A post with no text",
          url: rkey ? `https://bsky.app/profile/${handle}/post/${encodeURIComponent(rkey)}` : "",
        };
      }),
    };
  }

  // The two verbs that take the repository as their object; everything
  // else happened "in" it.
  const CRM_GITHUB_DIRECT = new Set(["starred", "forked"]);

  // A public GitHub event as one line: what the account did, and where.
  function crmGithubEventLine(event) {
    const payload = event?.payload || {};
    const commits = Array.isArray(payload.commits) ? payload.commits.length : 0;
    const pushed = payload.size || commits || 1;
    // An event that names no number still reads as a sentence.
    const ref = (value) => (Number(value) > 0 ? ` #${Number(value)}` : "");
    const what = {
      PushEvent: () => `pushed ${pushed} commit${pushed === 1 ? "" : "s"}`,
      PullRequestEvent: () => `${payload.action || "updated"} pull request${ref(payload.number || payload.pull_request?.number)}`,
      PullRequestReviewEvent: () => `reviewed pull request${ref(payload.pull_request?.number)}`,
      PullRequestReviewCommentEvent: () => `commented on pull request${ref(payload.pull_request?.number)}`,
      IssuesEvent: () => `${payload.action || "updated"} issue${ref(payload.issue?.number)}`,
      IssueCommentEvent: () => `commented on issue${ref(payload.issue?.number)}`,
      WatchEvent: () => "starred",
      ForkEvent: () => "forked",
      CreateEvent: () => `created ${payload.ref_type || "a branch"}${payload.ref ? ` ${payload.ref}` : ""}`,
      DeleteEvent: () => `deleted ${payload.ref_type || "a ref"}${payload.ref ? ` ${payload.ref}` : ""}`,
      ReleaseEvent: () => `released ${payload.release?.tag_name || "a version"}`,
      PublicEvent: () => "made public",
      GollumEvent: () => "edited the wiki",
    }[String(event?.type || "")]?.()
      || String(event?.type || "Event").replace(/Event$/, "").toLowerCase();
    const repo = String(event?.repo?.name || "");
    const title = payload.pull_request?.title || payload.issue?.title
      || (Array.isArray(payload.commits) ? payload.commits[0]?.message : "")
      || payload.comment?.body || "";
    const line = `${what}${repo ? `${CRM_GITHUB_DIRECT.has(what) ? " " : " in "}${repo}` : ""}`;
    return title ? `${line}: ${crmPlainText(title, 120)}` : line;
  }

  // GitHub's public activity feed. It is unauthenticated, so the page reads
  // it only when a card opens and takes whatever the rate limit allows.
  async function crmGithubUpdates(account) {
    const events = await crmReadPublicJson(
      `https://api.github.com/users/${encodeURIComponent(account.key)}/events/public`
      + `?per_page=${CRM_UPDATE_LIMIT * 2}`);
    const repoUrl = (name) => String(name || "").split("/").length === 2
      ? `https://github.com/${String(name).split("/").map(encodeURIComponent).join("/")}` : "";
    return {
      picture: account.picture,
      posts: (Array.isArray(events) ? events : []).slice(0, CRM_UPDATE_LIMIT).map((event) => ({
        at: Date.parse(event?.created_at) || 0,
        text: crmGithubEventLine(event),
        url: repoUrl(event?.repo?.name),
      })),
    };
  }

  function openCrmContactDetail(contactId) {
    const contact = crmContactById(contactId);
    const dialog = $(`[data-crm-contact-detail-dialog]`);
    // A shared sheet is somebody else's read-only copy of a few columns, so
    // there is no record behind it to open.
    if (!contact || !dialog || crmActiveShared()) return;
    state.crmView.detailContactId = contact.id;
    state.crmView.detailUpdates = {};
    renderCrmContactDetail();
    if (!dialog.open) dialog.showModal();
    crmSetContactParam(contact.id);
    void loadCrmContactUpdates(contact);
  }

  function crmContactUrl(contactId) {
    const url = new URL(location.pathname, location.origin);
    url.searchParams.set("contact", contactId);
    return url.toString();
  }

  function crmSetContactParam(contactId) {
    crmSetLinkParam("contact", contactId);
  }

  async function copyCrmContactLink(contactId, button = null) {
    const contact = crmContactById(contactId);
    if (!contact) return;
    try {
      await navigator.clipboard.writeText(crmContactUrl(contact.id));
    } catch {
      crmMessage("error", "Could not copy the link.");
      return;
    }
    crmMessage("notice", `Link to ${crmContactLabel(contact)} copied. It opens for members of ${state.crmView.organization}.`);
    if (!button) return;
    button.textContent = "Link copied";
    setTimeout(() => { if (button.isConnected) button.textContent = "Copy link"; }, 2000);
  }

  function openCrmContactFromUrl() {
    const id = new URL(location.href).searchParams.get("contact") || "";
    if (!id || !state.crmView.installed) return;
    if (crmContactById(id)) { openCrmContactDetail(id); return; }
    crmSetContactParam("");
    crmMessage("error", `That link points to a contact that is not in ${state.crmView.organization}.`);
  }

  function crmReadingParts(email) {
    const seconds = Number(email.readSeconds) || 0;
    const depth = Number(email.scrollDepth) || 0;
    const slides = Number(email.slideCount) || 0;
    const slide = Number(email.slideReached) || 0;
    const client = String(email.mailClient || "");
    const parts = [];
    if (seconds) parts.push(`read ${seconds >= 60 ? `${Math.floor(seconds / 60)} min+` : `${seconds}s+`}`);
    if (slides && slide) {
      parts.push(slide >= slides ? `read all ${slides} slides` : `reached slide ${slide}/${slides}${email.slideTitle ? ` · ${email.slideTitle}` : ""}`);
    } else if (depth) {
      parts.push(depth >= 100 ? "scrolled to the end" : `scrolled ${depth}%`);
    }
    if (client && email.readingReported === "no" && !seconds && Number(email.opens)) parts.push(`${client} doesn't report read time`);
    else if (client && (seconds || depth)) parts.push(client);
    return parts;
  }

  // Everything that has happened on the record, newest first: the messages
  // linked to it, the deck link's opens, and the record's own stamps.
  function crmContactActivity(contact) {
    const items = crmLinkedEmails(contact.id).map((email) => ({
      at: email.sentAt || email.createdAt || 0,
      label: [`${email.direction === "inbound" ? "Received" : email.automated ? "Sent by AI" : "Sent"} · ${crmProviderLabel(email)}`,
        Number(email.opens) ? `${email.opens} open${Number(email.opens) === 1 ? "" : "s"}` : "",
        Number(email.clicks) ? `${email.clicks} click${Number(email.clicks) === 1 ? "" : "s"}` : "",
        ...crmReadingParts(email)].filter(Boolean).join(" · "),
      text: email.subject || "No subject",
      url: crmHttpsUrl(email.providerUrl),
    }));
    for (const note of contact.noteLog || []) {
      items.push({ at: note.at || 0, label: `Note · ${note.by || "a member"}`, text: note.text, url: "" });
    }
    const deck = crmDeckFor(contact.id);
    if (deck?.views) {
      items.push({
        at: deck.lastViewedAt || 0, label: "Deck opened",
        text: `${deck.views} open${deck.views === 1 ? "" : "s"} · ${crmDuration(deck.seconds)} on the pages`,
        url: "",
      });
    }
    if (contact.lastContactedAt) {
      items.push({ at: contact.lastContactedAt, label: "Last contacted",
                   text: "Recorded on the contact", url: "" });
    }
    if (contact.updatedAt) {
      items.push({ at: contact.updatedAt, label: "Record edited",
                   text: contact.updatedBy ? `by ${contact.updatedBy}` : "", url: "" });
    }
    if (contact.createdAt) {
      items.push({ at: contact.createdAt, label: "Record added",
                   text: contact.createdBy ? `by ${contact.createdBy}` : "", url: "" });
    }
    return items.sort((first, second) => (second.at || 0) - (first.at || 0));
  }

  // One line of a timeline: when it happened, what it was, and a link to it
  // where the record holds one.
  function crmDetailLine(item) {
    const when = item.at ? ` · ${relativeTimeLabel(item.at)}` : "";
    const text = item.text
      ? item.url
        ? `<a href="${escapeHtml(item.url)}" target="_blank" rel="noopener noreferrer" class="crm-link block text-xs">${escapeHtml(item.text)}</a>`
        : `<p class="text-xs text-foreground">${escapeHtml(item.text)}</p>`
      : "";
    return `<li class="crm-detail-line"><p class="text-xs text-muted-foreground">${escapeHtml(item.label)}${escapeHtml(when)}</p>${text}</li>`;
  }

  function renderCrmContactDetail() {
    const contact = crmContactById(state.crmView.detailContactId);
    const head = $(`[data-crm-detail-head]`);
    const body = $(`[data-crm-detail-body]`);
    if (!contact || !head || !body) return;
    const id = escapeHtml(contact.id);
    const accounts = crmLiveAccounts(contact);
    const subtitle = [contact.title, contact.company, contact.location].filter(Boolean).join(" · ");
    head.innerHTML = `
      <div class="flex min-w-0 items-start gap-4">
        <span class="crm-photo">${crmAvatar(contact, "xl")}<img data-crm-detail-photo alt="" hidden /></span>
        <div class="min-w-0">
          <h2 class="truncate text-lg font-semibold text-foreground">${escapeHtml(crmContactLabel(contact))}</h2>
          <p class="truncate text-xs text-muted-foreground">${escapeHtml(subtitle) || "No title or company on this record"}</p>
          <div class="mt-2 flex flex-wrap items-center gap-2">
            <span class="crm-stage crm-tone-${crmStageTone(contact)}">${escapeHtml(titleCaseSlug(contact.stage || "lead"))}</span>
            <span class="text-xs text-muted-foreground">${escapeHtml(crmFlow(contact.flow).name)}</span>
            ${crmLastContacted(contact)}
          </div>
          <div class="mt-2 flex flex-wrap items-center gap-1">${crmProfileTiles(contact, [])}${crmEmailChipMarkup(contact)}</div>
        </div>
      </div>`;
    const columns = [...CRM_BASE_COLUMNS, ...crmCustomFieldKeys().map((key) => ({
      field: key, label: crmColumnLabel(key),
    }))];
    const fields = columns.map((column) => `
      <label class="grid gap-1 text-xs text-muted-foreground${column.field === "notes" ? " sm:col-span-2" : ""}">${escapeHtml(column.label)}${crmContactControl(contact, column, "0")}</label>`).join("");
    const provenance = [
      contact.createdAt ? `Added ${relativeTimeLabel(contact.createdAt)}${contact.createdBy ? ` by ${contact.createdBy}` : ""}` : "",
      contact.updatedAt ? `last edited ${relativeTimeLabel(contact.updatedAt)}${contact.updatedBy ? ` by ${contact.updatedBy}` : ""}` : "",
      `shared with every member of ${contact.organization || state.crmView.organization}`,
    ].filter(Boolean).join(" · ");
    const activity = crmContactActivity(contact);
    const read = accounts.length
      ? `Read from ${accounts.map((account) => account.label).join(", ")} by this browser when the card opens; nothing is stored.`
      : "This record names no Mastodon, Bluesky or GitHub account to read.";
    body.innerHTML = `
      <div class="grid gap-5">
        <section>
          <h3 class="crm-detail-heading">Details</h3>
          <div class="grid gap-3 sm:grid-cols-2">${fields}</div>
          <p class="mt-2 text-xs text-muted-foreground">${escapeHtml(provenance)}.</p>
        </section>
        <section>
          <h3 class="crm-detail-heading">Add a note</h3>
          <textarea data-crm-note-text rows="3" maxlength="4000" aria-label="Note" placeholder="What happened, what was agreed, what comes next" class="w-full rounded-md border border-border bg-background px-2 py-1.5 text-control text-foreground focus:border-primary focus:outline-none"></textarea>
          <div class="mt-2 flex flex-wrap items-center justify-between gap-2">
            <p class="text-xs text-muted-foreground">Saved to the activity below and pinged to everyone on this contact's lists, including the people a list is shared with.</p>
            <button type="button" data-crm-note-save="${id}" class="h-control-sm rounded-md border border-border px-3 text-xs font-semibold text-foreground hover:bg-secondary">Save note</button>
          </div>
        </section>
        <section>
          <h3 class="crm-detail-heading">Activity</h3>
          ${activity.length
            ? `<ul class="grid gap-2">${activity.map(crmDetailLine).join("")}</ul>`
            : `<p class="text-xs text-muted-foreground">Nothing has been linked to this record yet.</p>`}
        </section>
        <section>
          <div class="mb-2 flex flex-wrap items-center justify-between gap-2">
            <h3 class="crm-detail-heading">Recent updates</h3>
            ${accounts.length ? `<button type="button" data-crm-detail-reload class="rounded-md border border-border px-2 py-1 text-xs font-semibold text-foreground hover:bg-secondary">Read again</button>` : ""}
          </div>
          <p class="mb-2 text-xs text-muted-foreground">${escapeHtml(read)}</p>
          <div data-crm-detail-updates class="grid gap-3"></div>
        </section>
        <div class="flex flex-wrap items-center justify-end gap-2 border-t border-border pt-3">
          <button type="button" data-crm-contact-compose="${id}" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Email</button>
          <button type="button" data-crm-contact-toot="${id}" title="Log a toot sent to or from them by pasting its link" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Log toot</button>
          <button type="button" data-crm-contact-ai="${id}" title="Gather everything about them and us into a prompt for ChatGPT or Claude to write the message" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Write with AI</button>
          ${crmEmailAddress(contact) ? `<button type="button" data-crm-contact-auto="${id}" title="${escapeHtml(CRM_AUTO_EMAIL_TITLE)}" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Send AI email</button>` : ""}
          <button type="button" data-crm-contact-deck="${id}" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Deck</button>
          <button type="button" data-crm-contact-link="${id}" title="Copy a link that opens this contact for members of ${escapeHtml(contact.organization || state.crmView.organization)}" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Copy link</button>
          <button type="button" data-crm-contact-edit="${id}" class="rounded-md border border-border px-3 py-2 text-xs font-semibold text-foreground hover:bg-secondary">Open the full form</button>
        </div>
      </div>`;
    renderCrmContactUpdates();
  }

  async function saveCrmContactNote(contactId, button) {
    const field = $(`[data-crm-note-text]`);
    const text = (field?.value || "").trim();
    if (!text) { field?.focus(); return; }
    if (button) button.disabled = true;
    try {
      const payload = await crmRequest(crmOrganizationPath("/contacts/note"), "POST", { contact: contactId, text });
      if (payload.contact) {
        state.crmView.contacts = state.crmView.contacts.map((contact) => contact.id === payload.contact.id ? payload.contact : contact);
      }
      if (state.crmView.detailContactId === contactId) renderCrmContactDetail();
      const pinged = Number(payload.pinged || 0);
      crmMessage("notice", pinged ? `Note saved. Pinged ${pinged} ${pinged === 1 ? "person" : "people"}.` : "Note saved.");
    } catch (error) {
      crmMessage("error", `The note was not saved: ${error.message}`);
      if (button?.isConnected) button.disabled = false;
    }
  }

  function renderCrmContactUpdates() {
    const host = $(`[data-crm-detail-updates]`);
    const contact = crmContactById(state.crmView.detailContactId);
    if (!host || !contact) return;
    host.innerHTML = crmLiveAccounts(contact).map((account) => {
      const slot = state.crmView.detailUpdates[`${account.service}:${account.key}`] || {};
      const posts = slot.loading
        ? `<p class="text-xs text-muted-foreground">Reading ${escapeHtml(account.label)}…</p>`
        : slot.error
        ? `<p class="text-xs text-muted-foreground">${escapeHtml(account.label)} could not be read: ${escapeHtml(slot.error)}.</p>`
        : (slot.posts || []).length
        ? `<ul class="grid gap-2">${slot.posts.map((post) => crmDetailLine({
            at: post.at, label: account.label, text: post.text, url: post.url,
          })).join("")}</ul>`
        : `<p class="text-xs text-muted-foreground">No public posts on this account.</p>`;
      const handle = account.url
        ? `<a href="${escapeHtml(account.url)}" target="_blank" rel="noopener noreferrer" class="crm-link truncate text-xs">${escapeHtml(account.handle)}</a>`
        : `<span class="truncate text-xs text-muted-foreground">${escapeHtml(account.handle)}</span>`;
      return `<section class="rounded-md border border-border p-3">
        <div class="mb-2 flex items-center justify-between gap-2"><span class="text-xs font-semibold uppercase tracking-wide text-muted-foreground">${escapeHtml(account.label)}</span>${handle}</div>
        ${posts}
      </section>`;
    }).join("");
    crmApplyDetailPhoto();
  }

  // The picture is whichever account answered with one first, in the same
  // order the desktop tries them. A picture that fails to load leaves the
  // initials showing underneath it.
  function crmApplyDetailPhoto() {
    const photo = $(`[data-crm-detail-photo]`);
    const contact = crmContactById(state.crmView.detailContactId);
    if (!photo || !contact) return;
    const picture = crmLiveAccounts(contact)
      .map((account) => state.crmView.detailUpdates[`${account.service}:${account.key}`]?.picture)
      .find(Boolean) || "";
    if (!picture || photo.dataset.crmPhoto === picture) return;
    photo.dataset.crmPhoto = picture;
    photo.hidden = true;
    photo.onload = () => { photo.hidden = false; };
    photo.onerror = () => { photo.hidden = true; };
    photo.src = picture;
  }

  // Every live account on the record, read in parallel so a slow instance
  // does not hold the others up. A card closed or reopened meanwhile drops
  // whatever is still in flight.
  const CRM_UPDATE_READERS = {
    mastodon: crmMastodonUpdates, bluesky: crmBlueskyUpdates, github: crmGithubUpdates,
  };

  async function loadCrmContactUpdates(contact) {
    const accounts = crmLiveAccounts(contact);
    for (const account of accounts) {
      state.crmView.detailUpdates[`${account.service}:${account.key}`] =
        { loading: true, posts: [], picture: account.picture };
    }
    renderCrmContactUpdates();
    await Promise.all(accounts.map(async (account) => {
      const slot = `${account.service}:${account.key}`;
      let result = { loading: false, posts: [], picture: account.picture };
      try {
        result = { ...result, ...await CRM_UPDATE_READERS[account.service](account) };
      } catch (error) {
        result.error = error.name === "TimeoutError" ? "it did not answer in time" : error.message;
      }
      if (state.crmView.detailContactId !== contact.id) return;
      state.crmView.detailUpdates[slot] = result;
      renderCrmContactUpdates();
    }));
  }

  // A stamp as a date input reads it, and back: the day itself, in the
  // reader's own timezone, so a date typed here is the date shown.
  function crmDateInputValue(stamp) {
    const date = new Date(Number(stamp) || 0);
    if (!stamp || Number.isNaN(date.getTime())) return "";
    return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, "0")}-${String(date.getDate()).padStart(2, "0")}`;
  }

  function crmDateInputStamp(value) {
    const parts = String(value || "").match(/^(\d{4})-(\d{2})-(\d{2})$/);
    if (!parts) return 0;
    return new Date(Number(parts[1]), Number(parts[2]) - 1, Number(parts[3]), 12).getTime();
  }

  function crmFieldValue(contact, field) {
    if (field === "tags") return (contact.tags || []).join(", ");
    if (field === "value") return contact.value ? String(contact.value) : "";
    if (field === "lastContactedAt") {
      return contact.lastContactedAt ? relativeTimeLabel(contact.lastContactedAt) : "Never";
    }
    if (CRM_CONTACT_FIELDS.includes(field)) return contact[field] || "";
    return (contact.fields || {})[field] || "";
  }

  // `line` is the table's single-line row: notes edit in a one-line field there
  // (the whole text is the tooltip) and only the mobile cards get a textarea.
  function crmInlineContactField(contact, column, width = "", line = false) {
    const value = crmFieldValue(contact, column.field);
    const placeholder = column.hint || `Add ${String(column.label || column.field).toLowerCase()}`;
    const attributes = `data-crm-contact-inline-id="${escapeHtml(contact.id)}" data-crm-contact-inline-field="${escapeHtml(column.field)}" aria-label="${escapeHtml(column.label || column.field)}" placeholder="${escapeHtml(placeholder)}" title="${escapeHtml(String(value))}" class="crm-cell" style="min-width:${width || column.width || "7rem"}"`;
    return column.field === "notes" && !line
      ? `<textarea ${attributes} rows="2">${escapeHtml(value)}</textarea>`
      : `<input ${attributes} type="${column.type || "text"}" value="${escapeHtml(value)}" />`;
  }

  function crmInlineContactSelect(contact, column, line = false) {
    const options = column.field === "flow" ? crmFlows().map((flow) => ({
      value: flow.id, label: flow.name,
    })) : (crmFlow(contact.flow).stages || []).map((stage) => ({
      value: stage, label: titleCaseSlug(stage),
    }));
    // The stage reads as a coloured pill; it is still the same select.
    const look = column.field === "stage"
      ? `class="crm-cell crm-stage crm-tone-${crmStageTone(contact)}"`
      : `class="crm-cell" style="min-width:${column.width || "7rem"}"`;
    return `<select data-crm-contact-inline-id="${escapeHtml(contact.id)}" data-crm-contact-inline-field="${escapeHtml(column.field)}" aria-label="${escapeHtml(column.label)}" ${look}>${options.map((option) => `<option value="${escapeHtml(option.value)}"${option.value === contact[column.field] ? " selected" : ""}>${escapeHtml(option.label)}</option>`).join("")}</select>`;
  }

  function crmContactControl(contact, column, width = "", line = false) {
    if (column.field === "lastContactedAt") {
      if (crmActiveShared()) return crmLastContacted(contact);
      // The warmth dot stays beside the date it is read from.
      return `<span class="flex items-center gap-1" title="${escapeHtml(crmFieldValue(contact, column.field))}">${crmWarmth(contact.lastContactedAt)}<input data-crm-contact-inline-id="${escapeHtml(contact.id)}" data-crm-contact-inline-field="lastContactedAt" aria-label="Last contacted" type="date" value="${escapeHtml(crmDateInputValue(contact.lastContactedAt))}" class="crm-cell" style="min-width:${width || column.width || "10rem"}" /><button type="button" data-crm-contacted-now="${escapeHtml(contact.id)}" title="Record now as the last time you were in touch" class="crm-tile">Now</button></span>`;
    }
    if (column.computed) return crmComputedCell(contact, column);
    if (column.field === "email" && !column.readonly && !crmActiveShared() && crmMailTile(contact)) {
      return `<span class="flex items-center gap-1">${crmInlineContactField(contact, column, width, line)}${crmMailTile(contact)}</span>`;
    }
    if (column.field === "stage" && crmActiveShared()) {
      return `<span class="crm-stage crm-tone-${crmStageTone(contact)}">${escapeHtml(titleCaseSlug(contact.stage))}</span>`;
    }
    // A Mastodon handle opens the profile on its own instance: the handle is the
    // link where the cell is read-only, and a link sits beside it where it edits.
    const profile = column.field === "mastodon" ? crmProfileUrl(column.field, contact.mastodon) : "";
    if (profile && (column.readonly || crmActiveShared())) {
      return `<a href="${escapeHtml(profile)}" target="_blank" rel="noopener noreferrer" class="crm-link block truncate whitespace-nowrap text-xs" style="max-width:${column.width || "12rem"}" title="${escapeHtml(profile)}">${escapeHtml(contact.mastodon)}</a>`;
    }
    if (profile) {
      return `<span class="flex items-center gap-1">${crmInlineContactField(contact, column, width, line)}<a href="${escapeHtml(profile)}" target="_blank" rel="noopener noreferrer" class="crm-tile" title="Open ${escapeHtml(profile)}" aria-label="Open Mastodon profile">&#8599;</a></span>`;
    }
    if (column.readonly || crmActiveShared()) {
      const value = crmFieldValue(contact, column.field);
      const text = column.select ? titleCaseSlug(value) : value;
      // On a table row a long value is cut off with an ellipsis, like a
      // desktop cell, instead of wrapping the row onto a second line.
      return line
        ? `<span class="block truncate whitespace-nowrap text-xs ${column.readonly ? "text-muted-foreground" : "text-foreground"}" style="max-width:${column.width || "12rem"}" title="${escapeHtml(String(value))}">${escapeHtml(text)}</span>`
        : `<span class="block ${column.field === "notes" ? "whitespace-pre-wrap" : "whitespace-nowrap"} text-xs ${column.readonly ? "text-muted-foreground" : "text-foreground"}" title="${escapeHtml(String(value))}">${escapeHtml(text)}</span>`;
    }
    return column.select
      ? crmInlineContactSelect(contact, column, line)
      : crmInlineContactField(contact, column, width, line);
  }

  function crmFilteredContacts() {
    const query = String($(`[data-crm-search]`)?.value || "").trim().toLowerCase();
    const view = crmActiveView();
    const flow = view?.flow ? "" : String($(`[data-crm-flow]`)?.value || "");
    const stage = String($(`[data-crm-stage]`)?.value || "");
    const owner = String($(`[data-crm-owner]`)?.value || "");
    const focus = String($(`[data-crm-focus]`)?.value || "");
    const shared = state.crmView.activeShared;
    const source = shared
      ? (state.crmView.sharedCache[shared]?.contacts || []) : state.crmView.contacts;
    const pipelines = crmPipelineIds();
    const matched = source.filter((contact) => {
      if (!shared && !(view && crmContactOnTab(contact, view, pipelines))) return false;
      if (flow && contact.flow !== flow) return false;
      if (stage && contact.stage !== stage) return false;
      if (owner && !crmOwnerMatches(contact, owner)) return false;
      if (focus && !crmFocusMatches(contact, focus)) return false;
      return crmContactMatchesQuery(contact, query);
    });
    if (focus === "duplicates") {
      const order = state.crmView.duplicates?.order || new Map();
      matched.sort((a, b) => order.get(a.id) - order.get(b.id));
    } else if (focus === "my-queue" || focus === "waiting") {
      matched.sort((a, b) => Number(crmWaitingOnUs(b)) - Number(crmWaitingOnUs(a))
        || Number(a.lastContactedAt || 0) - Number(b.lastContactedAt || 0));
    }
    return crmSortContacts(matched);
  }

  function renderCrmTabs() {
    const host = $(`[data-crm-tabs]`); if (!host) return;
    hideCrmShareCard();
    const tabs = crmTabViews();
    const pipelines = crmPipelineIds();
    const counts = new Map();
    for (const contact of state.crmView.contacts) {
      for (const view of tabs) {
        if (crmContactOnTab(contact, view, pipelines)) {
          counts.set(view.id, (counts.get(view.id) || 0) + 1);
        }
      }
    }
    const tip = (view) => {
      if (view.id === CRM_NO_PIPELINE_TAB) return "Contacts whose pipeline was deleted. Right-click them to move them into another one.";
      if (!view.flow) return "Double-click to rename, drag to move, right-click for list options";
      const kind = CRM_FLOW_ICONS.find((item) => item.id === crmFlow(view.flow).icon) || CRM_FLOW_ICONS[0];
      return view.virtual
        ? `${kind.label} pipeline: every contact in ${crmFlowName(view.flow)}. Double-click to rename, right-click for pipeline options`
        : `${kind.label} pipeline: a list on ${crmFlowName(view.flow)}. Double-click to rename, drag to move, right-click for list options`;
    };
    const onContacts = state.crmView.section === "contacts";
    const listIcon = `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" aria-hidden="true"><path d="M5.5 4h8M5.5 8h8M5.5 12h8M2.5 4h.01M2.5 8h.01M2.5 12h.01"/></svg>`;
    const tab = (id, label, count, view) => {
      const active = onContacts && state.crmView.activeView === id;
      const people = id === CRM_NO_PIPELINE_TAB ? [] : crmSheetShareNames(view);
      // Who the sheet reaches, as a count on the row; the whole list is the
      // hover card, so the column stays readable with many lists.
      const share = people.length
        ? `<span data-crm-share="${escapeHtml(id)}" title="${escapeHtml(crmSheetShareLabel(view))}" class="crm-side-tag">\u{1F465} ${people.length}</span>`
        : "";
      return `<div data-crm-tab-row="${escapeHtml(id)}"${view.virtual ? "" : ` data-crm-tab-drag="${escapeHtml(id)}" draggable="true"`} class="crm-side-row${active ? " crm-side-current" : ""}">
        <button data-crm-tab="${escapeHtml(id)}" type="button" role="tab" aria-selected="${active}" title="${escapeHtml(tip(view))}" class="crm-side-item">${view.flow ? crmFlowIcon(crmFlow(view.flow).icon) : listIcon}<span class="crm-side-label">${escapeHtml(label)}</span>${view.visibility === "private" ? `<span class="crm-side-tag" title="${escapeHtml(crmSheetShareLabel(view))}">Private</span>` : ""}${share}<span class="crm-side-count">${count}</span></button>
        <button data-crm-tab-menu="${escapeHtml(id)}" type="button" aria-label="Options for ${escapeHtml(label)}" class="crm-side-menu">⋯</button>
      </div>`;
    };
    const sharedTab = (view) => {
      const active = onContacts && state.crmView.activeShared === view.id;
      const cached = state.crmView.sharedCache[view.id];
      const count = cached ? `<span class="crm-side-count">${cached.contacts.length}</span>` : "";
      return `<div data-crm-tab-row="${escapeHtml(view.id)}" class="crm-side-row${active ? " crm-side-current" : ""}">
        <button data-crm-tab-shared="${escapeHtml(view.id)}" type="button" role="tab" aria-selected="${active}" title="Shared by ${escapeHtml(view.owner || view.organization)} from ${escapeHtml(view.organization)} · ${view.canEdit ? "you can edit it" : "read-only"}" class="crm-side-item">${listIcon}<span class="crm-side-label">${escapeHtml(view.name)}</span><span class="crm-side-tag">${escapeHtml(view.organization)}</span>${count}</button>
        ${state.crmView.sharedOnly ? "" : `<button data-crm-tab-shared-menu="${escapeHtml(view.id)}" type="button" aria-label="Options for ${escapeHtml(view.name)}" class="crm-side-menu">⋯</button>`}
      </div>`;
    };
    const shared = state.crmView.sharedViews;
    host.innerHTML = tabs.map((view) =>
      tab(view.id, view.name, counts.get(view.id) || 0, view)).join("")
      + (shared.length
        ? `<p class="crm-side-heading crm-side-heading-more" title="Lists other ForkMesh users shared with you">Shared with me</p>`
          + shared.map(sharedTab).join("")
        : "");
  }

  function renderCrmSharedBanner() {
    const banner = $(`[data-crm-shared-banner]`); if (!banner) return;
    const view = crmActiveShared();
    banner.classList.toggle("hidden", !view);
    banner.classList.toggle("flex", Boolean(view));
    if (!view) return;
    const cached = state.crmView.sharedCache[view.id];
    const access = (cached?.view || view).canEdit ? "you can add contacts and move them along its stages" : "read-only";
    $(`[data-crm-shared-banner-text]`).textContent = cached
      ? `Shared by ${view.owner || view.organization} from ${view.organization} · ${access} · ${cached.contacts.length} ${cached.contacts.length === 1 ? "contact" : "contacts"} with the columns they chose to show.`
      : `Loading the list ${view.organization} shared with you…`;
    const copy = $(`[data-crm-shared-copy]`);
    if (copy) {
      copy.classList.toggle("hidden", Boolean(state.crmView.sharedOnly));
      copy.textContent = `Copy into ${state.crmView.organization}`;
    }
  }

  function crmSheetShareLabel(view) {
    if (!view) return "Shared with the organization";
    const names = view.sharedWith || [];
    const outside = names.filter((name) => !state.crmView.members.includes(name));
    if (view.visibility !== "private") {
      return outside.length ? `Shared with the organization and ${outside.join(", ")}` : "Shared with the organization";
    }
    return names.length ? `Shared with ${names.join(", ")}` : "Private to you";
  }

  // Everyone a sheet reaches besides the member who owns it: the names it picks
  // out, plus the whole roster while it stays open to the organization.
  function crmSheetShareNames(view) {
    if (!view) return [];
    const names = [...(view.sharedWith || [])];
    if (view.visibility !== "private") {
      for (const name of state.crmView.members) {
        if (!names.includes(name)) names.push(name);
      }
    }
    const owner = String(view.owner || "").toLowerCase();
    return names.filter((name) => name && name !== owner);
  }

  function hideCrmShareCard() {
    const card = $(`[data-crm-share-card]`);
    if (card) { card.hidden = true; card.innerHTML = ""; }
  }

  function showCrmShareCard(badge) {
    const card = $(`[data-crm-share-card]`); if (!card) return;
    const view = crmTabView(badge.dataset.crmShare);
    const names = crmSheetShareNames(view);
    if (!names.length) { hideCrmShareCard(); return; }
    const picked = new Set(view.sharedWith || []);
    card.innerHTML = `<p class="mb-1 text-[10px] font-semibold uppercase text-muted-foreground">${escapeHtml(
      view.visibility === "private"
        ? `Shared with ${names.length} ${names.length === 1 ? "person" : "people"}`
        : `Open to ${names.length} ${names.length === 1 ? "person" : "people"} in ${state.crmView.organization}`)}</p>`
      + names.map((name) => `<p class="truncate text-xs text-foreground">${escapeHtml(name)}${picked.has(name)
        ? "" : ` <span class="text-muted-foreground">· member</span>`}</p>`).join("")
      + `<p class="mt-1 text-[10px] text-muted-foreground">Owner: ${escapeHtml(view.owner || state.crmView.organization)}</p>`;
    card.hidden = false;
    // Clamp into the viewport the same way the right-click menu does.
    const bounds = card.getBoundingClientRect();
    const anchor = badge.getBoundingClientRect();
    card.style.left = `${Math.max(4, Math.min(anchor.left, window.innerWidth - bounds.width - 4))}px`;
    card.style.top = `${Math.min(anchor.bottom + 6, window.innerHeight - bounds.height - 4)}px`;
  }

  function renderCrmContacts() {
    const list = $(`[data-crm-contact-list]`); if (!list) return;
    crmSyncActiveTab();
    const columns = crmVisibleColumns();
    const contacts = crmFilteredContacts();
    const shared = Boolean(crmActiveShared());
    const people = crmPeople.open && !shared;
    const shownIds = new Set(contacts.map((contact) => contact.id));
    crmSelection.shown = contacts.map((contact) => contact.id);
    crmSelection.ids = new Set([...crmSelection.ids].filter((id) => !shared && shownIds.has(id)));
    renderCrmTabs();
    renderCrmStageStrip();
    const flowFilter = $(`[data-crm-flow]`); if (flowFilter) flowFilter.hidden = Boolean(crmActiveView()?.flow);
    renderCrmSharedBanner();
    renderCrmNextContact();
    const board = state.crmView.layout === "board";
    renderCrmTableViews();
    const columnsButton = $(`[data-crm-columns]`);
    if (columnsButton) {
      columnsButton.disabled = shared || board;
      columnsButton.title = board ? "Columns apply to the table layout" : "Choose which columns this view shows";
    }
    $$(`[data-crm-layout-option]`).forEach((button) =>
      button.setAttribute("aria-pressed", String(button.dataset.crmLayoutOption === state.crmView.layout)));
    const peopleButton = $(`[data-crm-people-toggle]`);
    if (peopleButton) { peopleButton.setAttribute("aria-pressed", String(people)); peopleButton.disabled = shared; }
    for (const [selector, hidden] of [["[data-crm-board]", !board || people], ["[data-crm-table]", board || people], ["[data-crm-contact-cards]", board || people], ["[data-crm-people]", !people]]) {
      const node = $(selector); if (node) node.hidden = hidden;
    }
    const strip = $(`[data-crm-stage-strip]`); if (strip && people) strip.hidden = true;
    if (people) renderCrmPeople();
    if (board && !people) renderCrmBoard(contacts);
    else if ($(`[data-crm-board-note]`)) $(`[data-crm-board-note]`).hidden = true;
    const head = $(`[data-crm-contact-head]`);
    if (head) {
      const sorted = (column) => crmSort.field === column.field
        ? ` aria-sort="${crmSort.descending ? "descending" : "ascending"}"` : "";
      const arrow = (column) => crmSort.field === column.field
        ? `<span aria-hidden="true" class="ml-1">${crmSort.descending ? "\u25BC" : "\u25B2"}</span>` : "";
      head.innerHTML = (shared ? "" : `<th class="w-8 px-2 py-2"><input type="checkbox" data-crm-select-all aria-label="Select every shown contact" /></th>`)
        + columns.map((column) => shared
        ? `<th data-crm-column="${escapeHtml(column.field)}"${sorted(column)} title="Click to sort" style="cursor:pointer" class="whitespace-nowrap px-2 py-2 font-semibold">${escapeHtml(column.label)}${arrow(column)}</th>`
        : `<th data-crm-column="${escapeHtml(column.field)}"${sorted(column)} draggable="true" title="Click to sort, drag to move, or right-click to remove this column" style="cursor:grab" class="whitespace-nowrap px-2 py-2 font-semibold">${escapeHtml(column.label)}${arrow(column)}</th>`).join("")
        + (shared ? "" : `<th class="whitespace-nowrap px-2 py-2 text-right font-semibold">Actions</th><th class="px-1 py-2"><button type="button" data-crm-add-column title="Add a column" aria-label="Add a column" class="crm-tile">+</button></th>`);
    }
    // The picture and the name both open the contact's detail card, where
    // every field is edited; a shared sheet is somebody else's read-only copy
    // of a few columns, so there is no record of ours behind it to open.
    const photo = (contact) => shared
      ? crmAvatar(contact)
      : `<button type="button" data-crm-contact-detail="${escapeHtml(contact.id)}" title="Open the detail card" aria-label="Open ${escapeHtml(crmContactLabel(contact))}" class="crm-photo-button">${crmPicture(contact)}</button>`;
    const name = (contact, column) => shared
      ? crmContactControl(contact, column)
      : `<button type="button" data-crm-contact-detail="${escapeHtml(contact.id)}" title="${escapeHtml(crmContactLabel(contact))} · open the detail card" class="crm-name-open truncate">${escapeHtml(crmContactLabel(contact))}</button>`;
    // The name cell carries an avatar, the emails tied to the record, and any
    // profile whose column is hidden; a shared sheet is read-only, so it gets
    // neither chips nor row actions.
    const cell = (contact, column) => column.field === "name"
      ? `<div class="flex items-start gap-2">${photo(contact)}<div class="min-w-0 flex-1">${name(contact, column)}${crmNameMeta(contact, columns)}</div></div>`
      : crmContactControl(contact, column);
    const actions = (contact, compact) => shared ? "" : compact
      ? `<div class="mt-3 flex justify-end gap-1"><button data-crm-contact-deck="${escapeHtml(contact.id)}" class="rounded border border-border px-2 py-1 text-xs text-foreground">Deck</button><button data-crm-contact-compose="${escapeHtml(contact.id)}" class="rounded border border-border px-2 py-1 text-xs text-foreground">Email</button><button data-crm-contact-prompt="${escapeHtml(contact.id)}" class="rounded border border-border px-2 py-1 text-xs text-foreground">Copy prompt</button><button data-crm-contact-edit="${escapeHtml(contact.id)}" class="rounded border border-border px-2 py-1 text-xs text-foreground">Edit</button><button data-crm-contact-delete-row="${escapeHtml(contact.id)}" class="rounded border border-destructive px-2 py-1 text-xs text-destructive">Delete</button></div>`
      : `<td class="px-3 py-2 align-top"><div class="crm-row-actions flex justify-end gap-1"><button data-crm-contact-deck="${escapeHtml(contact.id)}" title="Share the investor deck with a private, tracked link" class="whitespace-nowrap rounded border border-border px-2 py-1 text-xs text-foreground hover:bg-background">Deck</button><button data-crm-contact-compose="${escapeHtml(contact.id)}" class="whitespace-nowrap rounded border border-border px-2 py-1 text-xs text-foreground hover:bg-background">Email</button><button data-crm-contact-prompt="${escapeHtml(contact.id)}" title="Copy a prompt for an assistant to write them a toot about ForkMesh" class="whitespace-nowrap rounded border border-border px-2 py-1 text-xs text-foreground hover:bg-background">Copy prompt</button><button data-crm-contact-edit="${escapeHtml(contact.id)}" class="whitespace-nowrap rounded border border-border px-2 py-1 text-xs text-foreground hover:bg-background">Edit</button><button data-crm-contact-delete-row="${escapeHtml(contact.id)}" aria-label="Delete ${escapeHtml(contact.name || contact.email || "contact")}" class="whitespace-nowrap rounded border border-border px-2 py-1 text-xs text-destructive hover:bg-background">Delete</button></div></td><td></td>`;
    list.innerHTML = contacts.map((contact) => `
      <tr data-crm-contact-row="${escapeHtml(contact.id)}" class="crm-row">
        ${shared ? "" : `<td class="px-2 py-2 align-top"><input type="checkbox" data-crm-select="${escapeHtml(contact.id)}" aria-label="Select ${escapeHtml(crmContactLabel(contact))}" class="mt-1.5" /></td>`}
        ${columns.map((column) => `<td class="px-3 py-2 align-top">${cell(contact, column)}</td>`).join("")}
        ${actions(contact, false)}
      </tr>`).join("");
    const cards = $(`[data-crm-contact-cards]`);
    if (cards) cards.innerHTML = contacts.map((contact) => `
      <article data-crm-contact-row="${escapeHtml(contact.id)}" class="rounded-md border border-border bg-card p-3">
        <div class="flex items-start justify-between gap-3">
          <div class="flex min-w-0 items-start gap-2">${photo(contact)}<div class="min-w-0">${columns.some((column) => column.field === "name")
            ? `<span class="block text-xs text-muted-foreground">Name</span>${name(contact, crmBaseColumn("name"))}`
            : `<span class="block truncate text-sm font-semibold text-foreground">${escapeHtml(crmContactLabel(contact))}</span>`}${crmNameMeta(contact, columns)}</div></div>
          <span class="crm-stage crm-tone-${crmStageTone(contact)}">${escapeHtml(titleCaseSlug(contact.stage || "lead"))}</span>
        </div>
        <div class="mt-3 grid grid-cols-2 gap-x-3 gap-y-2 text-xs">
          ${columns.filter((column) => column.field !== "name").map((column) => `<label class="grid gap-1 text-muted-foreground${column.field === "notes" ? " col-span-2" : ""}">${escapeHtml(column.label)}${crmContactControl(contact, column, "0")}</label>`).join("")}
        </div>
        ${actions(contact, true)}
      </article>`).join("");
    $(`[data-crm-contact-empty]`)?.classList.toggle("hidden", contacts.length > 0 || people);
    crmPaintSelection();
    crmWatchPictures();
    const empty = $(`[data-crm-contact-empty]`);
    if (empty) empty.textContent = shared && !state.crmView.sharedCache[state.crmView.activeShared]
      ? "Loading the shared sheet…" : "No contacts match this view.";
    renderCrmGrantsLoad();
  }

  let crmGrantsBusy = false;

  function crmGrantsTarget() {
    const view = crmActiveView();
    if (!view?.flow || crmActiveShared() || crmPeople.open || crmFlow(view.flow).icon !== "grants") return null;
    return state.crmView.contacts.some((contact) => contact.flow === view.flow) ? null : view;
  }

  function renderCrmGrantsLoad() {
    const box = $(`[data-crm-grants-empty]`); if (!box) return;
    const view = crmGrantsTarget();
    box.classList.toggle("hidden", !view);
    if (!view) return;
    $(`[data-crm-contact-empty]`)?.classList.add("hidden");
    const manages = Boolean(state.crmView.canManageSettings);
    const button = $(`[data-crm-grants-load]`);
    button.disabled = !manages || crmGrantsBusy;
    button.title = manages
      ? `Add the 100 grants, funds and credit programs that best fit ForkMesh to ${crmFlowName(view.flow)}, ranked, with the award, deadline and application details of each`
      : "Only organization owners and admins can load the grants list";
    $(`[data-crm-grants-load-label]`).textContent = crmGrantsBusy ? "Loading the ranked grants…" : "Load the 100 ranked grants";
  }

  async function loadCrmGrants() {
    const view = crmGrantsTarget();
    if (!view || crmGrantsBusy || !state.crmView.canManageSettings) return;
    crmGrantsBusy = true;
    renderCrmGrantsLoad();
    crmMessage("error"); crmMessage("notice");
    try {
      const payload = await crmRequest(crmOrganizationPath("/contacts/grants"), "POST", {
        flow: view.flow, viewId: view.virtual ? "" : view.id,
      });
      if (payload.viewId) { state.crmView.activeView = payload.viewId; crmSetListParam(payload.viewId); }
      crmMessage("notice", payload.created
        ? `${payload.view?.name || "Grants"}: ${payload.created} grants loaded.`
        : "Every ranked grant is already on this pipeline.");
      await loadCrm();
    } catch (error) { crmMessage("error", `Could not load the grants: ${error.message}`); }
    finally { crmGrantsBusy = false; renderCrmGrantsLoad(); }
  }

  // --- board layout --------------------------------------------------------
  // The rows on screen, as one column per stage of a single flow. Dragging a
  // card to another column changes its stage; clicking it (or Enter) opens
  // the contact's detail card, whose Stage field is the keyboard way to move
  // it.

  const CRM_BOARD_COLUMN_LIMIT = 50;

  function crmLayoutKey() {
    return `forkmesh-crm-layout:${state.crmView.organization}`;
  }

  function crmReadLayout() {
    try { return localStorage.getItem(crmLayoutKey()) === "board" ? "board" : "table"; }
    catch { return "table"; }
  }

  function setCrmLayout(layout) {
    state.crmView.layout = layout === "board" ? "board" : "table";
    crmPeople.open = false;
    try { localStorage.setItem(crmLayoutKey(), state.crmView.layout); }
    catch { /* the layout still applies for this visit */ }
    renderCrmContacts();
  }

  // A pipeline tab's flow, else the flow filter, else the flow most of the
  // rows on screen belong to.
  function crmBoardFlow(contacts) {
    const bound = crmActiveView()?.flow || String($(`[data-crm-flow]`)?.value || "");
    if (bound) return crmFlow(bound);
    const counts = new Map();
    for (const contact of contacts) counts.set(contact.flow, (counts.get(contact.flow) || 0) + 1);
    const [top] = [...counts.entries()].sort((a, b) => b[1] - a[1])[0] || [];
    return crmFlow(top);
  }

  function crmShortAge(ms) {
    const days = crmDaysSince(ms);
    return days === null ? "never" : days === 0 ? "today" : `${days}d`;
  }

  function renderCrmBoard(contacts) {
    const host = $(`[data-crm-board]`); if (!host) return;
    const note = $(`[data-crm-board-note]`);
    const shared = Boolean(crmActiveShared()) && !crmActiveShared()?.canEdit;
    const flow = crmBoardFlow(contacts);
    const inFlow = contacts.filter((contact) => crmFlow(contact.flow).id === flow.id);
    const stages = [...(flow.stages || [])];
    // A stage renamed away in settings still has contacts; keep them visible.
    for (const contact of inFlow) if (contact.stage && !stages.includes(contact.stage)) stages.push(contact.stage);
    const others = contacts.length - inFlow.length;
    if (note) {
      note.hidden = !others;
      note.textContent = others
        ? `Showing ${flow.name}. ${others} ${others === 1 ? "contact" : "contacts"} in other flows ${others === 1 ? "isn't" : "aren't"} on this board; pick a flow to see ${others === 1 ? "it" : "them"}.`
        : "";
    }
    const card = (contact) => {
      const owner = contact.owner ? `<span title="Owner: ${escapeHtml(contact.owner)}" class="truncate">${escapeHtml(contact.owner)}</span>` : "";
      return `<article data-crm-contact-detail="${escapeHtml(contact.id)}" data-crm-board-card="${escapeHtml(contact.id)}" role="button" tabindex="0" aria-label="Open ${escapeHtml(contact.name || contact.email || "contact")}"${shared ? "" : ' draggable="true"'} class="crm-board-card">
        <div class="flex min-w-0 items-center gap-2">${crmPicture(contact)}<div class="min-w-0"><p class="truncate text-xs font-semibold text-foreground">${escapeHtml(contact.name || contact.email || "Unnamed contact")}</p><p class="truncate text-xs text-muted-foreground">${escapeHtml(contact.company || contact.email || "")}</p></div></div>
        <div class="flex items-center justify-between gap-2 text-xs text-muted-foreground"><span class="flex items-center">${crmWarmth(contact.lastContactedAt)}${crmShortAge(contact.lastContactedAt)}</span>${owner}</div>
        ${crmQuietTag(contact)}
      </article>`;
    };
    host.innerHTML = stages.map((stage) => {
      const rows = inFlow.filter((contact) => contact.stage === stage);
      const shown = rows.slice(0, CRM_BOARD_COLUMN_LIMIT).map(card).join("");
      const more = rows.length > CRM_BOARD_COLUMN_LIMIT
        ? `<p class="px-1 text-xs text-muted-foreground">+ ${rows.length - CRM_BOARD_COLUMN_LIMIT} more; search to narrow the board.</p>` : "";
      const worth = rows.reduce((total, contact) => total + (Number(contact.value) || 0), 0);
      const worthLabel = worth ? ` \u00b7 ${worth.toLocaleString()}` : "";
      return `<section data-crm-board-stage="${escapeHtml(stage)}" aria-label="${escapeHtml(titleCaseSlug(stage))}, ${rows.length}${worth ? `, worth ${worth}` : ""}" class="crm-board-column">
        <div class="crm-board-head"><span class="crm-stage crm-tone-${crmToneOf(flow.id, stage)}">${escapeHtml(titleCaseSlug(stage))}</span><span class="crm-board-count">${rows.length}${escapeHtml(worthLabel)}</span></div>
        ${shown || `<p class="crm-board-empty">${shared ? "No contacts" : "Drop a contact here"}</p>`}${more}
      </section>`;
    }).join("");
  }

  async function saveCrmSharedContact(view, id, body) {
    const payload = await crmRequest(
      `/api/crm/-/shared/${encodeURIComponent(view.id)}/contacts${id ? `/${encodeURIComponent(id)}` : ""}`,
      id ? "PATCH" : "POST", body);
    const cached = state.crmView.sharedCache[view.id];
    if (cached && payload.contact) {
      const at = cached.contacts.findIndex((item) => item.id === payload.contact.id);
      if (at >= 0) cached.contacts[at] = payload.contact; else cached.contacts.push(payload.contact);
    }
    renderCrmContacts();
    return payload.contact;
  }

  async function saveCrmContactStage(id, stage) {
    const sharedView = crmActiveShared();
    if (sharedView) {
      const cached = state.crmView.sharedCache[sharedView.id]?.contacts || [];
      const row = cached.find((item) => item.id === id);
      if (!sharedView.canEdit || !row || row.stage === stage) return;
      try {
        await saveCrmSharedContact(sharedView, id, { stage });
        crmMessage("notice", `${row.name || row.email || "Contact"} moved to ${titleCaseSlug(stage)}.`);
      } catch (error) { crmMessage("error", `Could not update Stage: ${error.message}`); }
      return;
    }
    const contact = crmContactById(id);
    if (!contact || contact.stage === stage) return;
    try {
      const payload = await crmRequest(crmOrganizationPath(`/contacts/${id}`), "PATCH", { stage });
      Object.assign(contact, payload.contact || { stage });
      renderCrmContacts();
      crmMessage("notice", `${contact.name || contact.email || "Contact"} moved to ${titleCaseSlug(stage)}.`);
    } catch (error) { crmMessage("error", `Could not update Stage: ${error.message}`); }
  }

  function renderCrmEmails() {
    const list = $(`[data-crm-email-list]`); if (!list) return;
    list.innerHTML = state.crmView.emails.map((email) => {
      const linked = ["gmail", "zoho", "mastodon"].includes(email.provider);
      const provider = email.providerUrl && String(email.providerUrl).startsWith("https://")
        ? `<a href="${escapeHtml(email.providerUrl)}" target="_blank" rel="noopener noreferrer" class="text-primary hover:underline">${linked ? `Open in ${escapeHtml(crmProviderLabel(email))}` : "Open send"} ↗</a>` : "";
      const contact = crmContactById(email.contactId);
      // A Mastodon conversation has handles where an email has addresses.
      const who = email.handle ? `@${email.handle}` : "";
      const party = email.direction === "inbound"
        ? `From ${escapeHtml(who || email.fromEmail || contact?.email || "unknown")}`
        : `To ${escapeHtml(who || email.to || "unknown")}`;
      return `<article class="border-b border-border p-3 last:border-b-0">
        <div class="flex items-start justify-between gap-2"><p class="min-w-0 truncate text-sm font-semibold text-foreground">${escapeHtml(email.subject || "No subject")}</p><span class="shrink-0 text-xs ${crmStatusClass(email.status)}">${escapeHtml(linked ? "linked" : (email.status || "external"))}</span></div>
        <p class="mt-1 truncate text-xs text-muted-foreground">${party} · ${escapeHtml(linked ? crmProviderLabel(email) : (email.transport || "external"))}${contact?.name ? ` · ${escapeHtml(contact.name)}` : ""}</p>
        <p class="mt-1 flex justify-between gap-2 text-xs text-muted-foreground"><span>${email.createdAt ? escapeHtml(relativeTimeLabel(email.createdAt)) : ""}</span>${provider}</p>
        ${email.externalId ? `<p class="mt-1 truncate font-mono text-[10px] text-muted-foreground">${escapeHtml(email.externalId)}</p>` : ""}
      </article>`;
    }).join("");
    $(`[data-crm-email-empty]`)?.classList.toggle("hidden", state.crmView.emails.length > 0);
  }

  function renderCrmMail() {
    const list = $(`[data-crm-mail-groups]`); if (!list) return;
    const boxes = state.crmView.settings?.mailBoxes || ["Inbox", "Follow-up", "Important", "Archive"];
    const filter = $(`[data-crm-mail-box-filter]`);
    const selected = String(filter?.value || "");
    if (filter) {
      filter.innerHTML = `<option value="">All boxes</option>` + boxes.map((box) =>
        `<option value="${escapeHtml(box)}">${escapeHtml(box)}</option>`).join("");
      filter.value = boxes.includes(selected) ? selected : "";
    }
    const messages = state.crmView.mail.filter((message) => !filter?.value || message.box === filter.value);
    const groups = new Map();
    for (const message of messages) {
      if (!groups.has(message.groupKey)) groups.set(message.groupKey, []);
      groups.get(message.groupKey).push(message);
    }
    list.innerHTML = [...groups.values()].map((group) => {
      const latest = group[0];
      const provider = latest.providerUrl?.startsWith("https://")
        ? `<a href="${escapeHtml(latest.providerUrl)}" target="_blank" rel="noopener noreferrer" class="text-primary hover:underline">Open in Zoho ↗</a>` : "";
      return `<article class="rounded-md border border-border bg-secondary p-3">
        <div class="flex items-start justify-between gap-2"><p class="min-w-0 truncate text-sm font-semibold text-foreground">${escapeHtml(latest.subject || "No subject")}</p><span class="flex shrink-0 items-center gap-1">${group.some((message) => message.mine) ? `<span title="From your own Zoho Mail: only you see it" class="rounded-full border border-border px-2 py-0.5 text-[10px] text-muted-foreground">Only you</span>` : ""}<span class="rounded-full border border-border px-2 py-0.5 text-[10px] text-muted-foreground">${group.length} ${group.length === 1 ? "message" : "messages"}</span></span></div>
        <p class="mt-1 truncate text-xs text-muted-foreground">${escapeHtml(latest.sender || latest.from || "Unknown sender")}</p>
        <p class="mt-2 line-clamp-2 text-xs text-muted-foreground">${escapeHtml(latest.summary || "")}</p>
        <div class="mt-3 flex items-center justify-between gap-2"><select data-crm-mail-move="${escapeHtml(latest.id)}" class="rounded border border-border bg-background px-2 py-1 text-xs text-foreground">${boxes.map((box) => `<option value="${escapeHtml(box)}"${box === latest.box ? " selected" : ""}>${escapeHtml(box)}</option>`).join("")}</select><span class="text-xs">${provider}</span></div>
      </article>`;
    }).join("");
    $(`[data-crm-mail-empty]`)?.classList.toggle("hidden", groups.size > 0);
  }

  function renderCrm() {
    const summary = state.crmView.summary || {};
    for (const key of ["contacts", "customers", "sent", "failed"]) {
      const node = $(`[data-crm-stat-${key}]`); if (node) node.textContent = Number(summary[key] || 0).toLocaleString();
    }
    $(`[data-crm-failed-summary]`)?.classList.toggle("text-destructive", Number(summary.failed || 0) > 0);
    renderCrmSections(); renderCrmFlowFilters(); renderCrmContacts(); renderCrmEmails(); renderCrmMail(); renderCrmOutreach();
    renderCrmTeam(); renderCrmMailLinks(); crmNoteListViewed(); renderCrmZohoBar(); renderCrmMyZoho();
    $(`[data-crm-account-evidence]`)?.classList.toggle("hidden", !state.crmView.canManageSettings);
    renderCrmUsersImport();
    if ($(`[data-crm-lists-dialog]`)?.open) renderCrmListsOverview();
    const pipelineButton = $(`[data-crm-pipeline-new]`);
    if (pipelineButton) {
      pipelineButton.disabled = !state.crmView.canManageSettings;
      pipelineButton.title = state.crmView.canManageSettings
        ? "Create a pipeline with its own stages; it opens as a new tab"
        : "Only organization owners and admins can add pipelines";
    }
  }

  // --- page sections ---------------------------------------------------------
  // Contacts, Inbox (linked email and Zoho boxes) and Outreach are tabs. The
  // open one is in the address as ?section= so it survives a reload or a link.

  const CRM_SECTIONS = ["contacts", "inbox", "outreach", "team", "zoho"];

  function crmSectionFromUrl() {
    const section = new URL(location.href).searchParams.get("section") || "";
    return CRM_SECTIONS.includes(section) ? section : "contacts";
  }

  function selectCrmSection(section, focus = false) {
    const next = CRM_SECTIONS.includes(section) ? section : "contacts";
    state.crmView.section = next;
    const url = new URL(location.href);
    if (next === "contacts") url.searchParams.delete("section");
    else url.searchParams.set("section", next);
    history.replaceState(history.state, "", url);
    renderCrmSections();
    if (focus) $$(`[data-crm-section-tab]`).find((tab) => tab.dataset.crmSectionTab === next)?.focus();
  }

  function renderCrmSections() {
    const current = state.crmView.section;
    $$(`[data-crm-section-tab]`).forEach((tab) => {
      const selected = tab.dataset.crmSectionTab === current;
      tab.setAttribute("aria-selected", String(selected));
      tab.tabIndex = selected ? 0 : -1;
    });
    $$(`[data-crm-section]`).forEach((panel) => { panel.hidden = panel.dataset.crmSection !== current; });
    $$(`[data-crm-section-tab]`).forEach((tab) => tab.classList.toggle("crm-side-current", tab.dataset.crmSectionTab === current));
    $$(`[data-crm-tab-row]`).forEach((row) => {
      const id = row.dataset.crmTabRow;
      const open = current === "contacts" && (id === state.crmView.activeView || id === state.crmView.activeShared);
      row.classList.toggle("crm-side-current", open);
      row.querySelector("[role=tab]")?.setAttribute("aria-selected", String(open));
    });
    const people = $(`[data-crm-side-people]`);
    if (people) {
      const open = current === "contacts" && crmPeople.open;
      people.setAttribute("aria-pressed", String(open));
      people.classList.toggle("crm-side-current", open);
    }
    renderCrmMyZohoDot(); renderCrmZohoBar();
    const counts = {
      contacts: state.crmView.contacts.length,
      inbox: state.crmView.emails.length + state.crmView.mail.length,
    };
    $$(`[data-crm-section-count]`).forEach((badge) => {
      badge.textContent = Number(counts[badge.dataset.crmSectionCount] || 0).toLocaleString();
    });
  }

  function crmOutreach() {
    return state.crmView.outreach || null;
  }

  const CRM_TEAM_SERIES = [
    { key: "emails", label: "Emails", color: "var(--crm-series-1)" },
    { key: "outreach", label: "Outreach", color: "var(--crm-series-2)" },
    { key: "contacts", label: "Contacts added", color: "var(--crm-series-3)" },
  ];

  function crmTeamSum(values, from) {
    return (values || []).slice(from).reduce((total, value) => total + Number(value || 0), 0);
  }

  function crmTeamRanking() {
    const days = state.crmView.activity.days || [];
    const from = Math.max(0, days.length - state.crmView.teamPeriod);
    return (state.crmView.activity.members || []).map((member) => {
      const row = { name: member.name };
      for (const series of CRM_TEAM_SERIES) row[series.key] = crmTeamSum(member[series.key], from);
      row.touches = row.emails + row.outreach;
      return row;
    }).sort((a, b) => b.touches - a.touches || b.contacts - a.contacts || a.name.localeCompare(b.name));
  }

  function crmTeamDay(day, long = false) {
    const date = new Date(`${day}T12:00:00`);
    return date.toLocaleDateString(undefined, long ? { weekday: "short", month: "short", day: "numeric" } : { month: "short", day: "numeric" });
  }

  function crmTeamDaily() {
    const activity = state.crmView.activity;
    const days = activity.days || [];
    const from = Math.max(0, days.length - state.crmView.teamPeriod);
    const members = (activity.members || []).filter((member) => !state.crmView.teamMember || member.name === state.crmView.teamMember);
    return days.slice(from).map((day, offset) => {
      const index = from + offset, row = { day };
      for (const series of CRM_TEAM_SERIES) row[series.key] = members.reduce((total, member) => total + Number((member[series.key] || [])[index] || 0), 0);
      row.total = CRM_TEAM_SERIES.reduce((total, series) => total + row[series.key], 0);
      return row;
    });
  }

  function crmTeamBarPath(x, y, width, height, radius) {
    const r = Math.min(radius, width / 2, height);
    return `M${x.toFixed(1)} ${(y + height).toFixed(1)}V${(y + r).toFixed(1)}Q${x.toFixed(1)} ${y.toFixed(1)} ${(x + r).toFixed(1)} ${y.toFixed(1)}H${(x + width - r).toFixed(1)}Q${(x + width).toFixed(1)} ${y.toFixed(1)} ${(x + width).toFixed(1)} ${(y + r).toFixed(1)}V${(y + height).toFixed(1)}Z`;
  }

  function crmTeamChart(daily) {
    const W = 720, H = 260, left = 36, top = 12, right = 8, bottom = 28;
    const width = W - left - right, height = H - top - bottom;
    const peak = Math.max(0, ...daily.map((row) => row.total));
    const step = Math.max(1, Math.ceil(peak / 4));
    const max = step * 4;
    let out = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${escapeHtml(`Daily CRM activity for the last ${daily.length} days`)}">`;
    for (let i = 0; i <= 4; i += 1) {
      const y = top + height - height * i / 4;
      out += `<line x1="${left}" x2="${left + width}" y1="${y.toFixed(1)}" y2="${y.toFixed(1)}" stroke="var(--crm-grid)" stroke-width="1"/><text x="${left - 6}" y="${(y + 4).toFixed(1)}" text-anchor="end" font-size="10" fill="var(--crm-ink-muted)">${(step * i).toLocaleString()}</text>`;
    }
    const slot = width / Math.max(1, daily.length), bar = Math.min(28, Math.max(4, slot - 4));
    const labelEvery = daily.length > 10 ? 5 : 1;
    daily.forEach((row, index) => {
      const x = left + index * slot + (slot - bar) / 2;
      const tip = [crmTeamDay(row.day, true), ...CRM_TEAM_SERIES.map((series) => `${series.label}: ${row[series.key].toLocaleString()}`)].join("\n");
      out += `<g data-crm-team-tip="${escapeHtml(tip)}"><rect class="crm-team-hit" x="${(left + index * slot).toFixed(1)}" y="${top}" width="${slot.toFixed(1)}" height="${height}"/>`;
      let base = top + height;
      const shown = CRM_TEAM_SERIES.filter((series) => row[series.key] > 0);
      shown.forEach((series, position) => {
        const full = height * row[series.key] / max;
        const gap = position === 0 ? 0 : 2;
        const segment = Math.max(1, full - gap);
        base -= full;
        out += position === shown.length - 1
          ? `<path d="${crmTeamBarPath(x, base + gap, bar, segment, 4)}" fill="${series.color}"/>`
          : `<rect x="${x.toFixed(1)}" y="${(base + gap).toFixed(1)}" width="${bar.toFixed(1)}" height="${segment.toFixed(1)}" fill="${series.color}"/>`;
      });
      out += "</g>";
      if ((daily.length - 1 - index) % labelEvery === 0) out += `<text x="${(index === daily.length - 1 ? x + bar : x + bar / 2).toFixed(1)}" y="${H - 8}" text-anchor="${index === daily.length - 1 ? "end" : "middle"}" font-size="10" fill="var(--crm-ink-muted)">${escapeHtml(crmTeamDay(row.day))}</text>`;
    });
    return `${out}</svg>`;
  }

  function crmTeamTable(daily) {
    return `<table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="px-2 py-1">Day</th>${CRM_TEAM_SERIES.map((series) => `<th class="px-2 py-1 text-right">${escapeHtml(series.label)}</th>`).join("")}<th class="px-2 py-1 text-right">Total</th></tr></thead><tbody>${[...daily].reverse().map((row) => `<tr class="border-t border-border"><td class="px-2 py-1">${escapeHtml(crmTeamDay(row.day, true))}</td>${CRM_TEAM_SERIES.map((series) => `<td class="px-2 py-1 text-right">${row[series.key].toLocaleString()}</td>`).join("")}<td class="px-2 py-1 text-right font-semibold">${row.total.toLocaleString()}</td></tr>`).join("")}</tbody></table>`;
  }

  function renderCrmTeam() {
    const board = $(`[data-crm-team-board]`); if (!board) return;
    const view = state.crmView;
    const ranking = crmTeamRanking();
    if (view.teamMember && !ranking.some((row) => row.name === view.teamMember)) view.teamMember = "";
    const me = crmOutreach()?.name || "";
    board.innerHTML = ranking.map((row, index) => `<tr data-crm-team-row="${escapeHtml(row.name)}" class="crm-team-row" aria-selected="${row.name === view.teamMember}"><td class="px-2 py-2 text-xs text-muted-foreground">${index + 1}</td><td class="px-2 py-2 font-medium text-foreground">${escapeHtml(row.name)}${row.name === me ? ` <span class="text-xs font-normal text-muted-foreground">(you)</span>` : ""}</td><td class="px-2 py-2 text-right">${row.emails.toLocaleString()}</td><td class="px-2 py-2 text-right">${row.outreach.toLocaleString()}</td><td class="px-2 py-2 text-right">${row.contacts.toLocaleString()}</td><td class="px-2 py-2 text-right font-semibold">${row.touches.toLocaleString()}</td></tr>`).join("");
    $(`[data-crm-team-empty]`)?.classList.toggle("hidden", ranking.some((row) => row.touches || row.contacts));
    $$(`[data-crm-team-period-option]`).forEach((button) => button.setAttribute("aria-pressed", String(Number(button.dataset.crmTeamPeriodOption) === view.teamPeriod)));
    const select = $(`[data-crm-team-member]`);
    if (select) {
      select.innerHTML = `<option value="">All members</option>${ranking.map((row) => `<option value="${escapeHtml(row.name)}">${escapeHtml(row.name)}</option>`).join("")}`;
      select.value = view.teamMember;
    }
    const daily = crmTeamDaily();
    const title = $(`[data-crm-team-chart-title]`);
    if (title) title.textContent = `Daily activity · ${view.teamMember || "all members"} · last ${daily.length} days`;
    const chart = $(`[data-crm-team-chart]`), table = $(`[data-crm-team-table]`);
    if (chart) {
      chart.innerHTML = crmTeamChart(daily);
      chart.classList.toggle("hidden", view.teamTable);
      crmTeamTooltip(chart);
    }
    if (table) { table.innerHTML = crmTeamTable(daily); table.classList.toggle("hidden", !view.teamTable); }
    $(`[data-crm-team-table-toggle]`)?.setAttribute("aria-pressed", String(view.teamTable));
  }

  function crmTeamTooltip(host) {
    const tip = document.createElement("div");
    tip.className = "crm-team-tip hidden";
    host.appendChild(tip);
    host.onmousemove = (event) => {
      const target = event.target.closest("[data-crm-team-tip]");
      if (!target) { tip.classList.add("hidden"); return; }
      tip.textContent = target.dataset.crmTeamTip;
      tip.classList.remove("hidden");
      const rect = host.getBoundingClientRect();
      tip.style.left = `${Math.max(4, Math.min(event.clientX - rect.left + 12, rect.width - tip.offsetWidth - 4))}px`;
      tip.style.top = `${Math.max(0, event.clientY - rect.top - tip.offsetHeight - 8)}px`;
    };
    host.onmouseleave = () => tip.classList.add("hidden");
  }

  function crmOutreachHint(message = "", bad = false) {
    const hint = $(`[data-crm-outreach-hint]`); if (!hint) return;
    hint.textContent = message;
    hint.classList.toggle("text-destructive", Boolean(bad));
    hint.classList.toggle("text-muted-foreground", !bad);
  }

  function crmOutreachStatusLine(outreach) {
    if (!outreach.allowed) return `Signed in as ${outreach.name}.`;
    return `Sending as ${outreach.name} from ${outreach.fromEmail} · ${outreach.sentToday || 0}/${outreach.dailyLimit} sent today${outreach.configured ? "" : " · email provider not configured"}`;
  }

  function renderCrmOutreachTemplates() {
    const select = $(`[data-crm-outreach-template]`); if (!select) return;
    const templates = crmOutreach()?.templates || [];
    const current = select.value;
    select.innerHTML = templates.map((template) => `<option value="${escapeHtml(template.key)}">${escapeHtml(template.label)}</option>`).join("");
    if (templates.some((template) => template.key === current)) select.value = current;
  }

  function applyCrmOutreachTemplate() {
    const key = $(`[data-crm-outreach-template]`)?.value || "";
    const template = (crmOutreach()?.templates || []).find((item) => item.key === key);
    if (!template) return;
    $(`[data-crm-outreach-subject]`).value = template.subject || "";
    $(`[data-crm-outreach-body]`).value = template.body || "";
    crmOutreachHint("");
  }

  function renderCrmOutreachTeam() {
    const outreach = crmOutreach(); const panel = $(`[data-crm-outreach-team-panel]`); if (!panel) return;
    panel.classList.toggle("hidden", !outreach?.canManage);
    if (!outreach?.canManage) return;
    const members = outreach.members || [];
    const list = $(`[data-crm-outreach-team-list]`); if (!list) return;
    list.innerHTML = members.length ? members.map((member) => `<div class="flex items-center justify-between gap-2 px-3 py-2 text-sm">
        <div class="min-w-0"><p class="truncate font-semibold text-foreground">${escapeHtml(member.name)}</p><p class="truncate text-xs text-muted-foreground">added by ${escapeHtml(member.addedBy || "?")}${member.addedAt ? ` · ${escapeHtml(relativeTimeLabel(member.addedAt))}` : ""}</p></div>
        <button data-crm-outreach-team-remove="${escapeHtml(member.name)}" type="button" class="shrink-0 rounded-md border border-border px-2 py-1 text-xs text-foreground hover:bg-secondary">Remove</button>
      </div>`).join("")
      : `<p class="px-3 py-2 text-xs text-muted-foreground">No extra members yet; only owners and admins can send.</p>`;
  }

  function renderCrmOutreachLog() {
    const list = $(`[data-crm-outreach-log]`); if (!list) return;
    const recent = crmOutreach()?.recent || [];
    list.innerHTML = recent.map((entry) => `<article class="px-3 py-2">
        <div class="flex items-start justify-between gap-2"><p class="min-w-0 truncate text-sm text-foreground">${escapeHtml(entry.to || "?")} — ${escapeHtml(entry.subject || "(no subject)")}</p><span class="shrink-0 text-xs ${entry.ok ? "text-primary" : "text-destructive"}">${entry.ok ? "sent" : "failed"}</span></div>
        <p class="mt-1 truncate text-xs text-muted-foreground">by ${escapeHtml(entry.sender || "?")}${entry.ts ? ` · ${escapeHtml(relativeTimeLabel(entry.ts))}` : ""}${entry.error ? ` · ${escapeHtml(entry.error)}` : ""}</p>
      </article>`).join("");
    $(`[data-crm-outreach-log-empty]`)?.classList.toggle("hidden", recent.length > 0);
  }

  // Status pipeline: for each template type, how far the sends got (attempted,
  // accepted, failed), derived from the same recent-sends log the list shows.
  function renderCrmOutreachPipeline() {
    const wrap = $(`[data-crm-outreach-pipeline]`); if (!wrap) return;
    const outreach = crmOutreach(); const templates = outreach?.templates || [];
    const order = []; const stats = new Map();
    const ensure = (key, label) => {
      if (!stats.has(key)) { stats.set(key, { label, ok: 0, fail: 0 }); order.push(key); }
      return stats.get(key);
    };
    for (const template of templates) if (template.key !== "blank") ensure(template.key, template.label);
    for (const entry of outreach?.recent || []) {
      const key = entry.template || "blank";
      const known = templates.find((template) => template.key === key);
      const bucket = ensure(key, known ? known.label : (key === "blank" ? "Blank / custom" : key));
      if (entry.ok) bucket.ok += 1; else bucket.fail += 1;
    }
    const stage = (count, label, tone = "text-foreground") => `<div class="flex-1 rounded-md bg-secondary px-1 py-1 text-center text-[10px] uppercase tracking-wide text-muted-foreground"><span class="block text-sm font-semibold ${tone}">${count}</span>${label}</div>`;
    wrap.innerHTML = order.length ? order.map((key) => {
      const bucket = stats.get(key); const total = bucket.ok + bucket.fail;
      return `<div class="px-3 py-2">
        <div class="flex justify-between gap-2 text-sm font-semibold text-foreground"><span>${escapeHtml(bucket.label)}</span><span class="text-xs font-normal text-muted-foreground">${total ? `${total} sent` : "none yet"}</span></div>
        <div class="mt-1.5 flex gap-1.5">${stage(total, "Contacted")}${stage(bucket.ok, "Sent", "text-primary")}${stage(bucket.fail, "Failed", "text-destructive")}</div>
      </div>`;
    }).join("") : `<p class="px-3 py-2 text-xs text-muted-foreground">No templates to track yet.</p>`;
  }

  function renderCrmOutreach() {
    const outreach = crmOutreach();
    const status = $(`[data-crm-outreach-status]`);
    const allowed = Boolean(outreach?.allowed);
    $(`[data-crm-outreach-denied]`)?.classList.toggle("hidden", allowed || !outreach);
    $(`[data-crm-outreach-app]`)?.classList.toggle("hidden", !allowed);
    if (status) status.textContent = outreach ? crmOutreachStatusLine(outreach) : "";
    if (!allowed) return;
    renderCrmOutreachTemplates(); renderCrmOutreachTeam(); renderCrmOutreachLog(); renderCrmOutreachPipeline();
  }

  async function loadCrmOutreach() {
    try { state.crmView.outreach = await crmRequest(crmOrganizationPath("/outreach")); renderCrmOutreach(); }
    catch (error) { crmMessage("error", `Outreach is unavailable: ${error.message}`); }
  }

  async function sendCrmOutreach(event) {
    event.preventDefault();
    const outreach = crmOutreach(); if (!outreach?.allowed) return;
    const body = {
      to: $(`[data-crm-outreach-to]`).value.trim(), subject: $(`[data-crm-outreach-subject]`).value.trim(),
      body: $(`[data-crm-outreach-body]`).value.trim(), template: $(`[data-crm-outreach-template]`).value,
    };
    if (!body.to || !body.subject || !body.body) { crmOutreachHint("Recipient, subject and message are all required.", true); return; }
    if (/\[[^\]]*\]/.test(body.subject + body.body) &&
        !confirm("The email still contains [bracketed] placeholders. Send anyway?")) return;
    if (!confirm(`Send this email to ${body.to} from ${outreach.fromEmail}?`)) return;
    const button = $(`[data-crm-outreach-send]`); button.disabled = true; crmOutreachHint("Sending…");
    try {
      const payload = await crmRequest(crmOrganizationPath("/outreach/send"), "POST", body);
      $(`[data-crm-outreach-to]`).value = "";
      crmOutreachHint(`Sent to ${payload.to}.`);
      await loadCrmOutreach();
    } catch (error) {
      const detail = error.payload?.error === "daily_limit_reached"
        ? `Daily send limit reached (${error.payload.dailyLimit}/day). Try again tomorrow.`
        : error.payload?.detail || error.message;
      crmOutreachHint(`Send failed: ${detail}`, true);
      if (error.payload?.error === "send_failed") await loadCrmOutreach();
    } finally { button.disabled = false; }
  }

  async function updateCrmOutreachTeam(action, name) {
    try {
      const payload = await crmRequest(crmOrganizationPath("/outreach/team"), "POST", { action, name });
      if (state.crmView.outreach) state.crmView.outreach.members = payload.members || [];
      renderCrmOutreachTeam();
    } catch (error) {
      crmMessage("error", error.payload?.error === "member_not_found"
        ? `${name} is not a member of ${state.crmView.organization}.`
        : `Could not update the outreach team: ${error.message}`);
    }
  }

  function openCrmContact(contact = null) {
    const dialog = $(`[data-crm-contact-dialog]`); if (!dialog) return;
    $(`[data-crm-contact-dialog-title]`).textContent = contact ? "Edit contact" : "Add contact";
    $(`[data-crm-contact-id]`).value = contact?.id || "";
    for (const field of CRM_DIALOG_FIELDS) {
      const node = $(`[data-crm-contact-${field}]`); if (node) node.value = contact?.[field] || "";
    }
    const flowSelect = $(`[data-crm-contact-flow]`);
    flowSelect.innerHTML = crmFlows().map((flow) => `<option value="${escapeHtml(flow.id)}">${escapeHtml(flow.name)}</option>`).join("");
    // A contact added on a pipeline tab starts in that pipeline.
    const sharedView = !contact && crmActiveShared()?.canEdit ? crmActiveShared() : null;
    if (sharedView?.flow) flowSelect.innerHTML = `<option value="${escapeHtml(sharedView.flow)}">${escapeHtml(sharedView.name)}</option>`;
    flowSelect.value = crmFlow(contact?.flow || sharedView?.flow || crmActiveView()?.flow || "customers").id;
    renderCrmContactStages(contact?.stage || "");
    $(`[data-crm-contact-tags]`).value = (contact?.tags || []).join(", ");
    $(`[data-crm-contact-delete]`).classList.toggle("hidden", !contact);
    state.crmView.contactRequestId = contact ? "" : crmRequestId();
    crmContactDialogError();
    dialog.showModal(); $(`[data-crm-contact-name]`)?.focus();
  }

  function crmRequestId() {
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    return [...bytes].map((byte) => byte.toString(16).padStart(2, "0")).join("");
  }

  function crmContactDialogError(message = "", existing = null) {
    const box = $(`[data-crm-contact-error]`); if (!box) return;
    $(`[data-crm-contact-error-text]`).textContent = message;
    const open = $(`[data-crm-contact-existing]`);
    open.dataset.crmContactExisting = existing?.id || "";
    open.classList.toggle("hidden", !existing?.id);
    box.classList.toggle("hidden", !message);
    if (message) box.scrollIntoView({ block: "nearest" });
  }

  async function openCrmExistingContact(id) {
    if (!id) return;
    $(`[data-crm-contact-dialog]`).close();
    if (!crmContactById(id)) await loadCrm();
    openCrmContactDetail(id);
  }

  function crmContactSaveError(error) {
    const code = error.payload?.error || "";
    const existing = error.payload?.contact || null;
    if (code === "contact_email_exists" || code === "contact_exists") {
      const label = existing ? crmContactLabel(existing) : "Another contact";
      return [`${label} already uses that email address${existing ? ", so it is already in the CRM" : ""}.`, existing];
    }
    if (code === "crm_full") return ["The CRM is full: delete or merge contacts before adding more.", null];
    if (code === "name_or_email_required") return ["Add a name or an email address.", null];
    if (code === "invalid_email") return ["That email address is not valid.", null];
    if (!error.status) return ["The server could not be reached. Check the connection and press Save again; it will not add the contact twice.", null];
    return [`Could not save contact: ${error.message}`, null];
  }

  // Profiles are researched by hand: open a web search seeded with whatever the
  // contact record already knows, so handles can be pasted back into the form.
  function crmProfileSearchQuery() {
    const name = String($(`[data-crm-contact-name]`)?.value || "").trim();
    const company = String($(`[data-crm-contact-company]`)?.value || "").trim();
    const email = String($(`[data-crm-contact-email]`)?.value || "").trim();
    const subject = [name && `"${name}"`, company, !name && !company && email]
      .filter(Boolean).join(" ");
    return subject ? `${subject} twitter OR mastodon OR bluesky OR linkedin` : "";
  }

  function openCrmProfileSearch() {
    const query = crmProfileSearchQuery();
    if (!query) return crmMessage("error", "Add a name, company, or email first.");
    window.open(`https://duckduckgo.com/?q=${encodeURIComponent(query)}`,
      "_blank", "noopener,noreferrer");
  }

  function renderCrmContactStages(preferred = "") {
    const flow = crmFlow($(`[data-crm-contact-flow]`)?.value);
    const stageSelect = $(`[data-crm-contact-stage]`); if (!stageSelect) return;
    stageSelect.innerHTML = (flow.stages || []).map((stage) =>
      `<option value="${escapeHtml(stage)}">${escapeHtml(titleCaseSlug(stage))}</option>`).join("");
    stageSelect.value = (flow.stages || []).includes(preferred) ? preferred : (flow.stages?.[0] || "lead");
  }

  async function saveCrmContact(event) {
    event.preventDefault();
    const id = $(`[data-crm-contact-id]`).value;
    const body = {};
    for (const field of [...CRM_DIALOG_FIELDS, "flow", "stage"]) body[field] = $(`[data-crm-contact-${field}]`).value;
    body.tags = $(`[data-crm-contact-tags]`).value.split(",").map((tag) => tag.trim()).filter(Boolean);
    // A contact typed while a sheet is open would otherwise land outside the
    // tab that is on screen, looking like the save was lost.
    const sheet = crmActiveView();
    if (sheet && !sheet.virtual && !sheet.flow && !id) body.views = [sheet.id];
    if (!id && !String(body.name || "").trim() && !String(body.email || "").trim()) {
      crmContactDialogError("Add a name or an email address.");
      return;
    }
    const submit = $(`[data-crm-contact-submit]`);
    if (submit?.disabled) return;
    if (submit) submit.disabled = true;
    crmContactDialogError();
    const sharedView = crmActiveShared();
    try {
      if (sharedView?.canEdit && !id) {
        await saveCrmSharedContact(sharedView, "", body);
        $(`[data-crm-contact-dialog]`).close();
        crmMessage("notice", `Contact added to ${sharedView.name} in ${sharedView.organization}.`);
        return;
      }
      if (!id) body.requestId = state.crmView.contactRequestId || (state.crmView.contactRequestId = crmRequestId());
      let payload;
      for (let attempt = 0; ; attempt += 1) {
        try {
          payload = await crmRequest(crmOrganizationPath(`/contacts${id ? `/${id}` : ""}`), id ? "PATCH" : "POST", body);
          break;
        } catch (error) {
          const transient = !error.status || crmTransientStatus(error.status);
          if (id || !transient || attempt >= CRM_READ_RETRY_DELAYS_MS.length) throw error;
          await new Promise((resolve) => setTimeout(resolve, CRM_READ_RETRY_DELAYS_MS[attempt]));
        }
      }
      $(`[data-crm-contact-dialog]`).close();
      state.crmView.contactRequestId = "";
      crmMessage("notice", id ? "Contact updated." : payload.replayed ? "Contact was already added." : "Contact added.");
      await loadCrm();
    } catch (error) {
      const [message, existing] = crmContactSaveError(error);
      crmContactDialogError(message, existing);
    } finally { if (submit) submit.disabled = false; }
  }

  async function saveCrmInlineContactField(input) {
    const id = input.dataset.crmContactInlineId;
    const field = input.dataset.crmContactInlineField;
    const dated = field === "lastContactedAt";
    const custom = !dated && !CRM_CONTACT_FIELDS.includes(field);
    if (!id || !field || (custom && !crmCustomFieldKeys().includes(field))) return;
    const restore = () => {
      const contact = crmContactById(id) || {};
      input.value = dated
        ? crmDateInputValue(contact.lastContactedAt) : crmFieldValue(contact, field);
    };
    input.disabled = true;
    try {
      const payload = await crmRequest(crmOrganizationPath(`/contacts/${id}`), "PATCH",
        custom ? { fields: { [field]: input.value } }
        : { [field]: dated ? crmDateInputStamp(input.value)
            : field === "tags"
            ? input.value.split(",").map((tag) => tag.trim()).filter(Boolean)
            : input.value });
      const contact = crmContactById(id);
      if (contact && payload.contact) Object.assign(contact, payload.contact);
      restore();
      renderCrmContacts();
      // An open detail card holds the same fields: re-reading it moves its
      // stage pill, its stamps, and the stages its flow offers along too.
      if (state.crmView.detailContactId) renderCrmContactDetail();
      crmMessage("notice", `${crmColumnLabel(field)} updated.`);
    } catch (error) {
      restore();
      crmMessage("error", `Could not update ${crmColumnLabel(field)}: ${error.message}`);
    } finally { input.disabled = false; }
  }

  async function deleteCrmContact(contactId = "") {
    const id = typeof contactId === "string" ? contactId : $(`[data-crm-contact-id]`).value;
    const contact = crmContactById(id); if (!id || !confirm(`Delete ${contact?.name || "this contact"}? Email history will remain.`)) return;
    try { await crmRequest(crmOrganizationPath(`/contacts/${id}`), "DELETE", {}); $(`[data-crm-contact-dialog]`).close(); crmMessage("notice", "Contact deleted."); await loadCrm(); }
    catch (error) { crmMessage("error", `Could not delete contact: ${error.message}`); }
  }

  function openCrmEmail(contact = null) {
    const settings = state.crmView.settings || {}; state.crmView.draftContactId = "";
    $(`[data-crm-email-contact-id]`).value = contact?.id || "";
    $(`[data-crm-email-to]`).value = contact?.email || "";
    $(`[data-crm-email-from-name]`).value = settings.fromName || "";
    $(`[data-crm-email-from-email]`).value = settings.fromEmail || "";
    $(`[data-crm-email-key]`).value = ""; $(`[data-crm-email-subject]`).value = ""; $(`[data-crm-email-text]`).value = "";
    const mine = settings.zohoMine;
    $(`[data-crm-email-via]`).textContent = mine?.connected
      ? `Sends from your Zoho Mail (account ${mine.accountId}), so it lands in your Sent folder. Fill in a Mailtrap key to send through Mailtrap instead; the key is never saved.`
      : "Your Mailtrap key is used for this request only and is never saved. Connect My Zoho Mail to send from your own mailbox.";
    $(`[data-crm-email-from-email]`).required = !mine?.connected;
    $(`[data-crm-email-submit]`).textContent = crmEmailSubmitLabel();
    $(`[data-crm-email-dialog]`).showModal(); $(`[data-crm-email-subject]`)?.focus();
  }

  function crmEmailSubmitLabel() {
    return state.crmView.settings?.zohoMine?.connected && !$(`[data-crm-email-key]`)?.value ? "Send from Zoho Mail" : "Send with Mailtrap";
  }

  async function sendCrmEmail(event) {
    event.preventDefault(); const button = $(`[data-crm-email-submit]`); button.disabled = true; button.textContent = "Sending…";
    const body = {
      contactId: $(`[data-crm-email-contact-id]`).value, to: $(`[data-crm-email-to]`).value,
      fromName: $(`[data-crm-email-from-name]`).value, fromEmail: $(`[data-crm-email-from-email]`).value,
      subject: $(`[data-crm-email-subject]`).value, text: $(`[data-crm-email-text]`).value,
      mailtrapKey: $(`[data-crm-email-key]`).value,
    };
    try {
      const sent = await crmRequest(crmOrganizationPath("/emails/send"), "POST", body); $(`[data-crm-email-key]`).value = ""; $(`[data-crm-email-dialog]`).close();
      crmMessage("notice", sent.via === "zoho" ? `Sent from ${sent.fromEmail || "your mailbox"} through Zoho Mail.` : "Mailtrap accepted the email.");
      const draft = state.crmView.draftContactId; state.crmView.draftContactId = "";
      if (draft && draft === body.contactId) await markCrmDraftSent(draft, false); else await loadCrm();
    }
    catch (error) { $(`[data-crm-email-key]`).value = ""; crmMessage("error", `Email was not accepted: ${error.message}`); }
    finally { button.disabled = false; button.textContent = crmEmailSubmitLabel(); }
  }

  const CRM_DRAFT_MARK = "DRAFT, not sent.";
  const CRM_ZOHO_CALLBACK = "https://app.forkmesh.com/api/crm/-/zoho/callback";

  function crmDraftAt(notes) {
    let at = notes.indexOf(CRM_DRAFT_MARK);
    while (at > 0 && notes[at - 1] !== "\n") at = notes.indexOf(CRM_DRAFT_MARK, at + 1);
    return at;
  }

  function crmNoteDrafts() {
    const drafts = [];
    for (const contact of state.crmView.contacts || []) {
      const notes = String(contact.notes || ""); const at = crmDraftAt(notes); if (at < 0) continue;
      const end = notes.indexOf("\n\n", at);
      const lines = notes.slice(at, end < 0 ? undefined : end).split("\n");
      const mastodon = /mastodon/i.test(lines.shift());
      const subject = lines[0]?.startsWith("Subject:") ? lines.shift().slice(8).trim() : "";
      const body = lines.join("\n").trim(); if (!body) continue;
      let thread = "";
      if (mastodon) {
        const toot = (state.crmView.emails || [])
          .filter((email) => email.contactId === contact.id && email.provider === "mastodon" && email.providerUrl)
          .sort((a, b) => (b.sentAt || 0) - (a.sentAt || 0))[0];
        thread = toot?.providerUrl || notes.match(/https:\/\/[A-Za-z0-9.-]+\/@[A-Za-z0-9_.]+\/[0-9]{6,}/)?.[0] || "";
      }
      const to = String((mastodon ? contact.mastodon : contact.email) || "");
      drafts.push({ contactId: contact.id, name: contact.name || to, mastodon, to, subject, body, thread });
    }
    return drafts;
  }

  function renderCrmDraftsLabel() {
    const label = $(`[data-crm-drafts-label]`); if (!label) return;
    const count = crmNoteDrafts().length;
    label.textContent = count ? `Drafts (${count})` : "Drafts";
  }

  function renderCrmDrafts() {
    const drafts = crmNoteDrafts(); state.crmView.drafts = drafts;
    const mastodon = drafts.filter((draft) => draft.mastodon).length;
    $(`[data-crm-drafts-summary]`).textContent = drafts.length
      ? `${drafts.length} unsent ${drafts.length === 1 ? "draft" : "drafts"}: ${drafts.length - mastodon} by email, ${mastodon} as Mastodon replies. Click the button on a row to send it.`
      : "No unsent drafts. A draft is a contact note paragraph that starts with \"DRAFT, not sent.\"";
    $(`[data-crm-drafts-body]`).innerHTML = drafts.length ? `<table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="px-2 py-1">Contact</th><th class="px-2 py-1">Channel</th><th class="px-2 py-1">To</th><th class="px-2 py-1">Message</th><th class="px-2 py-1"></th></tr></thead><tbody>${drafts.map((draft, index) => {
      const ready = draft.mastodon ? Boolean(draft.thread) : Boolean(draft.to);
      const message = draft.subject ? `${draft.subject} — ${draft.body}` : draft.body;
      return `<tr class="border-t border-border align-top"><td class="px-2 py-2 font-semibold">${escapeHtml(draft.name)}</td><td class="px-2 py-2">${draft.mastodon ? "Mastodon reply" : "Email"}</td><td class="px-2 py-2">${escapeHtml(draft.to)}</td><td class="px-2 py-2" title="${escapeHtml(message)}">${escapeHtml(message.length > 160 ? message.slice(0, 159) + "…" : message)}</td><td class="px-2 py-2"><button data-crm-draft-send="${index}" type="button" ${ready ? "" : "disabled"} title="${escapeHtml(ready ? (draft.mastodon ? `Copy the reply and open ${draft.thread}` : `Open the compose form filled in for ${draft.to}`) : (draft.mastodon ? "No outreach toot is linked to this contact" : "The contact has no email address"))}" class="whitespace-nowrap rounded-md bg-primary px-2.5 py-1.5 text-xs font-semibold text-primary-foreground disabled:opacity-45">${draft.mastodon ? "Reply on Mastodon" : "Send"}</button></td></tr>`;
    }).join("")}</tbody></table>` : "";
  }

  function openCrmDrafts() {
    renderCrmDrafts(); const dialog = $(`[data-crm-drafts-dialog]`);
    if (!dialog.open) dialog.showModal();
  }

  async function sendCrmDraft(index) {
    const draft = state.crmView.drafts?.[index]; if (!draft) return;
    if (!draft.mastodon) {
      const contact = crmContactById(draft.contactId); if (!contact) return;
      $(`[data-crm-drafts-dialog]`).close();
      openCrmEmail(contact);
      $(`[data-crm-email-subject]`).value = draft.subject; $(`[data-crm-email-text]`).value = draft.body;
      state.crmView.draftContactId = draft.contactId;
      return;
    }
    const copied = navigator.clipboard?.writeText(draft.body).then(() => true, () => false) || Promise.resolve(false);
    window.open(draft.thread, "_blank", "noopener,noreferrer");
    const where = (await copied) ? "The reply is on the clipboard" : `Copy the reply first:\n\n${draft.body}\n\n`;
    if (confirm(`${where} and the thread to ${draft.name} is open in a new tab. Reply there, paste it and post it.\n\nMark this draft as sent?`))
      await markCrmDraftSent(draft.contactId, true);
  }

  async function markCrmDraftSent(contactId, stamp) {
    const contact = crmContactById(contactId); if (!contact) return;
    const notes = String(contact.notes || ""); const at = crmDraftAt(notes); if (at < 0) return;
    const by = state.session?.name ? ` by ${state.session.name}` : "";
    const body = { notes: notes.slice(0, at) + `DRAFT, sent ${crmDay(Date.now())}${by}.` + notes.slice(at + CRM_DRAFT_MARK.length) };
    if (stamp) body.lastContactedAt = Date.now();
    try { await crmRequest(crmOrganizationPath(`/contacts/${contactId}`), "PATCH", body); crmMessage("notice", "Draft marked sent in the contact's notes."); }
    catch (error) { crmMessage("error", `The draft was not marked sent: ${error.message}`); }
    await loadCrm();
    if ($(`[data-crm-drafts-dialog]`)?.open) renderCrmDrafts();
  }

  function openCrmZohoGuide() {
    const zoho = state.crmView.settings?.zoho || {}; const canManage = state.crmView.canManageSettings;
    $(`[data-crm-zoho-guide-state]`).textContent = zoho.connected
      ? "Zoho Mail is already connected for this organization: use Sync Zoho Mail. This guide is only needed to set it up again."
      : zoho.oneClick ? "One-click connect is on for this server: skip to step A7."
      : "One-click connect is off: the server has no ForkMesh Zoho OAuth client yet. Follow A (server operator) or B (this organization only).";
    const connect = $(`[data-crm-zoho-guide-connect]`);
    connect.disabled = !(canManage && (zoho.oneClick || crmZohoSetupAvailable()) && !zoho.connected);
    connect.title = zoho.oneClick ? "" : crmZohoSetupAvailable() ? "Runs B for you with the browser extension" : "Available once step A is done, or with the browser extension 2.26.0 or newer";
    $(`[data-crm-zoho-guide-settings]`).disabled = !canManage;
    const dialog = $(`[data-crm-zoho-guide-dialog]`); if (!dialog.open) dialog.showModal();
  }

  async function copyCrmZohoGuide(which) {
    const text = which === "redirect" ? CRM_ZOHO_CALLBACK : $(`[data-crm-zoho-guide-secrets]`).textContent;
    try { await navigator.clipboard.writeText(text); crmMessage("notice", which === "redirect" ? "Zoho redirect URI copied." : "secrets.env lines copied: fill in the client id and secret."); }
    catch { crmMessage("error", "The browser did not allow copying; select the text in the guide instead."); }
  }

  function openCrmSettings() {
    const settings = state.crmView.settings || {};
    $(`[data-crm-settings-provider]`).value = settings.providerName || "";
    $(`[data-crm-settings-url]`).value = settings.providerUrlTemplate || "";
    $(`[data-crm-settings-from-name]`).value = settings.fromName || "";
    $(`[data-crm-settings-from-email]`).value = settings.fromEmail || "";
    $(`[data-crm-settings-mail-provider]`).value = settings.mailProvider === "gmail" ? "gmail" : "zoho";
    $(`[data-crm-settings-mail-boxes]`).value = (settings.mailBoxes || []).join(", ");
    const zoho = settings.zoho || {};
    $(`[data-crm-zoho-region]`).value = zoho.region || "com";
    $(`[data-crm-zoho-account]`).value = zoho.accountId || "";
    $(`[data-crm-zoho-client]`).value = "";
    $(`[data-crm-zoho-secret]`).value = "";
    $(`[data-crm-zoho-refresh]`).value = "";
    $(`[data-crm-zoho-code]`).value = "";
    $(`[data-crm-zoho-exchange]`).disabled = !state.crmView.canManageSettings;
    $(`[data-crm-zoho-extension]`)?.classList.toggle("hidden", !crmZohoSetupAvailable() || !state.crmView.canManageSettings);
    $(`[data-crm-zoho-status]`).textContent = zoho.connected
      ? `Connected to account ${zoho.accountId}${zoho.managed ? " through ForkMesh's Zoho app" : ""}${zoho.lastSyncAt ? ` · last synced ${relativeTimeLabel(zoho.lastSyncAt)}` : " · not synced yet"}${zoho.canSend ? " · sends from the mailbox" : " · read only: reconnect and allow sending"}.${zoho.managed ? "" : " Leave secrets blank to keep them unchanged."}`
      : zoho.oneClick
      ? "Not connected. Connect with Zoho signs you in to Zoho and asks to read mail, folders, and the account and to send the CRM's emails. Or enter your own OAuth client below."
      : "Not connected. Use a Zoho OAuth client with ZohoMail.messages.READ, ZohoMail.messages.CREATE, ZohoMail.folders.READ, and ZohoMail.accounts.READ scopes.";
    $(`[data-crm-zoho-disconnect]`).classList.toggle("hidden", !zoho.connected);
    const mastodon = settings.mastodon || {};
    $(`[data-crm-mastodon-account]`).value = mastodon.account ? `@${mastodon.account}` : "";
    $(`[data-crm-mastodon-token]`).value = "";
    $(`[data-crm-mastodon-status]`).textContent = crmMastodonStatus(mastodon);
    $(`[data-crm-mastodon-sync]`).classList.toggle("hidden", !mastodon.connected);
    $(`[data-crm-mastodon-disconnect]`).classList.toggle("hidden", !mastodon.connected);
    $(`[data-crm-zoho-connect]`)?.classList.toggle("hidden", !zoho.oneClick || !state.crmView.canManageSettings);
    const connectLabel = $(`[data-crm-zoho-connect]`);
    if (connectLabel) connectLabel.textContent = zoho.connected ? "Reconnect with Zoho" : "Connect with Zoho";
    const canManage = Boolean(state.crmView.canManageSettings);
    $(`[data-crm-settings-access]`).textContent = canManage ? "Mailtrap keys remain request-only. Zoho refresh credentials are encrypted for organization-wide sync and never shown again." : "Only organization owners and admins can change these shared defaults.";
    $(`[data-crm-settings-save]`).disabled = !canManage;
    $(`[data-crm-plugin-zone]`)?.classList.toggle("hidden", !canManage);
    $(`[data-crm-settings-dialog]`).showModal();
  }

  function crmFlowSlug(label) {
    let base = String(label || "").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "");
    if (!/^[a-z]/.test(base)) base = `pipeline-${base}`.replace(/-+$/, "");
    return base.slice(0, 34).replace(/-+$/, "");
  }

  function crmStageSlug(label) {
    return String(label || "").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 40);
  }

  function crmPipelineEditState() {
    let next = 0;
    const key = () => `draft-${(next += 1)}`;
    const drafts = crmFlows().map((flow) => ({
      key: key(), id: flow.id, name: flow.name || flow.id, icon: flow.icon || "pipeline",
      stages: (flow.stages || []).map((stage) => ({ original: stage, label: crmStageLabel(stage) })),
    }));
    const lists = state.crmView.views.map((view) => {
      const bound = drafts.find((draft) => draft.id && draft.id === view.flow)?.key || "";
      return { id: view.id, name: view.name, owner: view.owner || "", key: bound, initialKey: bound };
    });
    return { drafts, lists, shown: drafts.length ? 0 : -1, page: "pipelines", key };
  }

  function crmPipelineFlowContacts(flowId) {
    return flowId ? state.crmView.contacts.filter((contact) => contact.flow === flowId).length : 0;
  }

  function crmPipelinePlan(edit, contacts) {
    const taken = edit.drafts.filter((draft) => draft.id && String(draft.name || "").trim()).map((draft) => draft.id);
    const flows = [];
    const idByKey = new Map();
    const renamed = new Map();
    for (const draft of edit.drafts) {
      const label = String(draft.name || "").trim().slice(0, 60);
      if (!label) continue;
      let id = draft.id;
      if (!id) {
        const base = crmFlowSlug(label);
        id = base;
        for (let suffix = 2; taken.includes(id); suffix += 1) id = `${base}-${suffix}`;
        taken.push(id);
      }
      idByKey.set(draft.key, id);
      const slugs = [];
      const renames = new Map();
      for (const stage of draft.stages) {
        const slug = crmStageSlug(stage.label);
        if (!CRM_STAGE_SHAPE.test(slug) || slugs.includes(slug)) continue;
        slugs.push(slug);
        if (draft.id && stage.original && stage.original !== slug) renames.set(stage.original, slug);
      }
      if (!slugs.length) slugs.push("lead");
      for (const from of [...renames.keys()]) if (slugs.includes(from)) renames.delete(from);
      if (renames.size) renamed.set(id, renames);
      flows.push({ id, name: label, icon: draft.icon || "pipeline", stages: slugs });
    }
    if (!flows.length) return { error: "Keep at least one pipeline, with a name." };
    const stageEdits = [];
    for (const contact of contacts) {
      const to = renamed.get(String(contact.flow || ""))?.get(String(contact.stage || ""));
      if (to) stageEdits.push({ id: contact.id, stage: to });
    }
    const bindings = edit.lists
      .filter((list) => list.key !== list.initialKey && (!list.key || idByKey.has(list.key)))
      .map((list) => ({ id: list.id, flow: list.key ? idByKey.get(list.key) : "" }));
    return { flows, stageEdits, bindings };
  }

  function crmRemovePipelineDraft(edit, at) {
    const draft = edit.drafts[at];
    if (!draft) return;
    edit.drafts.splice(at, 1);
    for (const list of edit.lists) if (list.key === draft.key) list.key = "";
    edit.shown = edit.drafts.length ? Math.min(at, edit.drafts.length - 1) : -1;
  }

  function crmPipelineBoundNames(edit, key) {
    return edit.lists.filter((list) => list.key === key).map((list) => list.name);
  }

  function renderCrmPipelinesDialog() {
    const edit = state.crmView.pipelineEdit;
    const body = $(`[data-crm-pipelines-body]`);
    if (!edit || !body) return;
    $$(`[data-crm-pipelines-page]`).forEach((button) =>
      button.setAttribute("aria-selected", String(button.dataset.crmPipelinesPage === edit.page)));
    const input = "rounded-md border border-border bg-background px-3 py-2 text-sm text-foreground";
    const small = "rounded-md border border-border px-2 py-1 text-xs text-foreground hover:bg-secondary disabled:opacity-40";
    if (edit.page === "lists") {
      const options = (selected) => `<option value=""${selected ? "" : " selected"}>No pipeline (plain list)</option>`
        + edit.drafts.map((draft) => `<option value="${escapeHtml(draft.key)}"${draft.key === selected ? " selected" : ""}>${escapeHtml(draft.name || "(unnamed)")}</option>`).join("");
      body.innerHTML = `<p class="mb-3 text-xs text-muted-foreground">Pick the pipeline each list shows. A list bound to a pipeline shows every contact in it; a plain list shows only the contacts added to it.${edit.lists.length ? "" : " The organization has no lists yet; each pipeline gets its own tab."}</p>
        ${edit.lists.length ? `<table class="w-full text-left text-xs"><thead class="text-muted-foreground"><tr><th class="px-2 py-1">List</th><th class="px-2 py-1">Owner</th><th class="px-2 py-1">Pipeline</th></tr></thead><tbody>${edit.lists.map((list, index) => `<tr class="border-t border-border"><td class="px-2 py-1.5 font-medium">${escapeHtml(list.name)}</td><td class="px-2 py-1.5 text-muted-foreground">${escapeHtml(list.owner)}</td><td class="px-2 py-1.5"><select data-crm-pipeline-list="${index}" aria-label="Pipeline for ${escapeHtml(list.name)}" class="${input}">${options(list.key)}</select></td></tr>`).join("")}</tbody></table>` : ""}`;
      return;
    }
    const draft = edit.drafts[edit.shown] || null;
    const people = draft ? crmPipelineFlowContacts(draft.id) : 0;
    const bound = draft ? crmPipelineBoundNames(edit, draft.key) : [];
    const peopleLabel = people === 1 ? "1 contact" : `${people} contacts`;
    body.innerHTML = `<div class="crm-pipelines-grid">
      <div class="grid content-start gap-2">
        <div class="grid gap-1">${edit.drafts.map((item, index) => `<button type="button" data-crm-pipeline-pick="${index}" aria-pressed="${index === edit.shown}" class="flex items-center gap-2 rounded-md border px-2 py-1.5 text-left text-xs ${index === edit.shown ? "border-primary bg-secondary text-foreground" : "border-border text-muted-foreground hover:text-foreground"}">${crmFlowIcon(item.icon)}<span data-crm-pipeline-pick-name class="truncate">${escapeHtml(item.name || "(unnamed)")}</span></button>`).join("")}</div>
        <div class="flex flex-wrap gap-1"><button type="button" data-crm-pipeline-act="add" class="${small}">Add</button><button type="button" data-crm-pipeline-act="up" class="${small}"${edit.shown > 0 ? "" : " disabled"}>Up</button><button type="button" data-crm-pipeline-act="down" class="${small}"${edit.shown >= 0 && edit.shown < edit.drafts.length - 1 ? "" : " disabled"}>Down</button><button type="button" data-crm-pipeline-act="remove" class="${small} text-destructive"${draft ? "" : " disabled"}>Remove</button></div>
      </div>
      ${draft ? `<div class="grid content-start gap-3">
        <label class="grid gap-1 text-xs text-muted-foreground">Name<input data-crm-pipeline-draft-name maxlength="60" value="${escapeHtml(draft.name)}" class="${input}" /></label>
        <label class="grid gap-1 text-xs text-muted-foreground">Type<select data-crm-pipeline-draft-icon class="${input}">${crmFlowIconOptions(draft.icon)}</select></label>
        <div class="grid gap-1 text-xs text-muted-foreground">Stages
          <div data-crm-pipeline-stages class="grid gap-1">${draft.stages.map((stage, index) => `<div data-crm-stage-row="${index}" class="flex items-center gap-1"><input data-crm-stage-label="${index}" maxlength="60" value="${escapeHtml(stage.label)}" aria-label="Stage ${index + 1}" class="${input} min-w-0 flex-1 py-1" /><button type="button" data-crm-stage-act="up" class="${small}"${index ? "" : " disabled"} aria-label="Move up">↑</button><button type="button" data-crm-stage-act="down" class="${small}"${index < draft.stages.length - 1 ? "" : " disabled"} aria-label="Move down">↓</button><button type="button" data-crm-stage-act="remove" class="${small} text-destructive">Remove</button></div>`).join("")}</div>
          <button type="button" data-crm-stage-act="add" class="${small} w-fit">Add stage</button>
          <span>In order, first to last. Rename a stage in place or move it with the arrows; contacts at a renamed stage move with it.</span>
        </div>
        <p class="text-xs text-muted-foreground"><span class="font-semibold">Lists</span> · ${escapeHtml(bound.length
          ? `${peopleLabel} · shown on ${bound.join(", ")}`
          : `${peopleLabel} · no list is bound to it, so it has a tab of its own. Bind lists on the Lists page.`)}</p>
      </div>` : `<p class="text-xs text-muted-foreground">No pipeline is picked.</p>`}
    </div>`;
  }

  function openCrmPipelineSettings(focusFlow = "") {
    const dialog = $(`[data-crm-pipelines-dialog]`); if (!dialog) return;
    closeCrmMenu();
    if (!state.crmView.canManageSettings) {
      crmMessage("error", "Only organization owners and admins can edit pipelines.");
      return;
    }
    const edit = crmPipelineEditState();
    const focus = edit.drafts.findIndex((draft) => focusFlow && draft.id === focusFlow);
    if (focus >= 0) edit.shown = focus;
    state.crmView.pipelineEdit = edit;
    renderCrmPipelinesDialog();
    if (!dialog.open) dialog.showModal();
    if (focus >= 0) $(`[data-crm-stage-label]`)?.focus();
  }

  function handleCrmPipelinesClick(event) {
    const edit = state.crmView.pipelineEdit; if (!edit) return;
    const page = event.target.closest("[data-crm-pipelines-page]");
    if (page) { edit.page = page.dataset.crmPipelinesPage === "lists" ? "lists" : "pipelines"; renderCrmPipelinesDialog(); return; }
    const pick = event.target.closest("[data-crm-pipeline-pick]");
    if (pick) { edit.shown = Number(pick.dataset.crmPipelinePick); renderCrmPipelinesDialog(); return; }
    const act = event.target.closest("[data-crm-pipeline-act]")?.dataset.crmPipelineAct;
    const draft = edit.drafts[edit.shown];
    if (act === "add") {
      if (edit.drafts.length >= CRM_MAX_FLOWS) { alert(`An organization can have ${CRM_MAX_FLOWS} pipelines. Remove one first.`); return; }
      edit.drafts.push({ key: edit.key(), id: "", name: "New pipeline", icon: "pipeline",
        stages: ["New", "In progress", "Complete"].map((label) => ({ original: "", label })) });
      edit.shown = edit.drafts.length - 1;
      renderCrmPipelinesDialog();
      $(`[data-crm-pipeline-draft-name]`)?.select();
      return;
    }
    if ((act === "up" || act === "down") && draft) {
      const to = edit.shown + (act === "up" ? -1 : 1);
      if (to < 0 || to >= edit.drafts.length) return;
      [edit.drafts[edit.shown], edit.drafts[to]] = [edit.drafts[to], edit.drafts[edit.shown]];
      edit.shown = to;
      renderCrmPipelinesDialog();
      return;
    }
    if (act === "remove" && draft) {
      if (edit.drafts.length === 1) { alert("An organization keeps at least one pipeline. Add another first."); return; }
      const contacts = crmPipelineFlowContacts(draft.id);
      const bound = crmPipelineBoundNames(edit, draft.key).length;
      if ((contacts > 0 || bound > 0) && !confirm(`${contacts} contacts are in “${draft.name}” and ${bound} lists show it. Removing it leaves those contacts in the organization without a pipeline, and the lists become plain lists. Remove it?`)) return;
      crmRemovePipelineDraft(edit, edit.shown);
      renderCrmPipelinesDialog();
      return;
    }
    const stageAct = event.target.closest("[data-crm-stage-act]")?.dataset.crmStageAct;
    if (!stageAct || !draft) return;
    if (stageAct === "add") {
      draft.stages.push({ original: "", label: "New stage" });
      renderCrmPipelinesDialog();
      const inputs = $$(`[data-crm-stage-label]`);
      inputs[inputs.length - 1]?.select();
      return;
    }
    const at = Number(event.target.closest("[data-crm-stage-row]")?.dataset.crmStageRow);
    const stage = draft.stages[at];
    if (!stage) return;
    if (stageAct === "remove") {
      const contacts = draft.id && stage.original
        ? state.crmView.contacts.filter((contact) => contact.flow === draft.id && contact.stage === stage.original).length : 0;
      if (contacts > 0 && !confirm(`${contacts} contacts are at “${stage.label}”. They keep that stage until they are moved to another one. Remove it?`)) return;
      draft.stages.splice(at, 1);
    } else {
      const to = at + (stageAct === "up" ? -1 : 1);
      if (to < 0 || to >= draft.stages.length) return;
      [draft.stages[at], draft.stages[to]] = [draft.stages[to], draft.stages[at]];
    }
    renderCrmPipelinesDialog();
  }

  function handleCrmPipelinesInput(event) {
    const edit = state.crmView.pipelineEdit; if (!edit) return;
    const draft = edit.drafts[edit.shown];
    if (event.target.matches("[data-crm-pipeline-list]")) {
      const list = edit.lists[Number(event.target.dataset.crmPipelineList)];
      if (list) list.key = event.target.value;
      return;
    }
    if (!draft) return;
    if (event.target.matches("[data-crm-pipeline-draft-name]")) {
      draft.name = event.target.value.trim();
      const label = $(`[data-crm-pipeline-pick="${edit.shown}"] [data-crm-pipeline-pick-name]`);
      if (label) label.textContent = draft.name || "(unnamed)";
    } else if (event.target.matches("[data-crm-pipeline-draft-icon]")) {
      draft.icon = event.target.value;
      if (event.type === "change") renderCrmPipelinesDialog();
    } else if (event.target.matches("[data-crm-stage-label]")) {
      const stage = draft.stages[Number(event.target.dataset.crmStageLabel)];
      if (stage) stage.label = event.target.value.trim();
    }
  }

  async function saveCrmPipelines(event) {
    event.preventDefault();
    const edit = state.crmView.pipelineEdit; if (!edit) return;
    const plan = crmPipelinePlan(edit, state.crmView.contacts);
    if (plan.error) { crmMessage("error", plan.error); return; }
    $(`[data-crm-pipelines-dialog]`)?.close();
    crmMessage("notice", "Saving pipelines…");
    try {
      await crmRequest(crmOrganizationPath("/settings"), "PUT", { flows: plan.flows });
    } catch (error) {
      crmMessage("error", `Pipelines were not saved: ${error.message}`);
      await loadCrm();
      return;
    }
    let failure = "";
    if (plan.stageEdits.length) {
      try { await crmRequest(crmOrganizationPath("/contacts/bulk"), "POST", { edits: plan.stageEdits }); }
      catch (error) { failure = `the contacts at renamed stages were not moved: ${error.message}`; }
    }
    for (const binding of plan.bindings) {
      try { await crmRequest(crmOrganizationPath(`/views/${binding.id}`), "PATCH", { flow: binding.flow }); }
      catch (error) { failure = failure || `a list's pipeline was not changed: ${error.message}`; }
    }
    await loadCrm();
    if (failure) { crmMessage("error", `Pipelines were saved, but ${failure}`); return; }
    const parts = [plan.flows.length === 1 ? "1 pipeline saved" : `${plan.flows.length} pipelines saved`];
    if (plan.stageEdits.length) parts.push(`${plan.stageEdits.length} contacts moved to renamed stages`);
    if (plan.bindings.length) parts.push(`${plan.bindings.length} lists given a new pipeline`);
    crmMessage("notice", `${parts.join("; ")}.`);
  }

  function openCrmPipelineDialog() {
    const dialog = $(`[data-crm-pipeline-dialog]`); if (!dialog) return;
    $(`[data-crm-pipeline-name]`).value = "";
    $(`[data-crm-pipeline-icon]`).innerHTML = crmFlowIconOptions("pipeline");
    $(`[data-crm-pipeline-stages]`).value = "new\nin-progress\ncomplete";
    dialog.showModal(); $(`[data-crm-pipeline-name]`)?.focus();
  }

  async function saveCrmPipeline(event) {
    event.preventDefault();
    const name = String($(`[data-crm-pipeline-name]`).value || "").trim();
    const icon = String($(`[data-crm-pipeline-icon]`).value || "pipeline");
    const stages = $(`[data-crm-pipeline-stages]`).value.split(/[\n,]+/)
      .map((stage) => stage.trim()).filter(Boolean);
    const flows = crmFlows();
    if (!name) return;
    if (flows.length >= CRM_MAX_FLOWS) {
      crmMessage("error", `An organization can have ${CRM_MAX_FLOWS} pipelines. Remove one in CRM settings first.`);
      return;
    }
    const base = crmFlowSlug(name);
    const taken = new Set(flows.map((flow) => flow.id));
    let id = base;
    for (let suffix = 2; taken.has(id); suffix += 1) id = `${base}-${suffix}`;
    try {
      await crmRequest(crmOrganizationPath("/settings"), "PUT", {
        flows: [...flows, { id, name, icon, stages: stages.length ? stages : ["lead"] }],
      });
      const payload = await crmRequest(crmOrganizationPath("/views"), "POST", { name, flow: id });
      $(`[data-crm-pipeline-dialog]`).close();
      if (payload.view?.id) { state.crmView.activeView = payload.view.id; state.crmView.activeShared = ""; }
      crmMessage("notice", `Pipeline "${name}" created.`);
      await loadCrm();
    } catch (error) { crmMessage("error", `Could not create the pipeline: ${error.message}`); }
  }

  async function saveCrmSettings(event) {
    event.preventDefault();
    try {
      const body = {
        providerName: $(`[data-crm-settings-provider]`).value,
        providerUrlTemplate: $(`[data-crm-settings-url]`).value,
        fromName: $(`[data-crm-settings-from-name]`).value,
        fromEmail: $(`[data-crm-settings-from-email]`).value,
        mailProvider: $(`[data-crm-settings-mail-provider]`).value,
        mailBoxes: $(`[data-crm-settings-mail-boxes]`).value.split(",").map((box) => box.trim()).filter(Boolean),
      };
      const zoho = {
          region: $(`[data-crm-zoho-region]`).value,
          accountId: $(`[data-crm-zoho-account]`).value,
          clientId: $(`[data-crm-zoho-client]`).value,
          clientSecret: $(`[data-crm-zoho-secret]`).value,
          refreshToken: $(`[data-crm-zoho-refresh]`).value,
      };
      if (state.crmView.settings?.zoho?.connected || Object.values(zoho).some((value, index) => index > 0 && value)) body.zoho = zoho;
      const mastodon = { account: $(`[data-crm-mastodon-account]`).value.trim(), token: $(`[data-crm-mastodon-token]`).value.trim() };
      if (mastodon.token || (mastodon.account && state.crmView.settings?.mastodon?.account)) body.mastodon = mastodon;
      await crmRequest(crmOrganizationPath("/settings"), "PUT", body);
      for (const selector of ["secret", "refresh", "code"]) $(`[data-crm-zoho-${selector}]`).value = "";
      $(`[data-crm-mastodon-token]`).value = "";
      $(`[data-crm-settings-dialog]`).close(); crmMessage("notice", "CRM settings saved."); await loadCrm();
    } catch (error) { crmMessage("error", `Could not save settings: ${error.message}`); }
  }

  function crmMastodonStatus(mastodon) {
    if (!mastodon.connected) return "Not connected. Add the organization's Mastodon account and an access token with the read:statuses scope (Preferences → Development → New application on its instance) to log the toots it sends to contacts automatically.";
    const checked = mastodon.lastSyncAt ? `last checked ${relativeTimeLabel(mastodon.lastSyncAt)}` : "not checked yet";
    const error = mastodon.lastError ? ` · last check failed: ${mastodon.lastError.replaceAll("_", " ")}` : "";
    return `Connected as @${mastodon.account} · ${checked}${error}. Toots it sends that mention a contact are logged on that contact, private mentions included. Leave the token blank to keep it.`;
  }

  async function syncCrmToots(auto) {
    const button = $(`[data-crm-mastodon-sync]`); if (!auto && button) button.disabled = true;
    try {
      const payload = await crmRequest(crmOrganizationPath("/emails/toots"), "POST", auto ? { auto: true } : {});
      if (!auto) crmMessage("notice", `Toots checked: ${payload.checked || 0} read, ${payload.logged || 0} newly logged.`);
      if (payload.logged || !auto) await loadCrm();
      if (payload.logged && auto) crmMessage("notice", `${payload.logged} new toot${payload.logged === 1 ? "" : "s"} logged on contacts.`);
      if (!auto) $(`[data-crm-mastodon-status]`).textContent = crmMastodonStatus(state.crmView.settings?.mastodon || {});
    } catch (error) { if (!auto) crmMessage("error", `Could not check toots: ${error.message}`); }
    finally { if (button) button.disabled = false; }
  }

  function crmAutoSyncToots() {
    if (!state.crmView.settings?.mastodon?.connected) return;
    const key = state.crmView.organization;
    const last = state.crmView.tootSyncedAt?.[key] || 0;
    if (Date.now() - last < CRM_TOOT_SYNC_MS) return;
    state.crmView.tootSyncedAt = { ...(state.crmView.tootSyncedAt || {}), [key]: Date.now() };
    void syncCrmToots(true);
  }

  async function disconnectCrmMastodon() {
    if (!confirm("Disconnect the Mastodon account? Toots already logged stay on their contacts; new ones are no longer found.")) return;
    try {
      await crmRequest(crmOrganizationPath("/settings"), "PUT", { mastodonDisconnect: true });
      $(`[data-crm-settings-dialog]`).close(); crmMessage("notice", "Mastodon account disconnected."); await loadCrm();
    } catch (error) { crmMessage("error", `Could not disconnect Mastodon: ${error.message}`); }
  }

  async function logCrmToot(contactId) {
    const contact = crmContactById(contactId);
    const url = String(prompt(`Link to the toot to log on ${crmContactLabel(contact || {})}, e.g. https://mastodon.social/@forkmesh/117441498168443197`) || "").trim();
    if (!url) return;
    try {
      const payload = await crmRequest(crmOrganizationPath("/emails/toot"), "POST", { url, contactId });
      crmMessage("notice", payload.relinked ? "That toot was already logged; its record was refreshed." : `Toot logged on ${crmContactLabel(payload.contact || contact || {})}.`);
      await loadCrm();
      if (state.crmView.detailContactId === contactId) renderCrmContactDetail();
    } catch (error) { crmMessage("error", `Could not log the toot: ${error.message}`); }
  }

  function crmZohoActive() {
    const settings = state.crmView.settings || {};
    const mine = settings.zohoMine || null;
    if (!mine?.connected && settings.zoho?.connected) return { zoho: settings.zoho, mine: false };
    return { zoho: mine || settings.zoho || null, mine: Boolean(mine) };
  }

  async function syncCrmZoho(quiet = false, mailbox = "") {
    if (state.crmView.zohoSyncing) return;
    state.crmView.zohoSyncing = true;
    const button = $(`[data-crm-zoho-sync]`); if (button) button.disabled = true;
    renderCrmZohoBar(); renderCrmMyZoho();
    try {
      const body = { limit: 200, ...(quiet ? { auto: true } : {}), ...(mailbox ? { mailbox } : {}) };
      const payload = await crmRequest(crmOrganizationPath("/mail/import"), "POST", body);
      const whose = payload.mailbox === "mine" ? "Your Zoho Mail" : "The organization's Zoho Mail";
      if (!quiet) crmMessage("notice", `${whose} synced: ${payload.created || 0} new, ${payload.updated || 0} refreshed, ${payload.outboundRecorded || 0} sent emails recorded.`);
      state.crmView.zohoSyncing = false;
      if (!payload.skipped) await loadCrm();
    } catch (error) {
      state.crmView.zohoSyncing = false;
      if (!quiet) crmMessage("error", `Zoho Mail sync failed: ${error.message}`);
      await loadCrm();
    } finally {
      state.crmView.zohoSyncing = false;
      if (button) button.disabled = false;
      renderCrmZohoBar(); renderCrmMyZoho();
    }
  }

  const CRM_ZOHO_SYNC_MS = 15 * 60 * 1000;

  function crmAutoSyncZoho() {
    if (state.crmView.sharedOnly) return;
    const settings = state.crmView.settings || {};
    const org = state.crmView.organization;
    const due = (zoho, key) => {
      if (!zoho?.connected) return false;
      const tried = Math.max(Number(zoho.lastSyncAt) || 0, Number(zoho.lastSyncErrorAt) || 0, state.crmView.zohoSyncedAt?.[key] || 0);
      if (Date.now() - tried < CRM_ZOHO_SYNC_MS) return false;
      state.crmView.zohoSyncedAt = { ...(state.crmView.zohoSyncedAt || {}), [key]: Date.now() };
      return true;
    };
    const mailboxes = [due(settings.zohoMine, `${org}/mine`) && "mine", due(settings.zoho, org) && "organization"].filter(Boolean);
    if (!mailboxes.length) return;
    void (async () => { for (const mailbox of mailboxes) await syncCrmZoho(true, mailbox); })();
  }

  const CRM_ZOHO_STEP_BUTTONS = { connect: "Connect Zoho Mail", reconnect: "Reconnect Zoho Mail", sync: "Sync now", guide: "Open the setup guide" };

  function crmZohoBarBadge(zoho, steps) {
    if (!zoho.connected) return ["text-attention", "Not connected"];
    if (!zoho.canSend) return ["text-attention", "Read only: cannot send yet"];
    if (steps.some((step) => step.key === "sync_failed")) return ["text-attention", "Last sync failed"];
    return ["text-success", "Reads and sends"];
  }

  function crmZohoStateParts(zoho) {
    if (!zoho.connected) return ["the CRM cannot read or send from your mailbox yet"];
    const parts = [`account ${zoho.accountId}`];
    parts.push(state.crmView.zohoSyncing ? "syncing…" : zoho.lastSyncAt ? `last synced ${relativeTimeLabel(zoho.lastSyncAt)}` : "never synced");
    if (zoho.lastSentAt) parts.push(`last sent ${relativeTimeLabel(zoho.lastSentAt)}`);
    return parts;
  }

  function renderCrmZohoBar() {
    const bar = $(`[data-crm-zoho-bar]`); if (!bar) return;
    const { zoho, mine } = crmZohoActive();
    const steps = Array.isArray(zoho?.steps) ? zoho.steps : [];
    bar.classList.toggle("hidden", !zoho || !steps.length || !state.crmView.installed || Boolean(state.crmView.sharedOnly) || state.crmView.section === "zoho");
    if (!zoho) return;
    const [tone, label] = crmZohoBarBadge(zoho, steps);
    $(`[data-crm-zoho-bar-title]`).textContent = mine ? "Your Zoho Mail" : "Organization Zoho Mail";
    const badge = $(`[data-crm-zoho-bar-badge]`);
    badge.className = `font-semibold ${tone}`; badge.textContent = label;
    $(`[data-crm-zoho-bar-state]`).textContent = crmZohoStateParts(zoho).join(" · ");
    const canManage = Boolean(state.crmView.canManageSettings);
    const mailbox = mine ? "mine" : "organization";
    const list = $(`[data-crm-zoho-bar-steps]`);
    list.hidden = steps.length === 0;
    list.innerHTML = steps.map((step, index) => {
      const allowed = step.action === "sync" ? zoho.connected : mine || canManage;
      const action = allowed
        ? `<button data-crm-zoho-step="${escapeHtml(step.action)}" data-crm-zoho-mailbox="${mailbox}" type="button" ${state.crmView.zohoSyncing && step.action === "sync" ? "disabled" : ""} class="ml-2 h-7 rounded-md border border-border bg-secondary px-2.5 text-xs font-semibold text-foreground hover:bg-background disabled:opacity-50">${escapeHtml(mine && step.action === "guide" ? CRM_ZOHO_STEP_BUTTONS.connect : CRM_ZOHO_STEP_BUTTONS[step.action] || "Open CRM settings")}</button>`
        : `<button data-crm-zoho-step="mine" type="button" class="ml-2 h-7 rounded-md border border-border bg-secondary px-2.5 text-xs font-semibold text-foreground hover:bg-background">Connect my own mailbox</button>`;
      return `<li class="text-foreground"><span class="text-muted-foreground" style="font-variant-numeric:tabular-nums">${index + 1}.</span> ${escapeHtml(step.text)}${action}</li>`;
    }).join("");
  }

  function runCrmZohoStep(action, mailbox = "mine") {
    const settings = state.crmView.settings || {};
    const mine = mailbox === "mine";
    const zoho = (mine ? settings.zohoMine : settings.zoho) || {};
    const connect = action === "connect" || action === "reconnect" || (mine && action === "guide");
    if (action === "sync") void syncCrmZoho(false, mailbox);
    else if (action === "mine") selectCrmSection("zoho", true);
    else if (connect && zoho.oneClick) void connectCrmZoho(mine);
    else if (connect && mine) { selectCrmSection("zoho"); $(`[data-crm-my-zoho-client]`)?.focus(); }
    else if (connect && crmZohoSetupAvailable()) startCrmZohoSetup();
    else if (action === "reconnect") openCrmSettings();
    else openCrmZohoGuide();
  }

  function renderCrmMyZohoDot() {
    const dot = $(`[data-crm-my-zoho-dot]`); if (!dot) return;
    const mine = state.crmView.settings?.zohoMine;
    const warn = Boolean(mine?.connected && (mine.steps || []).length);
    dot.dataset.tone = !mine?.connected ? "" : warn ? "warn" : "ok";
    dot.title = !mine?.connected ? "Your Zoho Mail is not connected" : warn ? "Your Zoho Mail needs attention" : "Your Zoho Mail reads and sends";
  }

  function renderCrmMyZoho() {
    const panel = $(`[data-crm-my-zoho]`); if (!panel) return;
    const settings = state.crmView.settings || {};
    const mine = settings.zohoMine || {};
    const org = settings.zoho || {};
    const steps = Array.isArray(mine.steps) ? mine.steps : [];
    const [tone, label] = crmZohoBarBadge(mine, steps);
    const badge = $(`[data-crm-my-zoho-badge]`);
    badge.className = `text-xs font-semibold ${tone}`; badge.textContent = label;
    $(`[data-crm-my-zoho-status]`).textContent = mine.connected
      ? `${crmZohoStateParts(mine).join(" · ")} · ${mine.managed ? "signed in through ForkMesh's Zoho app" : "connected with your Self Client"}${mine.region ? ` (${mine.region})` : ""}.`
      : mine.oneClick
      ? "Not connected. Connect with Zoho signs you in to Zoho and asks to read your mail, folders and account and to send the CRM's emails. Or use a Self Client code below."
      : "Not connected. This server has no ForkMesh Zoho app, so connect with a Self Client code below.";
    const connect = $(`[data-crm-my-zoho-connect]`);
    connect.classList.toggle("hidden", !mine.oneClick);
    connect.textContent = mine.connected ? "Reconnect with Zoho" : "Connect with Zoho";
    const sync = $(`[data-crm-my-zoho-sync]`);
    sync.classList.toggle("hidden", !mine.connected);
    sync.disabled = Boolean(state.crmView.zohoSyncing);
    $(`[data-crm-my-zoho-disconnect]`).classList.toggle("hidden", !mine.connected);
    const list = $(`[data-crm-my-zoho-steps]`);
    const shown = mine.connected ? steps : [];
    list.hidden = shown.length === 0;
    list.innerHTML = shown.map((step, index) => `<li class="text-foreground"><span class="text-muted-foreground" style="font-variant-numeric:tabular-nums">${index + 1}.</span> ${escapeHtml(step.text)}</li>`).join("");
    const form = $(`[data-crm-my-zoho-form]`);
    if (!form.contains(document.activeElement)) {
      const region = $(`[data-crm-my-zoho-region]`); if (mine.region) region.value = mine.region;
      const client = $(`[data-crm-my-zoho-client]`); if (!client.value && mine.clientId) client.value = mine.clientId;
    }
    $(`[data-crm-org-zoho-status]`).textContent = org.connected
      ? `Organization mailbox: account ${org.accountId}${org.lastSyncAt ? `, last synced ${relativeTimeLabel(org.lastSyncAt)}` : ", never synced"}. ${mine.connected ? "Your own mailbox is used for your sends and syncs." : "Your CRM emails go out from it until you connect your own."}`
      : `No organization mailbox is connected.${state.crmView.canManageSettings ? " Owners and admins can add a shared one in CRM settings." : ""}`;
    const orgSync = $(`[data-crm-org-zoho-sync]`);
    orgSync.classList.toggle("hidden", !org.connected);
    orgSync.disabled = Boolean(state.crmView.zohoSyncing);
    $(`[data-crm-org-zoho-manage]`).classList.toggle("hidden", !state.crmView.canManageSettings);
  }

  async function exchangeCrmMyZohoCode(event) {
    event.preventDefault();
    const button = $(`[data-crm-my-zoho-exchange]`); button.disabled = true;
    try {
      const payload = await crmRequest(crmOrganizationPath("/zoho/exchange"), "POST", {
        mine: true,
        code: $(`[data-crm-my-zoho-code]`).value,
        region: $(`[data-crm-my-zoho-region]`).value,
        clientId: $(`[data-crm-my-zoho-client]`).value,
        clientSecret: $(`[data-crm-my-zoho-secret]`).value,
      });
      for (const selector of ["secret", "code"]) $(`[data-crm-my-zoho-${selector}]`).value = "";
      const zoho = payload.settings?.zohoMine || {};
      crmMessage("notice", `Your Zoho Mail is connected to account ${zoho.accountId} (${zoho.region}). Syncing your mailbox…`);
      await loadCrm();
      if (state.crmView.settings?.zohoMine?.connected) await syncCrmZoho(false, "mine");
    } catch (error) {
      const code = error.payload?.error || "";
      const zohoError = error.payload?.zohoError || "";
      crmMessage("error", CRM_ZOHO_CODE_ERRORS[code] || (code === "zoho_code_rejected"
        ? `Zoho rejected the code in every data center (${zohoError || "no reason given"}). Codes work once and expire in minutes: generate a new one and check the client ID and secret are the Self Client's.`
        : `Could not connect with the code: ${error.message}`));
    } finally { button.disabled = false; }
  }

  async function disconnectCrmMyZoho() {
    if (!confirm("Disconnect your Zoho Mail? Mail already synced stays. Your CRM emails go out through the organization's mailbox or Mailtrap until you connect again.")) return;
    try {
      await crmRequest(crmOrganizationPath("/zoho/disconnect"), "POST", { mine: true });
      crmMessage("notice", "Your Zoho Mail is disconnected."); await loadCrm();
    } catch (error) { crmMessage("error", `Could not disconnect your Zoho Mail: ${error.message}`); }
  }

  const crmZohoSetupAvailable = () => Boolean(document.documentElement.dataset.forkmeshCrmZohoSetup);

  const CRM_ZOHO_SETUP_ERRORS = {
    not_signed_in: "Sign in to the ForkMesh browser extension (its toolbar icon), then press Reconnect Zoho Mail again.",
    organization_admin_required: "Only organization owners and admins can connect Zoho Mail.",
    forbidden_sender: "The browser extension refused the request from this page. Reload the page and try again.",
    extension_unavailable: "The browser extension did not answer. Reload it on chrome://extensions, then this page.",
  };

  function startCrmZohoSetup() {
    if (!state.crmView.canManageSettings) { crmMessage("error", CRM_ZOHO_SETUP_ERRORS.organization_admin_required); return; }
    state.crmView.zohoSetupRequest = `zoho-${Date.now()}`;
    crmMessage("notice", "Opening the Zoho API Console in a new tab. The extension makes a Self Client code there and connects it; sign in to Zoho if it asks.");
    window.postMessage({ source: "forkmesh-crm-page", type: "crm-zoho-setup", organization: state.crmView.organization, request: state.crmView.zohoSetupRequest }, location.origin);
  }

  async function onCrmZohoSetupMessage(event) {
    if (event.source !== window || event.origin !== location.origin || event.data?.source !== "forkmesh-extension") return;
    if (event.data.type === "crm-zoho-setup" && event.data.request === state.crmView.zohoSetupRequest) {
      const response = event.data.response || {};
      if (!response.ok) crmMessage("error", CRM_ZOHO_SETUP_ERRORS[response.error] || `The extension could not start the Zoho setup: ${response.message || response.error || "no answer"}`);
      return;
    }
    if (event.data.type !== "crm-zoho-setup-update") return;
    const setup = event.data.setup || {};
    if (setup.organization && setup.organization !== state.crmView.organization) return;
    if (setup.state === "done") {
      if (state.crmView.zohoSetupDone === setup.id) return;
      state.crmView.zohoSetupDone = setup.id;
      crmMessage("notice", `Zoho Mail connected to account ${setup.accountId || ""}${setup.region ? ` (${setup.region})` : ""}. Syncing the shared boxes…`);
      await loadCrm();
      if (state.crmView.settings?.zoho?.connected) await syncCrmZoho(false, "organization");
    } else if (setup.state === "error") crmMessage("error", `Zoho Mail was not connected: ${setup.message || setup.error}${setup.detail ? ` (${setup.detail})` : ""}. Try again from the card in the Zoho API Console tab.`);
    else if (setup.state === "checking") crmMessage("notice", "The extension has the Self Client code; connecting it to Zoho Mail…");
    else if (setup.state === "stopped") crmMessage("notice", "The Zoho setup was stopped. Nothing was changed.");
  }

  const CRM_ZOHO_CODE_ERRORS = {
    zoho_code_incomplete: "Enter the Self Client's client ID, its client secret and the code.",
    zoho_account_missing: "Zoho accepted the code, but that login has no Zoho Mail account to sync.",
  };

  async function exchangeCrmZohoCode() {
    const button = $(`[data-crm-zoho-exchange]`); button.disabled = true;
    try {
      const payload = await crmRequest(crmOrganizationPath("/zoho/exchange"), "POST", {
        code: $(`[data-crm-zoho-code]`).value,
        region: $(`[data-crm-zoho-region]`).value,
        clientId: $(`[data-crm-zoho-client]`).value,
        clientSecret: $(`[data-crm-zoho-secret]`).value,
      });
      for (const selector of ["secret", "refresh", "code"]) $(`[data-crm-zoho-${selector}]`).value = "";
      const zoho = payload.settings?.zoho || {};
      $(`[data-crm-settings-dialog]`).close();
      crmMessage("notice", `Zoho Mail connected to account ${zoho.accountId} (${zoho.region}). Syncing the shared boxes…`);
      await loadCrm();
      if (state.crmView.settings?.zoho?.connected) await syncCrmZoho(false, "organization");
    } catch (error) {
      const code = error.payload?.error || "";
      const zohoError = error.payload?.zohoError || "";
      crmMessage("error", CRM_ZOHO_CODE_ERRORS[code] || (code === "zoho_code_rejected"
        ? `Zoho rejected the code in every data center (${zohoError || "no reason given"}). Codes work once and expire in minutes: generate a new one and check the client ID and secret are the Self Client's.`
        : `Could not connect with the code: ${error.message}`));
    } finally { button.disabled = !state.crmView.canManageSettings; }
  }

  async function connectCrmZoho(mine = false) {
    try {
      const payload = await crmRequest(crmOrganizationPath("/zoho/connect"), "POST", mine ? { mine: true } : {});
      if (!payload.authorizationUrl) throw new Error("Zoho did not return a sign-in link");
      window.location.assign(payload.authorizationUrl);
    } catch (error) { crmMessage("error", `Could not start the Zoho connection: ${error.message}`); }
  }

  const CRM_ZOHO_RETURNS = {
    connected: ["notice", "Zoho Mail connected. Syncing the shared boxes…"],
    connected_mine: ["notice", "Your Zoho Mail is connected. Syncing your mailbox…"],
    denied: ["error", "Zoho Mail was not connected: access was not allowed in Zoho."],
    expired: ["error", "The Zoho sign-in took too long or was already used. Start Connect Zoho Mail again."],
    forbidden: ["error", "Only organization owners and admins can connect the organization's Zoho Mail. Connect your own under My Zoho Mail."],
    unavailable: ["error", "Zoho one-click connection is not available here. Enter an OAuth client in CRM settings instead."],
    token_rejected: ["error", "Zoho rejected the sign-in. Start Connect Zoho Mail again."],
    account_missing: ["error", "That Zoho login has no Zoho Mail account to sync."],
  };

  function crmZohoSetupParam() {
    const url = new URL(location.href);
    if (url.searchParams.get("zohosetup") !== "1") return false;
    url.searchParams.delete("zohosetup");
    history.replaceState(history.state, "", url);
    return true;
  }

  function crmZohoReturn() {
    const url = new URL(location.href);
    const outcome = url.searchParams.get("zoho");
    if (!outcome) return "";
    url.searchParams.delete("zoho");
    history.replaceState(history.state, "", url);
    const [kind, text] = CRM_ZOHO_RETURNS[outcome] || ["error", "Zoho Mail was not connected."];
    crmMessage(kind, text);
    return outcome;
  }

  async function disconnectCrmZoho() {
    if (!confirm("Disconnect Zoho Mail? Imported CRM mail stays in its boxes, but future syncs stop until credentials are saved again.")) return;
    try {
      await crmRequest(crmOrganizationPath("/settings"), "PUT", { zohoDisconnect: true });
      $(`[data-crm-settings-dialog]`).close(); crmMessage("notice", "Zoho Mail disconnected."); await loadCrm();
    } catch (error) { crmMessage("error", `Could not disconnect Zoho Mail: ${error.message}`); }
  }

  async function moveCrmMail(id, box) {
    try {
      await crmRequest(crmOrganizationPath(`/mail/${id}`), "PATCH", { box });
      const message = state.crmView.mail.find((item) => item.id === id);
      if (message) message.box = box;
      renderCrmMail();
    } catch (error) { crmMessage("error", `Could not move mail: ${error.message}`); await loadCrm(); }
  }

  async function linkCrmEmail(event) {
    event.preventDefault();
    try {
      await crmRequest(crmOrganizationPath("/emails"), "POST", {
        to: $(`[data-crm-link-to]`).value, subject: $(`[data-crm-link-subject]`).value,
        externalId: $(`[data-crm-link-id]`).value, status: $(`[data-crm-link-status]`).value,
        providerUrl: $(`[data-crm-link-url]`).value,
        provider: $(`[data-crm-link-provider]`)?.value || "other",
        direction: $(`[data-crm-link-direction]`)?.value || "outbound",
      });
      $(`[data-crm-link-dialog]`).close(); crmMessage("notice", "External email send linked."); await loadCrm();
    } catch (error) { crmMessage("error", `Could not link send: ${error.message}`); }
  }

  // The sheet a picked CSV lands in. Empty means "make a new tab named after
  // the file", which is what the toolbar's own import button always does.
  let crmImportTarget = "";

  function crmPickCsv(viewId = "") {
    crmImportTarget = String(viewId || "");
    $(`[data-crm-import-file]`)?.click();
  }

  // A CSV is uploaded this many data rows at a time. Each part is its own
  // worker request, so the bar moves as rows are applied and no one request
  // carries the whole file's database writes.
  const CRM_IMPORT_PART_ROWS = 100;
  // Failures the worker already closed with a "failed" ping of its own.
  const CRM_IMPORT_WORKER_FAILURES = ["csv_required", "invalid_csv", "csv_header_required", "too_many_csv_rows"];

  // Split CSV text into records without parsing fields: a newline inside a
  // quoted value stays inside its record. Empty lines are dropped, as the
  // worker's reader drops them, so row numbers agree on both sides.
  function crmCsvRecords(text) {
    const records = [];
    let start = 0; let quoted = false;
    for (let index = 0; index <= text.length; index += 1) {
      const char = text[index];
      if (char === '"') quoted = !quoted;
      else if (index === text.length || (char === "\n" && !quoted)) {
        const record = text.slice(start, index).replace(/\r$/, "");
        if (record) records.push(record);
        start = index + 1;
      }
    }
    return records;
  }

  function crmImportProgress(fileName = "", done = 0, total = 0) {
    const box = $(`[data-crm-import-progress]`); if (!box) return;
    box.classList.toggle("hidden", !fileName);
    if (!fileName) return;
    const percent = total ? Math.min(100, Math.round((done / total) * 100)) : 0;
    $(`[data-crm-import-progress-label]`).textContent = `Importing ${fileName}…`;
    $(`[data-crm-import-progress-count]`).textContent = `${done} of ${total} row${total === 1 ? "" : "s"} · ${percent}%`;
    $(`[data-crm-import-progress-track]`)?.setAttribute("aria-valuenow", String(percent));
    $(`[data-crm-import-progress-bar]`).style.width = `${percent}%`;
  }

  function crmImportErrors(errors = [], skipped = errors.length) {
    const box = $(`[data-crm-import-errors]`); if (!box) return;
    box.classList.toggle("hidden", !errors.length);
    const shown = errors.slice(0, 200);
    $(`[data-crm-import-errors-title]`).textContent = `${skipped} row${skipped === 1 ? "" : "s"} skipped`;
    $(`[data-crm-import-errors-list]`).innerHTML = shown
      .map((item) => `<li>${escapeHtml(item.message || `Row ${item.row}: ${String(item.error || "").replaceAll("_", " ")}`)}</li>`)
      .join("") + (skipped > shown.length ? `<li>…and ${skipped - shown.length} more.</li>` : "");
  }

  // A picked CSV waiting on the import step's column mapping: its records, the
  // organization and sheet it lands in, and the worker's first guess at what
  // each column is.
  let crmImportPending = null;
  const CRM_IMPORT_PREVIEW_ROWS = 20;
  // Columns that tell one contact from another; a row needs at least one.
  const CRM_IMPORT_IDENTITY_FIELDS = ["name", "first_name", "last_name", "email", "company"];

  // What a CSV column can be imported as, with a split name's two halves right
  // after the name they make up.
  function crmImportTargets() {
    return CRM_BASE_COLUMNS.flatMap((column) => column.field === "name"
      ? [column, { field: "first_name", label: "First name" }, { field: "last_name", label: "Last name" }]
      : [column]);
  }

  async function previewCrmCsv(file, sheetName = "") {
    const target = crmImportTarget;
    crmImportTarget = "";
    const input = $(`[data-crm-import-file]`);
    if (!file) return;
    crmMessage("error"); crmMessage("notice"); crmImportErrors();
    if (file.size > 1500000) { crmMessage("error", "CSV files must be 1.5 MB or smaller."); if (input) input.value = ""; return; }
    if (input) input.disabled = true;
    const path = crmOrganizationPath("/contacts/import");
    try {
      const [header = "", ...rows] = crmCsvRecords((await file.text()).replace(/^\uFEFF/, ""));
      const preview = await crmRequest(path, "POST", {
        preview: true, csv: [header, ...rows.slice(0, CRM_IMPORT_PREVIEW_ROWS)].join("\r\n") + "\r\n",
      });
      crmImportPending = { file, target, path, header, rows, sheetName, delimiter: preview.delimiter || "", columns: preview.columns || [] };
      renderCrmImportMap();
      $(`[data-crm-import-map-dialog]`).showModal();
    } catch (error) { crmMessage("error", `CSV import failed: ${error.message}`); }
    finally { if (input) { input.value = ""; input.disabled = false; } }
  }

  function renderCrmImportMap() {
    const pending = crmImportPending; if (!pending) return;
    const targets = crmImportTargets();
    const count = pending.rows.length;
    $(`[data-crm-import-map-title]`).textContent = `Map columns: ${pending.file.name || "contacts.csv"}`;
    // A new import asks what its tab is called, starting from the file's name;
    // re-importing into an open tab keeps that tab's name.
    const nameInput = $(`[data-crm-import-map-name]`);
    $(`[data-crm-import-map-name-row]`)?.classList.toggle("hidden", Boolean(pending.target));
    if (nameInput) { nameInput.value = pending.sheetName || crmSheetNameFromFile(pending.file.name); nameInput.required = !pending.target; }
    $(`[data-crm-import-map-rows]`).innerHTML = pending.columns.map((column) => {
      const header = column.header || `Column ${column.index + 1}`;
      const samples = (column.samples || []).join(" · ");
      const options = [{ label: `Its own column: ${header}`, field: "custom" }, { label: "Don't import", field: "" }, ...targets]
        .map((option) => `<option value="${escapeHtml(option.field)}"${option.field === column.field ? " selected" : ""}>${escapeHtml(option.label)}</option>`).join("");
      return `<tr class="border-b border-border"><td class="whitespace-nowrap px-3 py-2 font-medium">${escapeHtml(header)}</td><td class="px-3 py-2 text-xs text-muted-foreground" style="max-width:18rem;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title="${escapeHtml(samples)}">${escapeHtml(samples) || "—"}</td><td class="px-3 py-2"><select data-crm-import-map-field aria-label="Import ${escapeHtml(header)} as" class="w-full rounded-md border border-border bg-background px-2 py-1 text-sm text-foreground">${options}</select></td></tr>`;
    }).join("");
    $(`[data-crm-import-map-submit]`).textContent = `Import ${count} row${count === 1 ? "" : "s"}`;
    renderCrmImportMapNote();
  }

  // The worker's own default for a sheet opened by an import (_sheet_name).
  function crmSheetNameFromFile(fileName) {
    const base = String(fileName || "").split("/").pop().replace(/\.[A-Za-z0-9]{1,8}$/, "");
    return base.replace(/[_-]+/g, " ").trim().slice(0, 60) || "Imported contacts";
  }

  function crmImportMapping() {
    return $$(`[data-crm-import-map-field]`).map((select) => select.value);
  }

  // Email is optional, but a row still needs something to be known by, and a
  // file without emails cannot be matched to the contacts it imported before.
  function renderCrmImportMapNote() {
    const mapping = crmImportMapping();
    const identified = mapping.some((field) => CRM_IMPORT_IDENTITY_FIELDS.includes(field));
    const note = $(`[data-crm-import-map-note]`);
    const text = !identified
      ? "Import a column as Name, Email or Company, or every row will be skipped."
      : mapping.includes("email") ? ""
      : mapping.includes("mastodon") ? "No Email column: rows are matched by their Mastodon handle, so importing this file again updates them."
      : "No Email column: every row is added as a new contact, so importing this file again adds its rows again.";
    note.textContent = text;
    note.classList.toggle("hidden", !text);
    note.classList.toggle("text-destructive", !identified);
    note.classList.toggle("text-muted-foreground", identified);
    $(`[data-crm-import-map-submit]`).disabled = !identified;
  }

  function submitCrmImportMap(event) {
    event.preventDefault();
    const pending = crmImportPending; if (!pending) return;
    crmImportPending = null;
    $(`[data-crm-import-map-dialog]`).close();
    const sheetName = String($(`[data-crm-import-map-name]`)?.value || "").trim().slice(0, 60);
    void importCrmCsv({ ...pending, mapping: crmImportMapping(), sheetName });
  }

  // --- Mastodon followers -------------------------------------------------
  // The worker cannot reach arbitrary instances, so the browser reads the
  // public follower list straight from the account's instance (no cookies,
  // as the profile page does) and hands it to the CSV import as a file: the
  // same column mapping, tab naming, split upload and pings. Rows carry no
  // email, so the worker matches them by Mastodon handle on a re-import.
  const CRM_MASTODON_PAGE = 80;
  const CRM_MASTODON_MAX = 2000;
  const CRM_MASTODON_TIMEOUT_MS = 15000;
  let crmMastodonTarget = "";
  let crmMastodonBusy = false;

  function openCrmMastodonDialog(viewId = "") {
    closeCrmMenu();
    $(`[data-crm-actions]`)?.removeAttribute("open");
    crmMastodonTarget = String(viewId || "");
    crmMastodonStatus();
    $(`[data-crm-mastodon-dialog]`)?.showModal();
    $(`[data-crm-mastodon-handle]`)?.focus();
  }

  function crmMastodonStatus(text = "", failed = false) {
    const box = $(`[data-crm-mastodon-status]`); if (!box) return;
    box.textContent = text;
    box.classList.toggle("hidden", !text);
    box.classList.toggle("text-destructive", failed);
    box.classList.toggle("text-muted-foreground", !failed);
  }

  // "@grace@mastodon.social", "grace@mastodon.social" or a profile link, as
  // { user, host }; null when it names no account.
  function crmMastodonAccount(text) {
    const url = crmMastodonUrl(String(text || "").trim().replace(/^https?:\/\//i, ""));
    const match = url.match(/^https:\/\/([^/]+)\/@([\w.-]{1,64})(?:@([^/]+))?$/);
    if (!match) return null;
    // A profile link to a remote account (host/@name@home) is read on the
    // account's own instance, where its full follower list lives.
    return match[3] ? { user: match[2], host: match[3].toLowerCase() } : { user: match[2], host: match[1].toLowerCase() };
  }

  async function crmMastodonFetch(url) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), CRM_MASTODON_TIMEOUT_MS);
    try {
      const response = await fetch(url, { credentials: "omit", cache: "no-store", headers: { accept: "application/json" }, signal: controller.signal });
      if (!response.ok) {
        const error = new Error(response.status === 404 ? "account not found" : `the instance answered ${response.status}`);
        error.status = response.status; throw error;
      }
      return { json: await response.json(), link: response.headers.get("link") || "" };
    } catch (error) {
      if (error.name === "AbortError") throw new Error("the instance did not answer in time");
      if (error instanceof TypeError) throw new Error("the instance could not be reached from this browser");
      throw error;
    } finally { clearTimeout(timer); }
  }

  // The rel="next" page of a Mastodon Link header, kept on the same instance.
  function crmMastodonNext(link, host) {
    const next = String(link || "").split(",").map((part) => part.match(/<([^>]+)>\s*;\s*rel="?next"?/i)).find(Boolean);
    if (!next) return "";
    try {
      const url = new URL(next[1]);
      return url.protocol === "https:" && url.host.toLowerCase() === host ? url.href : "";
    } catch { return ""; }
  }

  function crmMastodonText(html) {
    const body = new DOMParser().parseFromString(`<body>${String(html || "").replace(/<br\s*\/?>|<\/p>\s*<p>/gi, " ")}</body>`, "text/html").body;
    return String(body?.textContent || "").replace(/\s+/g, " ").trim();
  }

  function crmCsvCell(value) {
    const text = String(value ?? "");
    return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
  }

  // One CSV row per follower. A local follower's acct has no instance, so the
  // instance the list was read from is added to make every handle full.
  function crmMastodonFollowerRow(account, host) {
    const acct = String(account?.acct || account?.username || "");
    const handle = acct.includes("@") ? `@${acct}` : `@${acct}@${host}`;
    const name = String(account?.display_name || "").replace(/:[\w+-]+:/g, "").replace(/\s+/g, " ").trim() || String(account?.username || acct);
    return [name, handle, crmMastodonText(account?.note)].map(crmCsvCell).join(",");
  }

  async function crmMastodonFollowers({ user, host }, progress) {
    const lookup = await crmMastodonFetch(`https://${host}/api/v1/accounts/lookup?acct=${encodeURIComponent(user)}`);
    const id = String(lookup.json?.id || "");
    if (!/^[\w-]{1,64}$/.test(id)) throw new Error("account not found");
    const total = Number(lookup.json?.followers_count) || 0;
    const seen = new Set();
    const rows = [];
    let url = `https://${host}/api/v1/accounts/${encodeURIComponent(id)}/followers?limit=${CRM_MASTODON_PAGE}`;
    while (url && rows.length < CRM_MASTODON_MAX) {
      const page = await crmMastodonFetch(url);
      const accounts = Array.isArray(page.json) ? page.json : [];
      for (const account of accounts) {
        const acct = String(account?.acct || "").trim();
        const key = `${String(account?.id || "").trim()}${acct}`;
        if (!acct || seen.has(key) || rows.length >= CRM_MASTODON_MAX) continue;
        seen.add(key);
        rows.push(crmMastodonFollowerRow(account, host));
      }
      progress(rows.length, total);
      url = accounts.length ? crmMastodonNext(page.link, host) : "";
    }
    return { rows, total, account: lookup.json };
  }

  async function submitCrmMastodon(event) {
    event.preventDefault();
    if (crmMastodonBusy) return;
    const input = $(`[data-crm-mastodon-handle]`);
    const typed = crmMastodonAccount(input?.value);
    const account = typed && { user: typed.user.toLowerCase(), host: typed.host };
    if (!account) { crmMastodonStatus("Enter a handle like @name@mastodon.social or a profile link.", true); return; }
    const submit = $(`[data-crm-mastodon-submit]`);
    crmMastodonBusy = true;
    if (submit) submit.disabled = true;
    const handle = `@${account.user}@${account.host}`;
    crmMastodonStatus(`Reading the followers of ${handle}…`);
    try {
      const { rows, total } = await crmMastodonFollowers(account, (done, of) =>
        crmMastodonStatus(`Reading the followers of ${handle}… ${done}${of ? ` of ${of}` : ""}`));
      if (!rows.length) {
        crmMastodonStatus(total ? `${handle} hides its followers, so there is nothing to import.` : `${handle} has no followers yet.`, true);
        return;
      }
      $(`[data-crm-mastodon-dialog]`)?.close();
      const csv = ["Name,Mastodon,Notes", ...rows].join("\r\n") + "\r\n";
      const file = new File([csv], `mastodon-followers-${account.user}@${account.host}.csv`, { type: "text/csv" });
      crmImportTarget = crmMastodonTarget;
      await previewCrmCsv(file, `Followers of ${handle}`.slice(0, 60));
      if (rows.length >= CRM_MASTODON_MAX && total > rows.length) {
        crmMessage("notice", `Importing the ${CRM_MASTODON_MAX} most recent of ${handle}'s ${total} followers; one import takes at most ${CRM_MASTODON_MAX}.`);
      }
    } catch (error) {
      crmMastodonStatus(`Could not read the followers of ${handle}: ${error.message}.`, true);
    } finally {
      crmMastodonBusy = false;
      if (submit) submit.disabled = false;
    }
  }

  async function importCrmCsv({ file, target, path, header, rows, delimiter, mapping, sheetName = "" }) {
    const input = $(`[data-crm-import-file]`);
    if (input) input.disabled = true;
    crmMessage("error"); crmMessage("notice"); crmImportErrors();
    // Every part goes to the organization the import started in, even if the
    // picker moves on while it runs, and reads the columns as they were mapped.
    const fileName = file.name || "contacts.csv";
    const batch = {
      id: globalThis.crypto?.randomUUID?.() || `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 12)}`,
      part: 0, parts: Math.max(1, Math.ceil(rows.length / CRM_IMPORT_PART_ROWS)),
      offset: 0, rows: rows.length, delimiter, created: 0, updated: 0, skipped: 0,
    };
    const errors = [];
    let viewId = target;
    let payload = {};
    crmImportProgress(fileName, 0, rows.length);
    try {
      for (; batch.part < batch.parts; batch.part += 1) {
        const slice = rows.slice(batch.offset, batch.offset + CRM_IMPORT_PART_ROWS);
        payload = await crmRequest(path, "POST", {
          csv: [header, ...slice].join("\r\n") + "\r\n", fileName: file.name || "contacts.csv", sheetName, viewId, mapping,
          import: { ...batch, errors: errors.slice(0, 3).map(({ row, error }) => ({ row, error })) },
        });
        // Later parts extend the sheet the first part opened, read with the
        // delimiter the first part detected.
        if (payload.viewId) viewId = payload.viewId;
        batch.delimiter = payload.delimiter || batch.delimiter;
        batch.created += payload.created || 0; batch.updated += payload.updated || 0; batch.skipped += payload.skipped || 0;
        errors.push(...(payload.errors || []));
        batch.offset += slice.length;
        crmImportProgress(fileName, batch.offset, rows.length);
        crmImportErrors(errors, batch.skipped);
      }
      // Land on the tab the import just filled, so its columns - including any
      // the file brought that the CRM has no built-in home for - are on screen.
      if (payload.viewId) state.crmView.activeView = payload.viewId;
      const sheet = payload.view?.name || "CSV";
      crmMessage("notice", `${sheet}: ${batch.created} created, ${batch.updated} updated, ${batch.skipped} skipped.`);
      await loadCrm();
    }
    catch (error) {
      const progress = batch.offset ? ` after ${batch.offset} of ${rows.length} rows` : "";
      crmMessage("error", `CSV import failed${progress}: ${error.message}`);
      // A dropped connection or a failed part never reached the worker's own
      // failure ping, so close the import's "started" ping from here.
      if (!CRM_IMPORT_WORKER_FAILURES.includes(error.payload?.error)) {
        void crmRequest(path, "POST", { fileName, viewId, import: { ...batch, errors: errors.slice(0, 3).map(({ row, error: code }) => ({ row, error: code })), failed: error.message } }).catch(() => {});
      }
      if (batch.created || batch.updated) await loadCrm();
    }
    finally {
      crmImportProgress();
      if (input) input.disabled = false;
    }
  }

  async function exportCrmCsv() {
    try {
      const payload = await crmRequest(crmOrganizationPath("/contacts/export"));
      const url = URL.createObjectURL(new Blob([payload.csv || ""], { type: "text/csv;charset=utf-8" }));
      const link = document.createElement("a"); link.href = url; link.download = payload.filename || "contacts.csv"; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000);
    } catch (error) { crmMessage("error", `CSV export failed: ${error.message}`); }
  }

  const crmSelection = { ids: new Set(), anchor: "", shown: [] };
  const crmSort = { field: "", descending: false };
  const crmSessionColumns = { organization: "", labels: new Map() };
  const crmPeople = { open: false, sort: "recent", timer: 0 };
  const CRM_MASTODON_FRESH_MS = 15 * 60 * 1000;
  const CRM_MASTODON_IN_FLIGHT = 4;
  const CRM_MASTODON_PER_HOST = 2;
  const CRM_MASTODON_BACKOFF_MS = 5 * 60 * 1000;
  const CRM_REACH_MASTODON_READS = 40;
  const CRM_NEXT_UPCOMING = 5;
  const CRM_NEXT_RENDER_DELAY_MS = 400;
  const crmMastodonLatest = { latest: new Map(), pending: new Set(), queue: [], backoff: new Map(), ids: new Map(), renderTimer: 0 };
  const CRM_NUMERIC_SORT = new Set(["lastContactedAt", "value", "linked-emails", "linked-toots", "mastodon-latest"]);

  function crmSessionColumnKeys() {
    return crmSessionColumns.organization === state.crmView.organization
      ? [...crmSessionColumns.labels.keys()] : [];
  }

  function crmTimestamp(ms) {
    const value = Number(ms) || 0;
    if (value <= 0) return "—";
    const date = new Date(value);
    const pad = (number) => String(number).padStart(2, "0");
    return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`;
  }

  function crmSentAt(email) {
    const sent = Number(email?.sentAt) || 0;
    return sent > 0 ? sent : Number(email?.createdAt) || 0;
  }

  function crmLinkedMessages(contactId, toots) {
    return crmLinkedEmails(contactId)
      .filter((email) => (email.provider === "mastodon") === Boolean(toots))
      .sort((a, b) => crmSentAt(b) - crmSentAt(a));
  }

  function crmLinkedLine(email) {
    const subject = String(email?.subject || "").trim();
    return `${email?.direction === "inbound" ? "↙" : "↗"} ${subject || "(no subject)"} · ${crmTimestamp(crmSentAt(email))}`;
  }

  function crmLinkedCellText(contactId, toots) {
    const messages = crmLinkedMessages(contactId, toots);
    if (!messages.length) return "";
    return crmLinkedLine(messages[0]) + (messages.length > 1 ? `  (+${messages.length - 1} more)` : "");
  }

  function crmMailTile(contact) {
    const email = String(contact.email || "").trim();
    if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) return "";
    const provider = crmMailProvider() === "gmail" ? "Gmail" : "Zoho Mail";
    return `<a href="${escapeHtml(crmMailComposeUrl(email))}" target="_blank" rel="noopener noreferrer" class="crm-tile" title="Write to ${escapeHtml(email)} in ${provider}" aria-label="Write to ${escapeHtml(email)} in ${provider}">&#9993;</a>`;
  }

  function crmComputedCell(contact, column) {
    if (column.field === "mastodon-latest") return crmMastodonLatestCell(crmMastodonKey(contact.mastodon).toLowerCase());
    const toots = column.field === "linked-toots";
    const messages = crmLinkedMessages(contact.id, toots);
    if (!messages.length) return `<span class="block text-xs text-muted-foreground" style="min-width:${column.width}"></span>`;
    const linked = Boolean(crmHttpsUrl(messages[0].providerUrl));
    const title = `${messages.map(crmLinkedLine).join("\n")}\n\n${toots ? "Click to open it on Mastodon." : "Click to open it in the mailbox."}`;
    return `<button type="button" data-crm-linked="${toots ? "toots" : "emails"}" data-crm-linked-id="${escapeHtml(contact.id)}" title="${escapeHtml(title)}" class="block truncate whitespace-nowrap text-left text-xs ${linked ? "crm-link underline" : "text-foreground"}" style="max-width:${column.width};color:${linked ? "var(--dashboard-link)" : "inherit"}">${escapeHtml(crmLinkedCellText(contact.id, toots))}</button>`;
  }

  function openCrmLinked(event, contactId, toots) {
    const messages = crmLinkedMessages(contactId, toots);
    if (!messages.length) return;
    const noun = toots ? "toot" : "email";
    const open = (email) => {
      const url = crmHttpsUrl(email.providerUrl);
      if (!url) { crmMessage("error", `This ${noun} has no valid HTTPS provider link.`); return; }
      window.open(url, "_blank", "noopener,noreferrer");
    };
    if (messages.length === 1) { open(messages[0]); return; }
    openCrmMenu(event, [
      { heading: `${messages.length} linked ${noun}s` },
      ...messages.map((email) => ({
        label: crmLinkedLine(email), disabled: !crmHttpsUrl(email.providerUrl), run: () => open(email),
      })),
    ]);
  }

  function crmMastodonLatestCell(key) {
    if (!key) {
      return `<span class="block whitespace-nowrap text-xs text-muted-foreground" title="Add a Mastodon handle with its instance (@name@instance) to follow the contact's newest post.">—</span>`;
    }
    const latest = crmMastodonLatest.latest.get(key);
    const running = crmMastodonLatest.pending.has(key) || crmMastodonLatest.queue.includes(key);
    let text = ""; let title = ""; let url = "";
    if (!latest) {
      text = running ? `Looking up @${key}…` : "Not read yet";
      title = running ? "" : `@${key} has not been read. Use the refresh button on this row to check it now.`;
    } else if (latest.error) {
      text = "—"; title = `@${key} could not be read: ${latest.error}`;
    } else if (latest.postedAt <= 0) {
      text = "No public posts"; title = `@${key} has no public posts.`;
    } else {
      text = `${crmTimestamp(latest.postedAt)} · ${latest.excerpt}`;
      url = latest.url;
      title = `${crmTimestamp(latest.postedAt)}\n${latest.detail}${url ? `\n\nClick to open ${url}` : ""}`;
    }
    const body = url
      ? `<a href="${escapeHtml(url)}" target="_blank" rel="noopener noreferrer" title="${escapeHtml(title)}" class="crm-link block min-w-0 flex-1 truncate whitespace-nowrap text-xs">${escapeHtml(text)}</a>`
      : `<span title="${escapeHtml(title)}" class="block min-w-0 flex-1 truncate whitespace-nowrap text-xs text-muted-foreground">${escapeHtml(text)}</span>`;
    return `<span data-crm-mastodon-cell="${escapeHtml(key)}" class="flex items-center gap-1" style="width:20rem">${body}<button type="button" data-crm-mastodon-refresh="${escapeHtml(key)}" title="Check @${escapeHtml(key)} for a newer post now" aria-label="Check @${escapeHtml(key)} for a newer post now"${running ? " disabled" : ""} class="crm-tile">&#8635;</button></span>`;
  }

  function crmRepaintMastodonLatest(key) {
    for (const cell of $$(`[data-crm-mastodon-cell]`)) {
      if (cell.dataset.crmMastodonCell === key) cell.outerHTML = crmMastodonLatestCell(key);
    }
  }

  function refreshCrmMastodonNow(key) {
    if (!key || crmMastodonLatest.pending.has(key) || crmMastodonLatest.queue.includes(key)) return;
    const latest = crmMastodonLatest.latest.get(key);
    if (latest) latest.fetchedAt = 0;
    crmMastodonLatest.queue.push(key);
    crmPumpMastodonLatest();
    crmRepaintMastodonLatest(key);
  }

  function crmMastodonHostOf(key) {
    return String(key || "").slice(String(key || "").lastIndexOf("@") + 1);
  }

  function crmPumpMastodonLatest() {
    const now = Date.now();
    const held = [];
    while (crmMastodonLatest.queue.length && crmMastodonLatest.pending.size < CRM_MASTODON_IN_FLIGHT) {
      const key = crmMastodonLatest.queue.shift();
      if (!key || crmMastodonLatest.pending.has(key)) continue;
      const cached = crmMastodonLatest.latest.get(key);
      if (cached && now - cached.fetchedAt < CRM_MASTODON_FRESH_MS) continue;
      const host = crmMastodonHostOf(key);
      const until = crmMastodonLatest.backoff.get(host) || 0;
      if (until > now) {
        crmMastodonLatest.latest.set(key, { ...(cached || { postedAt: 0, excerpt: "", detail: "", url: "" }),
          error: cached?.postedAt > 0 ? "" : `${host} asked for fewer requests; it is read again later`,
          fetchedAt: until - CRM_MASTODON_FRESH_MS });
        crmRepaintMastodonLatest(key);
        continue;
      }
      if ([...crmMastodonLatest.pending].filter((other) => crmMastodonHostOf(other) === host).length >= CRM_MASTODON_PER_HOST) {
        held.push(key);
        continue;
      }
      void readCrmMastodonLatest(key);
    }
    crmMastodonLatest.queue.unshift(...held);
  }

  async function readCrmMastodonLatest(key) {
    crmMastodonLatest.pending.add(key);
    const latest = { fetchedAt: 0, postedAt: 0, excerpt: "", detail: "", url: "", error: "" };
    try {
      const post = ((await crmMastodonUpdates({ key, reuseId: true }, 1)).posts || [])[0];
      if (post) {
        const plain = crmMastodonText(post.html);
        latest.postedAt = post.at;
        latest.excerpt = crmPlainText(plain, 140);
        latest.detail = crmPlainText(plain, 2000);
        latest.url = post.url;
      }
    } catch (error) {
      if (error.status === 404) crmMastodonLatest.ids.delete(key);
      if (error.status === 429) {
        const host = crmMastodonHostOf(key);
        crmMastodonLatest.backoff.set(host, Date.now() + Math.max(error.retryAfter || 0, CRM_MASTODON_BACKOFF_MS));
      }
      latest.error = [404, 406, 410].includes(error.status) ? "the instance knows no such account"
        : error.status === 429 ? "the instance asked for fewer requests; it is read again later"
        : error.name === "TimeoutError" ? "it did not answer in time"
        : error instanceof TypeError ? "the instance could not be reached from this browser"
        : error.message;
    }
    const previous = crmMastodonLatest.latest.get(key);
    latest.fetchedAt = Date.now();
    if (latest.error && previous?.postedAt > 0) Object.assign(latest, { ...previous, fetchedAt: latest.fetchedAt });
    crmMastodonLatest.latest.set(key, latest);
    crmMastodonLatest.pending.delete(key);
    crmRepaintMastodonLatest(key);
    crmScheduleNextRender();
    crmPumpMastodonLatest();
  }

  function crmSortKey(contact, field) {
    if (field === "lastContactedAt") return Number(contact.lastContactedAt) || 0;
    if (field === "value") return Number(contact.value) || 0;
    if (field === "linked-emails" || field === "linked-toots") {
      return crmSentAt(crmLinkedMessages(contact.id, field === "linked-toots")[0]);
    }
    if (field === "mastodon-latest") {
      return crmMastodonLatest.latest.get(crmMastodonKey(contact.mastodon).toLowerCase())?.postedAt || 0;
    }
    if (field === "tags") return (contact.tags || []).join(", ");
    if (CRM_CONTACT_FIELDS.includes(field)) return String(contact[field] ?? "").trim();
    return String((contact.fields || {})[field] ?? "").trim();
  }

  function crmSortContacts(contacts, field = crmSort.field, descending = crmSort.descending) {
    if (!field) return contacts;
    const numeric = CRM_NUMERIC_SORT.has(field);
    return contacts.map((contact, index) => ({ contact, index, key: crmSortKey(contact, field) }))
      .sort((a, b) => {
        let order = 0;
        if (numeric) {
          order = a.key < b.key ? -1 : a.key > b.key ? 1 : 0;
        } else {
          if (!a.key !== !b.key) return a.key ? -1 : 1;
          const left = a.key.toLowerCase(); const right = b.key.toLowerCase();
          order = left < right ? -1 : left > right ? 1 : 0;
        }
        return (descending ? -order : order) || a.index - b.index;
      })
      .map((item) => item.contact);
  }

  function crmSortBy(field) {
    if (!field) return;
    crmSort.descending = crmSort.field === field ? !crmSort.descending : false;
    crmSort.field = field;
    renderCrmContacts();
  }

  function crmContactMatchesQuery(contact, query) {
    if (!query) return true;
    return [...CRM_SEARCH_FIELDS.map((field) => contact[field]),
      contact.flow ? crmFlowLabel(contact.flow) : "",
      ...Object.values(contact.fields || {}),
      ...(contact.tags || [])].some((value) =>
        String(value || "").toLowerCase().includes(query));
  }

  function crmMenuSelection(id) {
    if (!crmSelection.ids.has(id)) {
      crmSelection.ids = new Set([id]);
      crmSelection.anchor = id;
      crmPaintSelection();
    }
    return [...crmSelection.ids].filter((item) => crmContactById(item));
  }

  function crmSelectContact(id, { toggle = false, range = false } = {}) {
    const order = crmSelection.shown;
    if (range && crmSelection.anchor && order.includes(crmSelection.anchor) && order.includes(id)) {
      const from = order.indexOf(crmSelection.anchor); const to = order.indexOf(id);
      const span = order.slice(Math.min(from, to), Math.max(from, to) + 1);
      crmSelection.ids = new Set(toggle ? [...crmSelection.ids, ...span] : span);
    } else if (toggle) {
      if (crmSelection.ids.has(id)) crmSelection.ids.delete(id); else crmSelection.ids.add(id);
      crmSelection.anchor = id;
    } else {
      crmSelection.ids = new Set([id]);
      crmSelection.anchor = id;
    }
    crmPaintSelection();
  }

  function crmPaintSelection() {
    for (const row of $$(`[data-crm-contact-row]`)) {
      const on = crmSelection.ids.has(row.dataset.crmContactRow);
      row.classList.toggle("crm-selected", on);
      const box = row.querySelector("[data-crm-select]");
      if (box) box.checked = on;
    }
    const all = $(`[data-crm-select-all]`);
    if (!all) return;
    const count = crmSelection.ids.size;
    all.checked = count > 0 && count === crmSelection.shown.length;
    all.indeterminate = count > 0 && !all.checked;
    all.title = count
      ? `${count} selected: right-click a selected row for what to do with them`
      : "Select every shown contact";
  }

  function handleCrmSelectionClick(event) {
    if (crmActiveShared()) return;
    const all = event.target.closest("[data-crm-select-all]");
    if (all) {
      crmSelection.ids = all.checked ? new Set(crmSelection.shown) : new Set();
      crmPaintSelection();
      return;
    }
    const row = event.target.closest("[data-crm-contact-row]");
    if (!row) return;
    const box = event.target.closest("[data-crm-select]");
    const modified = event.ctrlKey || event.metaKey || event.shiftKey;
    if (!box && !modified) return;
    if (!box) event.preventDefault();
    event.stopPropagation();
    crmSelectContact(row.dataset.crmContactRow, {
      toggle: Boolean(box) || event.ctrlKey || event.metaKey, range: event.shiftKey,
    });
  }

  function handleCrmSelectionPress(event) {
    if (event.button !== 0 || crmActiveShared() || event.target.closest("[data-crm-select]")) return;
    if ((event.ctrlKey || event.metaKey || event.shiftKey) && event.target.closest("[data-crm-contact-row]")) {
      event.preventDefault();
    }
  }

  async function crmPatchContacts(edits, past, target) {
    const changes = edits.filter((edit) => edit && crmContactById(edit.id));
    if (!changes.length) return;
    const count = changes.length;
    const noun = count === 1 ? "contact" : "contacts";
    const says = (...words) => words.filter(Boolean).join(" ");
    const byId = new Map(changes.map((edit) => [edit.id, edit]));
    state.crmView.contacts = state.crmView.contacts.map((contact) =>
      byId.has(contact.id) ? crmWithContactEdit(contact, byId.get(contact.id)) : contact);
    renderCrm();
    try {
      const payload = await crmRequest(crmOrganizationPath("/contacts/bulk"), "POST", { edits: changes });
      const saved = new Map((payload.contacts || []).map((contact) => [contact.id, contact]));
      state.crmView.contacts = state.crmView.contacts.map((contact) => saved.get(contact.id) || contact);
      renderCrm();
      if (state.crmView.detailContactId) renderCrmContactDetail();
      const missing = (payload.missing || []).length;
      if (missing) {
        crmMessage("error", `${says(`${missing} of ${count} contacts were not`, past, target)}: they no longer exist.`);
        await loadCrm();
        return;
      }
      crmMessage("notice", `${says(String(count), noun, past, target)}.`);
    } catch (error) {
      crmMessage("error", `${says(String(count), noun, "were not", past, target)}: ${error.message}`);
      await loadCrm();
    }
  }

  function crmMarkContacted(ids, stamp) {
    return crmPatchContacts(ids.filter(Boolean).map((id) => ({ id, lastContactedAt: stamp })),
      "marked", `as last contacted ${crmTimestamp(stamp)}`);
  }

  async function assignCrmContactsOwner(ids) {
    const count = ids.length === 1 ? "contact" : `${ids.length} contacts`;
    const answer = window.prompt(`Assign the ${count} to which account? Leave empty to unassign.`,
      String(state.session?.name || ""));
    if (answer === null) return;
    const owner = answer.trim();
    await crmPatchContacts(ids.map((id) => ({ id, owner })),
      owner ? "assigned" : "unassigned", owner ? `to ${owner}` : "");
  }

  async function deleteCrmContacts(ids) {
    const pending = ids.filter((id) => crmContactById(id));
    if (!pending.length || !confirm(`Delete ${pending.length} contacts? Their email history will remain.`)) return;
    let done = 0;
    try {
      for (const id of pending) {
        await crmRequest(crmOrganizationPath(`/contacts/${id}`), "DELETE", {});
        done += 1;
      }
      crmMessage("notice", `${done} contacts deleted.`);
    } catch (error) {
      crmMessage("error", `${done} of ${pending.length} contacts were deleted before an error: ${error.message}`);
    }
    crmSelection.ids = new Set();
    await loadCrm();
  }

  let crmMergeDuplicate = "";

  function openCrmMergePicker(duplicateId) {
    const duplicate = crmContactById(duplicateId);
    const dialog = $(`[data-crm-merge-dialog]`);
    if (!duplicate || !dialog) return;
    const targets = crmMergeTargets(duplicate);
    if (!targets.length) return;
    crmMergeDuplicate = duplicateId;
    $(`[data-crm-merge-intro]`).textContent = `Fold “${crmMergeLabel(duplicate)}” into the contact it duplicates. The one you pick keeps its details; this one fills its blanks, adds its tags, lists, notes and email history, and is then deleted.`;
    $(`[data-crm-merge-search]`).value = "";
    $(`[data-crm-merge-list]`).innerHTML = targets.map((target, index) => {
      const text = target.detail ? `${target.label}  ·  ${target.detail}` : target.label;
      return `<label data-crm-merge-option="${escapeHtml(text.toLowerCase())}" class="flex cursor-pointer items-center gap-2 px-3 py-1.5 text-xs text-foreground hover:bg-secondary"><input type="radio" name="crm-merge-target" value="${escapeHtml(target.id)}"${index === 0 && target.score > 0 ? " checked" : ""} /><span class="truncate">${escapeHtml(text)}</span></label>`;
    }).join("");
    renderCrmMergePicker();
    dialog.showModal();
    $(`[data-crm-merge-search]`)?.focus();
  }

  function renderCrmMergePicker() {
    const query = String($(`[data-crm-merge-search]`)?.value || "").trim().toLowerCase();
    for (const option of $$(`[data-crm-merge-option]`)) {
      option.hidden = Boolean(query) && !option.dataset.crmMergeOption.includes(query);
    }
    const picked = $$(`[name="crm-merge-target"]`).find((input) => input.checked);
    $(`[data-crm-merge-submit]`).disabled = !picked || Boolean(picked.closest("[data-crm-merge-option]")?.hidden);
  }

  function submitCrmMergePicker(event) {
    event.preventDefault();
    const picked = $$(`[name="crm-merge-target"]`).find((input) => input.checked);
    if (!picked || picked.closest("[data-crm-merge-option]")?.hidden) return;
    $(`[data-crm-merge-dialog]`).close();
    void mergeCrmContacts([crmMergeDuplicate], picked.value);
  }

  function crmOutreachPrompt(contact, sheetName = "") {
    const trim = (value) => String(value ?? "").trim();
    const name = trim(contact.name) || trim(contact.email);
    const mastodon = trim(contact.mastodon);
    const flowId = trim(contact.flow);
    const flow = flowId ? crmFlowLabel(flowId) : "";
    let group = trim(sheetName);
    if (!group || group === "No pipeline") group = flow;
    const audience = `${group} ${flow}`.toLowerCase();
    const mentions = (words) => words.some((word) => audience.includes(word));
    const about = [];
    if (mastodon) {
      const profile = crmProfileUrl("mastodon", mastodon);
      about.push(`Mastodon: ${mastodon}${profile && profile !== mastodon ? ` (${profile})` : ""}`);
    } else {
      about.push("no Mastodon handle saved, so look it up");
    }
    const title = trim(contact.title); const company = trim(contact.company);
    if (title && company) about.push(`${title} at ${company}`);
    else if (title || company) about.push(title + company);
    for (const field of ["website", "location"]) {
      if (trim(contact[field])) about.push(trim(contact[field]));
    }
    const quoted = `“${group}”`;
    let angle = "";
    if (mentions(["journalist", "press", "media", "reporter", "writer", "editor", "podcast", "newsletter", "blog"])) {
      angle = `They are on my ${quoted} sheet as a journalist, so pitch a story: why decentralized code hosting matters now and what is new about ForkMesh, and offer an interview or a demo. No sales talk.`;
    } else if (mentions(["investor", "angel", "vc", "fund", "capital"])) {
      angle = `They are on my ${quoted} sheet as an investor, so lead with the opportunity ForkMesh opens and ask for a short call; state only facts you were given or looked up, and never invent traction, numbers or names.`;
    } else if (mentions(["partner", "integration", "alliance"])) {
      angle = `They are on my ${quoted} sheet as a potential partner, so show how ForkMesh fits what they build and suggest exploring working together.`;
    } else if (mentions(["customer", "lead", "user", "developer", "maintainer", "community", "open source"])) {
      angle = `They are on my ${quoted} sheet as a potential user, so give one concrete way ForkMesh helps their projects and invite them to try it.`;
    } else if (group) {
      angle = `They are on my ${quoted} sheet, so pitch it the way that suits that group.`;
    }
    return [
      "Tell me about ForkMesh (https://forkmesh.com).",
      `Then tell me about ${name} (${about.join("; ")}).`,
      `Then write a short toot (under 500 characters) I can send them on Mastodon about ForkMesh.${angle ? ` ${angle}` : ""} Mention them by their handle, make it specific to their work, and use at most two hashtags.`,
    ].join("\n\n");
  }

  async function copyCrmOutreachPrompt(contact) {
    if (!contact) return;
    const name = String(contact.name || "").trim() || String(contact.email || "").trim();
    try {
      await navigator.clipboard.writeText(crmOutreachPrompt(contact, crmActiveView()?.name || ""));
      crmMessage("notice", `Copied a prompt about ${name}. Paste it into your assistant.`);
    } catch { crmMessage("error", "Could not copy the prompt."); }
  }

  function crmCustomColumnKey(label) {
    return String(label || "").trim().toLowerCase().replace(/[^a-z0-9]+/g, "_")
      .replace(/^_+|_+$/g, "").slice(0, 64);
  }

  function crmCustomColumnLabel(key) {
    const label = String(key || "").replaceAll("_", " ");
    return label ? label[0].toUpperCase() + label.slice(1) : label;
  }

  function crmStoredActiveView() {
    const view = crmActiveView();
    return view && !view.virtual && state.crmView.views.includes(view)
      && !/^(pipeline:|no-pipeline)/.test(String(view.id || "")) ? view : null;
  }

  async function addCrmCustomColumn() {
    closeCrmMenu();
    if (crmActiveShared()) return;
    const label = String(window.prompt("Column name") || "").trim();
    if (!label) return;
    const key = crmCustomColumnKey(label);
    if (!key) { crmMessage("error", "Use letters or digits in the column name."); return; }
    const builtin = CRM_BASE_COLUMNS.find((column) => column.field === key);
    if (builtin && (state.crmView.settings?.removedColumns || []).includes(key)) { await restoreCrmColumn(key); return; }
    if (builtin) {
      crmShowColumn(key);
      crmMessage("notice", `“${builtin.label}” is a built-in column; it is shown again.`);
      return;
    }
    const view = crmStoredActiveView();
    if (view) {
      const previous = Array.isArray(view.columns) ? view.columns : [];
      const at = previous.findIndex((column) => column?.field === key);
      const layout = at >= 0
        ? previous.map((column, index) => index === at ? { ...column, hidden: false } : column)
        : [...previous, { field: key, label: crmCustomColumnLabel(key), custom: true, hidden: false }];
      if (at < 0 || previous[at].hidden) {
        view.columns = layout;
        try {
          await crmRequest(crmOrganizationPath(`/views/${view.id}`), "PATCH", { columns: layout });
        } catch (error) {
          view.columns = previous;
          crmMessage("error", `The fields on “${view.name}” were not saved: ${error.message}`);
          await loadCrm();
          return;
        }
      }
    } else {
      if (crmSessionColumns.organization !== state.crmView.organization) {
        crmSessionColumns.organization = state.crmView.organization;
        crmSessionColumns.labels = new Map();
      }
      crmSessionColumns.labels.set(key, crmCustomColumnLabel(key));
    }
    crmShowColumn(key);
    renderCrmContacts();
    crmMessage("notice", `Column “${crmColumnLabel(key)}” added. Click into a cell to fill it; each value is saved on its contact and shared with the organization.`);
  }

  function crmPeopleLater() {
    if (!crmPeople.open || crmPeople.timer) return;
    crmPeople.timer = setTimeout(() => { crmPeople.timer = 0; renderCrmPeople(); }, 600);
  }

  function setCrmPeople(open) {
    crmPeople.open = Boolean(open);
    renderCrmContacts();
    renderCrmSections();
  }

  function crmPeopleRows(query = "") {
    const lists = new Map();
    const pipelines = crmPipelineIds();
    for (const view of crmTabViews().filter((item) => item.id !== CRM_NO_PIPELINE_TAB)) {
      for (const contact of state.crmView.contacts) {
        if (!crmContactOnTab(contact, view, pipelines)) continue;
        if (!lists.has(contact.id)) lists.set(contact.id, []);
        lists.get(contact.id).push(view.name);
      }
    }
    let named = 0;
    const picked = [];
    for (const contact of state.crmView.contacts) {
      const sources = crmPictureSources(contact);
      if (sources.length) named += 1;
      const picture = crmPictureFor(sources);
      if (!picture.done) crmQueuePictureSources(sources);
      if (!picture.url || !crmContactMatchesQuery(contact, query)) continue;
      picked.push({ contact, lists: lists.get(contact.id) || [] });
    }
    const fold = (value) => crmContactLabel(value).toLowerCase();
    picked.sort((a, b) => crmPeople.sort === "name"
      ? (fold(a.contact) < fold(b.contact) ? -1 : fold(a.contact) > fold(b.contact) ? 1 : 0)
      : crmPeople.sort === "contacted"
      ? Number(b.contact.lastContactedAt || 0) - Number(a.contact.lastContactedAt || 0)
      : Number(b.contact.createdAt || 0) - Number(a.contact.createdAt || 0));
    return { named, picked };
  }

  function renderCrmPeople() {
    const grid = $(`[data-crm-people-grid]`);
    if (!grid || !crmPeople.open) return;
    const query = String($(`[data-crm-search]`)?.value || "").trim().toLowerCase();
    const { named, picked } = crmPeopleRows(query);
    grid.innerHTML = picked.map(({ contact, lists }) => {
      const name = crmContactLabel(contact);
      const pipeline = !contact.flow ? "No pipeline"
        : contact.stage ? `${crmFlowLabel(contact.flow)} · ${titleCaseSlug(contact.stage)}` : crmFlowLabel(contact.flow);
      const listLine = lists.length ? lists.join(", ") : "On no list";
      const title = `${name}\nLists: ${listLine}\nPipeline: ${pipeline}\nAdded ${crmTimestamp(contact.createdAt)}`;
      return `<button type="button" data-crm-contact-detail="${escapeHtml(contact.id)}" title="${escapeHtml(title)}" class="crm-person">${crmPicture(contact, "people")}<span class="block w-full truncate text-xs font-semibold text-foreground">${escapeHtml(name)}</span><span class="block w-full truncate text-[11px] text-muted-foreground">${escapeHtml(listLine)}</span><span class="block w-full truncate text-[11px] text-muted-foreground">${escapeHtml(pipeline)}</span></button>`;
    }).join("");
    const loading = crmPictures.queue.length || crmPictures.inFlight ? " Pictures are still loading…" : "";
    const note = $(`[data-crm-people-note]`);
    if (note) note.textContent = `${picked.length} of ${named} contacts with an account or email address show a picture.${loading}`;
    const sort = $(`[data-crm-people-sort]`);
    if (sort) sort.value = crmPeople.sort;
  }

  function renderCrmListsOverview() {
    const body = $(`[data-crm-lists-body]`);
    if (!body) return;
    const views = state.crmView.views;
    body.innerHTML = views.length ? views.map((view) => {
      const columns = Array.isArray(view.columns) ? view.columns : [];
      const shown = columns.filter((column) => !column?.hidden).length;
      const people = crmSheetShareNames(view).length;
      const peopleLabel = people === 1 ? "1 person" : `${people} people`;
      const reach = view.visibility === "private"
        ? (people ? `Shared with ${peopleLabel}` : "Only its owner")
        : `Whole organization · ${peopleLabel}`;
      const count = state.crmView.contacts.filter((contact) => crmContactInView(contact, view)).length;
      const cells = [
        view.flow ? `${view.name} (pipeline)` : view.name,
        view.flow ? `Every contact in ${crmFlowLabel(view.flow)}` : view.fileName || "",
        count.toLocaleString(), `${shown} of ${columns.length}`, reach, view.owner || "",
        crmTimestamp(view.createdAt),
      ];
      return `<tr data-crm-lists-row="${escapeHtml(view.id)}" class="border-t border-border align-top">${cells.map((cell, index) => `<td class="px-2 py-1.5${index === 0 ? " font-semibold" : ""}" title="${escapeHtml(index === 4 ? crmSheetShareLabel(view) : cell)}">${index === 0 ? `<button type="button" data-crm-lists-open-row="${escapeHtml(view.id)}" class="crm-link text-left hover:underline" style="color:var(--dashboard-link)">${escapeHtml(cell)}</button>` : escapeHtml(cell)}</td>`).join("")}</tr>`;
    }).join("") : `<tr><td colspan="7" class="px-2 py-4 text-center text-muted-foreground">No lists yet: import a CSV or create a sheet.</td></tr>`;
  }

  function openCrmListsOverview() {
    const dialog = $(`[data-crm-lists-dialog]`);
    if (!dialog) return;
    renderCrmListsOverview();
    if (!dialog.open) dialog.showModal();
  }

  const CRM_GITHUB_CONTRIBUTORS_MAX = 2000;
  const CRM_GITHUB_COMMIT_PAGES = 10;
  const CRM_FORKMESH_USERS_MAX = 2000;
  const CRM_FORKMESH_USERS_FILE = "forkmesh-users.csv";
  const CRM_IMPORT_ADDRESS = /^[^@\s<>"(),;:]+@[a-z0-9-]+(\.[a-z0-9-]+)+$/;
  let crmGithubBusy = false;
  let crmUsersBusy = false;

  function crmGithubRepositoryPath(value) {
    const match = String(value || "").trim().match(
      /^(?:(?:https?:\/\/)?(?:www\.)?github\.com\/|git@github\.com:)?([A-Za-z0-9](?:[A-Za-z0-9-]{0,38}))\/([A-Za-z0-9._-]{1,100}?)(?:\.git)?\/?(?:[/?#].*)?$/i);
    if (!match || match[2] === "." || match[2] === "..") return "";
    return `${match[1]}/${match[2]}`;
  }

  function crmMaskedGitEmail(email) {
    return /(^|[.@+_-])(do-?not-?|no-?)reply([.@+_-]|$)/i.test(String(email || ""));
  }

  function crmCoAuthors(message) {
    const authors = [];
    for (const line of String(message || "").replace(/\r\n?/g, "\n").split("\n")) {
      const trailer = line.match(/^\s*co-authored-by\s*:\s*(.+?)\s*$/i);
      const match = trailer && trailer[1].match(/^(.*?)\s*<([^<>]+)>$/);
      if (match) authors.push([match[1], match[2]]);
    }
    return authors;
  }

  function crmGithubContributorsCsv(commits, limit = CRM_GITHUB_CONTRIBUTORS_MAX) {
    const people = new Map();
    for (const item of commits || []) {
      const commit = item?.commit || {};
      const signers = [[commit.author?.name, commit.author?.email], [commit.committer?.name, commit.committer?.email],
        ...crmCoAuthors(commit.message)];
      const counted = new Set();
      const day = String(commit.author?.date || "").slice(0, 10);
      for (const [rawName, rawEmail] of signers) {
        const email = String(rawEmail || "").trim().toLowerCase();
        const name = String(rawName || "").replace(/\s+/g, " ").trim();
        if (counted.has(email) || !CRM_IMPORT_ADDRESS.test(email)
          || /\.(local|localdomain|lan|home|internal|invalid|example)$/.test(email) || crmMaskedGitEmail(email)
          || /\[bot\]$/i.test(name) || email.split("@")[0].endsWith("[bot]")) continue;
        counted.add(email);
        const person = people.get(email);
        if (!person) people.set(email, { name, email, last: day, commits: 1 });
        else { if (!person.name) person.name = name; person.commits += 1; }
      }
    }
    const ranked = [...people.values()].sort((a, b) => b.commits - a.commits
      || (a.last !== b.last ? (a.last > b.last ? -1 : 1) : a.email < b.email ? -1 : a.email > b.email ? 1 : 0));
    const rows = ranked.slice(0, limit).map((person) =>
      [crmCsvCell(person.name), crmCsvCell(person.email), String(person.commits), crmCsvCell(person.last)].join(","));
    return { csv: ["Name,Email,Commits,Last commit", ...rows].join("\r\n") + "\r\n", found: ranked.length };
  }

  function crmGithubStatus(text = "", failed = false) {
    const box = $(`[data-crm-github-status]`); if (!box) return;
    box.textContent = text;
    box.classList.toggle("hidden", !text);
    box.classList.toggle("text-destructive", failed);
    box.classList.toggle("text-muted-foreground", !failed);
  }

  function openCrmGithubDialog() {
    closeCrmMenu();
    $(`[data-crm-actions]`)?.removeAttribute("open");
    crmGithubStatus();
    $(`[data-crm-github-dialog]`)?.showModal();
    $(`[data-crm-github-repository]`)?.focus();
  }

  function crmGithubRateLimit(response) {
    const reset = Number(response.headers.get("x-ratelimit-reset")) * 1000;
    const when = reset > Date.now() ? ` It resets at ${crmTimestamp(reset)}.` : "";
    return `GitHub's limit on reads from this browser without signing in is used up.${when}`;
  }

  async function crmGithubCommits(repository, progress) {
    const commits = [];
    let url = `https://api.github.com/repos/${repository}/commits?per_page=100`;
    let pages = 0;
    let limited = "";
    while (url && pages < CRM_GITHUB_COMMIT_PAGES) {
      let response;
      try {
        response = await fetch(url, {
          headers: { accept: "application/vnd.github+json" },
          credentials: "omit", referrerPolicy: "no-referrer", cache: "no-store",
        });
      } catch { throw new Error("GitHub could not be reached from this browser"); }
      if ((response.status === 403 || response.status === 429) && (response.status === 429
        || response.headers.get("x-ratelimit-remaining") === "0")) {
        limited = crmGithubRateLimit(response);
        if (!commits.length) throw new Error(limited);
        break;
      }
      if (response.status === 404) throw new Error("GitHub has no public repository by that name");
      if (response.status === 409) break;
      if (!response.ok) throw new Error(`GitHub answered ${response.status}`);
      const page = await response.json();
      commits.push(...(Array.isArray(page) ? page : []));
      pages += 1;
      progress(commits.length, pages);
      url = Array.isArray(page) && page.length ? crmMastodonNext(response.headers.get("link"), "api.github.com") : "";
    }
    return { commits, limited, more: Boolean(url) && !limited };
  }

  async function submitCrmGithub(event) {
    event.preventDefault();
    if (crmGithubBusy) return;
    const repository = crmGithubRepositoryPath($(`[data-crm-github-repository]`)?.value);
    if (!repository) { crmGithubStatus("Enter a repository like owner/name or https://github.com/owner/name.", true); return; }
    const submit = $(`[data-crm-github-submit]`);
    crmGithubBusy = true;
    if (submit) submit.disabled = true;
    crmGithubStatus(`Reading the contributors of ${repository}…`);
    try {
      const { commits, limited, more } = await crmGithubCommits(repository, (count) =>
        crmGithubStatus(`Reading the contributors of ${repository}… ${count} commits`));
      const { csv, found } = crmGithubContributorsCsv(commits);
      if (!found) { crmGithubStatus(`No contributor of ${repository} has a public email address to import.`, true); return; }
      $(`[data-crm-github-dialog]`)?.close();
      const notes = [
        limited && `${limited} Only the ${commits.length} newest commits were read.`,
        more && `Only the ${commits.length} newest commits were read.`,
        found > CRM_GITHUB_CONTRIBUTORS_MAX && `Importing the ${CRM_GITHUB_CONTRIBUTORS_MAX} most active of ${repository}'s ${found} contributors; one import takes at most ${CRM_GITHUB_CONTRIBUTORS_MAX}.`,
      ].filter(Boolean);
      const file = new File([csv], `github-contributors-${repository.replace("/", "-")}.csv`, { type: "text/csv" });
      crmImportTarget = "";
      await previewCrmCsv(file, `Contributors of ${repository}`.slice(0, 60));
      if (notes.length) crmMessage("notice", notes.join(" "));
    } catch (error) {
      crmGithubStatus(`Could not read the contributors of ${repository}: ${error.message}.`, true);
    } finally {
      crmGithubBusy = false;
      if (submit) submit.disabled = false;
    }
  }

  function crmForkMeshUsersCsv(users, limit = CRM_FORKMESH_USERS_MAX) {
    const kept = []; const seen = new Set(); let withoutEmail = 0;
    for (const user of users || []) {
      const kind = String(user?.kind || "").trim(); const status = String(user?.status || "").trim();
      if ((kind && kind !== "user") || (status && status !== "active") || !String(user?.name || "").trim()) continue;
      const email = String(user?.email || "").trim().toLowerCase();
      if (!CRM_IMPORT_ADDRESS.test(email)) { withoutEmail += 1; continue; }
      if (seen.has(email)) continue;
      seen.add(email);
      kept.push({ ...user, email });
    }
    kept.sort((a, b) => (Number(b.createdAt) || 0) - (Number(a.createdAt) || 0));
    const rows = kept.slice(0, limit).map((user) => {
      const created = Number(user.createdAt) || 0;
      return [crmCsvCell(String(user.name).trim()), crmCsvCell(user.email),
        created > 0 ? new Date(created).toISOString().slice(0, 10) : "",
        typeof user.emailNotifications === "boolean" && !user.emailNotifications ? "Off" : "On"].join(",");
    });
    return { csv: ["Name,Email,Signed up,Email notifications", ...rows].join("\r\n") + "\r\n", found: kept.length, withoutEmail };
  }

  function crmForkMeshUsersSheetId() {
    return state.crmView.views.find((view) => view.fileName === CRM_FORKMESH_USERS_FILE && view.canManage)?.id || "";
  }

  function renderCrmUsersImport() {
    const button = $(`[data-crm-users-import]`);
    if (!button) return;
    const available = crmAdmin() && state.crmView.installed && !(crmPictures.usersAt && !crmPictures.usersEmails);
    button.classList.toggle("hidden", !available);
    const refreshes = Boolean(crmForkMeshUsersSheetId());
    const label = $(`[data-crm-users-import-label]`);
    if (label) label.textContent = refreshes ? "Refresh ForkMesh users" : "Import ForkMesh users";
    button.title = refreshes
      ? "Read the ForkMesh user directory again: new signups join the ForkMesh users list and the users already on it are updated, matched by email"
      : "Import every active ForkMesh user with an email address as a new list, ForkMesh users, that Refresh users keeps up to date";
  }

  async function importCrmForkMeshUsers() {
    if (crmUsersBusy) return;
    if (!crmAdmin()) { crmMessage("error", "Only ForkMesh administrators can import the user directory."); return; }
    crmUsersBusy = true;
    crmMessage("notice", "Reading the ForkMesh user directory…");
    try {
      let users;
      try { users = await crmForkMeshUsers(true); } catch (error) {
        crmMessage("error", `Could not read the ForkMesh user directory: ${error.message}.`);
        return;
      }
      renderCrmUsersImport();
      if (!users) { crmMessage("error", "Only ForkMesh administrators can import the user directory."); return; }
      const { csv, found, withoutEmail } = crmForkMeshUsersCsv(users);
      if (!found) { crmMessage("error", "No active ForkMesh user has an email address to import."); return; }
      const [header = "", ...rows] = crmCsvRecords(csv);
      const note = found > CRM_FORKMESH_USERS_MAX
        ? `Importing the ${CRM_FORKMESH_USERS_MAX} newest of ${found} ForkMesh users; one import takes at most ${CRM_FORKMESH_USERS_MAX}.`
        : withoutEmail ? `Importing ${found} ForkMesh users; ${withoutEmail} without an email address are left out.` : "";
      await importCrmCsv({
        file: new File([csv], CRM_FORKMESH_USERS_FILE, { type: "text/csv" }),
        target: crmForkMeshUsersSheetId(), path: crmOrganizationPath("/contacts/import"),
        header, rows, delimiter: ",", mapping: ["name", "email", "custom", "custom"], sheetName: "ForkMesh users",
      });
      const done = $(`[data-crm-notice]`);
      if (note && done && !done.classList.contains("hidden")) crmMessage("notice", `${done.textContent} ${note}`);
      renderCrmUsersImport();
    } finally { crmUsersBusy = false; }
  }

  function handleCrmParityClick(event) {
    const now = event.target.closest("[data-crm-contacted-now]");
    if (now) { void crmMarkContacted([now.dataset.crmContactedNow], Date.now()); return; }
    const prompt = event.target.closest("[data-crm-contact-prompt]");
    if (prompt) { void copyCrmOutreachPrompt(crmContactById(prompt.dataset.crmContactPrompt)); return; }
    const refresh = event.target.closest("[data-crm-mastodon-refresh]");
    if (refresh) { refreshCrmMastodonNow(refresh.dataset.crmMastodonRefresh); return; }
    const linked = event.target.closest("[data-crm-linked]");
    if (linked) openCrmLinked(event, linked.dataset.crmLinkedId, linked.dataset.crmLinked === "toots");
  }

  function wireCrmTableParity() {
    const list = $(`[data-crm-contact-list]`);
    list?.addEventListener("click", handleCrmSelectionClick, true);
    list?.addEventListener("mousedown", handleCrmSelectionPress, true);
    for (const selector of ["[data-crm-contact-list]", "[data-crm-contact-cards]", "[data-crm-contact-detail-dialog]"]) {
      $(selector)?.addEventListener("click", handleCrmParityClick);
    }
    $(`[data-crm-contact-head]`)?.addEventListener("click", (event) => {
      if (event.target.closest("[data-crm-add-column]")) { void addCrmCustomColumn(); return; }
      if (event.target.closest("[data-crm-select-all]")) { handleCrmSelectionClick(event); return; }
      const cell = event.target.closest("[data-crm-column]");
      if (cell) crmSortBy(cell.dataset.crmColumn);
    });
    $(`[data-crm-people-toggle]`)?.addEventListener("click", () => setCrmPeople(!crmPeople.open));
    $(`[data-crm-people-sort]`)?.addEventListener("change", (event) => { crmPeople.sort = event.target.value; renderCrmPeople(); });
    $(`[data-crm-people-grid]`)?.addEventListener("click", (event) => {
      const card = event.target.closest("[data-crm-contact-detail]");
      if (card) openCrmContactDetail(card.dataset.crmContactDetail);
    });
    $(`[data-crm-merge-search]`)?.addEventListener("input", renderCrmMergePicker);
    $(`[data-crm-merge-list]`)?.addEventListener("change", renderCrmMergePicker);
    $(`[data-crm-merge-list]`)?.addEventListener("dblclick", (event) => {
      if (event.target.closest("[data-crm-merge-option]")) $(`[data-crm-merge-form]`)?.requestSubmit();
    });
    $(`[data-crm-merge-form]`)?.addEventListener("submit", submitCrmMergePicker);
    $(`[data-crm-lists-open]`)?.addEventListener("click", openCrmListsOverview);
    $(`[data-crm-lists-body]`)?.addEventListener("click", (event) => {
      const open = event.target.closest("[data-crm-lists-open-row]");
      if (!open) return;
      $(`[data-crm-lists-dialog]`)?.close();
      selectCrmSheet(open.dataset.crmListsOpenRow);
    });
    $(`[data-crm-github-import]`)?.addEventListener("click", openCrmGithubDialog);
    $(`[data-crm-github-form]`)?.addEventListener("submit", (event) => void submitCrmGithub(event));
    $(`[data-crm-users-import]`)?.addEventListener("click", () => void importCrmForkMeshUsers());
    $(`[data-crm-grants-load]`)?.addEventListener("click", () => void loadCrmGrants());
    document.addEventListener("error", (event) => {
      if (event.target?.matches?.(".crm-pic img")) event.target.remove();
    }, true);
    document.addEventListener("keydown", (event) => {
      if (event.key !== "Escape" || !crmSelection.ids.size || document.querySelector("dialog[open]")) return;
      crmSelection.ids = new Set();
      crmPaintSelection();
    });
  }

  async function loadCrm() {
    if (state.crmView.sharedOnly && state.crmView.activeShared) {
      delete state.crmView.sharedCache[state.crmView.activeShared];
      await selectCrmSharedSheet(state.crmView.activeShared);
      return;
    }
    if (!state.crmView.organization) return;
    renderCrmInstallState();
    if (!state.crmView.installed) return;
    state.crmView.loading = true;
    try {
      // A re-read echoes the revision last applied; an unchanged organization
      // answers with a few bytes and the page is not re-rendered.
      const query = new URLSearchParams({ tz: String(-new Date().getTimezoneOffset()) });
      if (state.crmView.revision) query.set("since", state.crmView.revision);
      const payload = await crmRequest(crmOrganizationPath(`?${query}`));
      if (payload.unchanged) { crmMessage("error", ""); return; }
      state.crmView.revision = payload.revision || "";
      state.crmView.contacts = payload.contacts || []; state.crmView.emails = payload.emails || [];
      state.crmView.mail = payload.mail || [];
      state.crmView.settings = payload.settings || {}; state.crmView.summary = payload.summary || {};
      state.crmView.outreach = payload.outreach || null;
      state.crmView.views = payload.views || []; state.crmView.members = payload.members || [];
      state.crmView.sharedViews = payload.sharedViews || [];
      state.crmView.decks = payload.decks || [];
      state.crmView.activity = payload.activity || { days: [], kinds: [], members: [] };
      if (!state.crmView.sharedViews.some((view) => view.id === state.crmView.activeShared)) {
        state.crmView.activeShared = "";
      }
      // Shared rows are re-read on the next open so a sharer's edits show up.
      for (const id of Object.keys(state.crmView.sharedCache)) {
        if (id !== state.crmView.activeShared) delete state.crmView.sharedCache[id];
      }
      state.crmView.role = payload.role || "member"; state.crmView.canManageSettings = Boolean(payload.canManageSettings);
      // Read once per organization, so a layout picked in a browser without
      // storage is not reset by the 30-second refresh.
      if (state.crmView.layoutOrg !== state.crmView.organization) {
        state.crmView.layoutOrg = state.crmView.organization;
        state.crmView.layout = crmReadLayout();
      }
      const zohoButton = $(`[data-crm-zoho-sync]`);
      const activeZoho = crmZohoActive();
      const zohoConnected = Boolean(activeZoho.zoho?.connected);
      if (zohoButton) {
        zohoButton.classList.remove("hidden");
        zohoButton.textContent = zohoConnected ? "Sync Zoho Mail" : "Connect Zoho Mail";
        zohoButton.title = !zohoConnected
          ? "Connect your own Zoho Mail so the CRM sends and syncs from your mailbox"
          : activeZoho.mine
          ? "Bring in your own Zoho Mail"
          : "Refresh the organization's shared Zoho Mail boxes";
      }
      renderCrmDraftsLabel(); crmMessage("error", ""); renderCrm();
      crmRefreshForkMeshAvatars();
      crmAutoSyncToots();
      crmAutoSyncZoho();
    } catch (error) {
      if (error.status === 409 && error.payload?.error === "plugin_not_installed") {
        const record = crmSelectedOrg(); if (record) record.installed = false;
        renderCrmInstallState(); return;
      }
      crmMessage("error", `CRM is unavailable: ${error.message}`);
    }
    finally { state.crmView.loading = false; }
  }

  // The "More" menu is a native disclosure. It closes after an item runs, on a
  // click outside it, and on Escape. Closing waits a tick so the Import CSV
  // label still hands its click to the file input before the menu collapses.
  function wireCrmActionsMenu() {
    const actions = $(`[data-crm-actions]`); if (!actions) return;
    const close = () => { actions.open = false; };
    actions.addEventListener("click", (event) => {
      if (event.target.closest(".crm-menu-item")) setTimeout(close);
    });
    document.addEventListener("click", (event) => {
      if (actions.open && !actions.contains(event.target)) close();
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape" && actions.open) { close(); actions.querySelector("summary")?.focus(); }
    });
  }

  async function initCrmPage() {
    // The mailbox and extension buttons work before sign-in or install.
    renderCrmExtensionState(); renderCrmMailLinks();
    wireCrmActionsMenu();
    state.crmView.section = crmSectionFromUrl();
    window.addEventListener("forkmesh-crm-extension", renderCrmExtensionState);
    window.addEventListener("message", (event) => void onCrmZohoSetupMessage(event));
    $(`[data-crm-extensions-open]`)?.addEventListener("click", () => void openCrmExtensionsPage());
    if (!state.session?.sessionToken) { crmMessage("error", "Sign in to use the organization CRM."); return; }
    const select = $(`[data-crm-organization]`);
    let sharedLink = null;
    try {
      const payload = await crmRequest("/api/crm"); state.crmView.organizations = payload.organizations || [];
      const link = await crmResolveLink();
      const installedOrg = state.crmView.organizations.find((org) => org.installed);
      if (!state.crmView.organizations.length || (link?.shared && !installedOrg)) {
        sharedLink = link?.shared ? link.id : "";
      } else {
        select.innerHTML = state.crmView.organizations.map((org) => `<option value="${escapeHtml(org.name)}">${escapeHtml(org.name)} · ${escapeHtml(org.role)}${org.installed ? "" : " · not installed"}</option>`).join("");
        const requested = link?.organization || (link?.shared ? installedOrg.name : "") || new URL(location.href).searchParams.get("org") || "";
        state.crmView.organization = state.crmView.organizations.some((org) => org.name === requested) ? requested : state.crmView.organizations[0].name;
        select.value = state.crmView.organization; await loadCrm();
        const zohoOutcome = crmZohoReturn();
        if (zohoOutcome === "connected" && state.crmView.settings?.zoho?.connected) void syncCrmZoho(false, "organization");
        if (zohoOutcome === "connected_mine") { selectCrmSection("zoho"); if (state.crmView.settings?.zohoMine?.connected) void syncCrmZoho(false, "mine"); }
        if (crmZohoSetupParam() && state.crmView.canManageSettings) runCrmZohoStep(state.crmView.settings?.zoho?.connected ? "reconnect" : "connect", "organization");
      }
    } catch (error) { crmMessage("error", `CRM is unavailable: ${error.message}`); return; }
    select?.addEventListener("change", () => { state.crmView.organization = select.value; const url = new URL(location.href); url.searchParams.set("org", select.value); url.searchParams.delete("list"); history.replaceState(history.state, "", url); void loadCrm(); });
    $(`[data-crm-columns]`)?.addEventListener("click", openCrmColumnsMenu);
    $(`[data-crm-search]`)?.addEventListener("input", renderCrmContacts); $(`[data-crm-flow]`)?.addEventListener("change", renderCrmContacts); $(`[data-crm-table-view]`)?.addEventListener("change", (event) => selectCrmTableView(event.target.value)); $(`[data-crm-table-view-save]`)?.addEventListener("click", () => void saveCrmTableViewDraft()); $(`[data-crm-stage]`)?.addEventListener("change", renderCrmContacts); $(`[data-crm-owner]`)?.addEventListener("change", renderCrmContacts); $(`[data-crm-focus]`)?.addEventListener("change", crmFocusChanged);
    $(`[data-crm-contact-new]`)?.addEventListener("click", () => openCrmContact());
    const handleContactAction = (event) => { const advance = event.target.closest("[data-crm-account-advance]"); if (advance) { void advanceCrmFromAccount(advance.dataset.crmAccountAdvance); return; } const detail = event.target.closest("[data-crm-contact-detail]"); if (detail) { openCrmContactDetail(detail.dataset.crmContactDetail); return; } const deck = event.target.closest("[data-crm-contact-deck]"); if (deck) { void openCrmDeck(deck.dataset.crmContactDeck); return; } const edit = event.target.closest("[data-crm-contact-edit]"); const compose = event.target.closest("[data-crm-contact-compose]"); const remove = event.target.closest("[data-crm-contact-delete-row]"); if (edit) openCrmContact(crmContactById(edit.dataset.crmContactEdit)); else if (compose) openCrmEmail(crmContactById(compose.dataset.crmContactCompose)); else if (remove) void deleteCrmContact(remove.dataset.crmContactDeleteRow); };
    $(`[data-crm-contact-list]`)?.addEventListener("click", handleContactAction);
    $(`[data-crm-contact-cards]`)?.addEventListener("click", handleContactAction);
    $(`[data-crm-next]`)?.addEventListener("click", handleCrmNextClick);
    $(`[data-crm-ai-dialog]`)?.addEventListener("click", handleCrmAiClick);
    $(`[data-crm-ai-dialog]`)?.addEventListener("change", handleCrmAiChange);
    $(`[data-crm-ai-prompt]`)?.addEventListener("input", () => { if (state.crmView.ai) state.crmView.ai.edited = true; });
    $(`[data-crm-ai-dialog]`)?.addEventListener("close", () => { state.crmView.ai = null; });
    const handleContactMenu = (event) => {
      const row = event.target.closest("[data-crm-contact-row]");
      if (row) openCrmContactMenu(event, row.dataset.crmContactRow);
    };
    $(`[data-crm-contact-list]`)?.addEventListener("contextmenu", handleContactMenu);
    $(`[data-crm-contact-cards]`)?.addEventListener("contextmenu", handleContactMenu);
    $(`[data-crm-team-period]`)?.addEventListener("click", (event) => {
      const option = event.target.closest("[data-crm-team-period-option]");
      if (!option) return;
      state.crmView.teamPeriod = Number(option.dataset.crmTeamPeriodOption) === 7 ? 7 : 30;
      renderCrmTeam();
    });
    $(`[data-crm-team-member]`)?.addEventListener("change", (event) => {
      state.crmView.teamMember = event.target.value; renderCrmTeam();
    });
    $(`[data-crm-team-board]`)?.addEventListener("click", (event) => {
      const row = event.target.closest("[data-crm-team-row]");
      if (!row) return;
      state.crmView.teamMember = state.crmView.teamMember === row.dataset.crmTeamRow ? "" : row.dataset.crmTeamRow;
      renderCrmTeam();
    });
    $(`[data-crm-team-table-toggle]`)?.addEventListener("click", () => {
      state.crmView.teamTable = !state.crmView.teamTable; renderCrmTeam();
    });
    const sections = $(`[data-crm-sections]`);
    sections?.addEventListener("click", (event) => {
      const tab = event.target.closest("[data-crm-section-tab]");
      if (tab) selectCrmSection(tab.dataset.crmSectionTab);
    });
    // Arrow keys move along the tabs, as a tablist should.
    sections?.addEventListener("keydown", (event) => {
      const offset = { ArrowDown: 1, ArrowUp: -1, ArrowRight: 1, ArrowLeft: -1 }[event.key];
      const pages = CRM_SECTIONS.filter((section) => section !== "contacts");
      const at = pages.indexOf(state.crmView.section);
      if (event.key === "Home" || event.key === "End") {
        event.preventDefault(); selectCrmSection(pages[event.key === "Home" ? 0 : pages.length - 1], true);
      } else if (offset) {
        event.preventDefault(); selectCrmSection(pages[(Math.max(at, offset > 0 ? -1 : 0) + offset + pages.length) % pages.length], true);
      }
    });
    $(`[data-crm-side-people]`)?.addEventListener("click", () => {
      const open = !(state.crmView.section === "contacts" && crmPeople.open);
      if (state.crmView.section !== "contacts") selectCrmSection("contacts");
      setCrmPeople(open);
    });
    $(`[data-crm-side-settings]`)?.addEventListener("click", openCrmSettings);
    $(`[data-crm-history-refresh]`)?.addEventListener("click", () => {
      const view = state.crmView.views.find((item) => item.id === state.crmView.historyView);
      if (view) void openCrmListHistory(view);
    });
    $(`[data-crm-deck-dialog]`)?.addEventListener("click", (event) => {
      const revoke = event.target.closest("[data-crm-deck-revoke]");
      if (event.target.closest("[data-crm-deck-copy]")) {
        const url = $(`[data-crm-deck-url]`)?.value || "";
        navigator.clipboard?.writeText(url).then(() => crmMessage("notice", "Deck link copied."), () => crmMessage("error", "Could not copy the link."));
      } else if (event.target.closest("[data-crm-deck-email]")) emailCrmDeck();
      else if (revoke) void revokeCrmDeck(revoke.dataset.crmDeckRevoke);
      else if (event.target.closest("[data-crm-deck-new]")) void openCrmDeck(state.crmView.deckContactId);
    });
    const detailDialog = $(`[data-crm-contact-detail-dialog]`);
    detailDialog?.addEventListener("click", (event) => {
      if (event.target.closest("[data-crm-detail-reload]")) {
        const contact = crmContactById(state.crmView.detailContactId);
        state.crmView.detailUpdates = {};
        if (contact) void loadCrmContactUpdates(contact);
        return;
      }
      const note = event.target.closest("[data-crm-note-save]");
      if (note) { void saveCrmContactNote(note.dataset.crmNoteSave, note); return; }
      const link = event.target.closest("[data-crm-contact-link]");
      if (link) { void copyCrmContactLink(link.dataset.crmContactLink, link); return; }
      const toot = event.target.closest("[data-crm-contact-toot]");
      if (toot) { void logCrmToot(toot.dataset.crmContactToot); return; }
      const ai = event.target.closest("[data-crm-contact-ai]");
      if (ai) { detailDialog.close(); openCrmAi(ai.dataset.crmContactAi); return; }
      const auto = event.target.closest("[data-crm-contact-auto]");
      if (auto) { void sendCrmAutoEmail(auto.dataset.crmContactAuto, "", "", auto); return; }
      const advance = event.target.closest("[data-crm-account-advance]");
      if (advance) { detailDialog.close(); void advanceCrmFromAccount(advance.dataset.crmAccountAdvance); return; }
      // Email, Deck and the full form are dialogs of their own: the card
      // closes so they are not stacked on top of it.
      if (!event.target.closest("[data-crm-contact-edit], [data-crm-contact-compose], [data-crm-contact-deck]")) return;
      detailDialog.close();
      handleContactAction(event);
    });
    detailDialog?.addEventListener("close", () => { state.crmView.detailContactId = ""; crmSetContactParam(""); });
    $(`[data-crm-layout]`)?.addEventListener("click", (event) => {
      const option = event.target.closest("[data-crm-layout-option]");
      if (option) setCrmLayout(option.dataset.crmLayoutOption);
    });
    const board = $(`[data-crm-board]`);
    let draggedCard = "";
    board?.addEventListener("click", handleContactAction);
    board?.addEventListener("keydown", (event) => {
      const item = event.target.closest("[data-crm-board-card]");
      if (item && (event.key === "Enter" || event.key === " ")) { event.preventDefault(); openCrmContactDetail(item.dataset.crmBoardCard); }
    });
    board?.addEventListener("dragstart", (event) => {
      const item = event.target.closest("[data-crm-board-card]");
      if (!item || (crmActiveShared() && !crmActiveShared().canEdit)) return;
      draggedCard = item.dataset.crmBoardCard;
      item.classList.add("crm-dragging");
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", draggedCard);
    });
    board?.addEventListener("dragover", (event) => {
      const column = event.target.closest("[data-crm-board-stage]");
      if (!draggedCard || !column) return;
      event.preventDefault(); event.dataTransfer.dropEffect = "move";
      $$(`[data-crm-board-stage]`).forEach((node) => node.classList.toggle("crm-drop", node === column));
    });
    board?.addEventListener("dragleave", (event) => {
      if (!board.contains(event.relatedTarget)) $$(`[data-crm-board-stage]`).forEach((node) => node.classList.remove("crm-drop"));
    });
    board?.addEventListener("drop", (event) => {
      const column = event.target.closest("[data-crm-board-stage]");
      if (!draggedCard || !column) return;
      event.preventDefault();
      const id = draggedCard; draggedCard = "";
      void saveCrmContactStage(id, column.dataset.crmBoardStage);
    });
    board?.addEventListener("dragend", () => {
      draggedCard = "";
      $$(`[data-crm-board-stage]`).forEach((node) => node.classList.remove("crm-drop"));
      $$(`[data-crm-board-card]`).forEach((node) => node.classList.remove("crm-dragging"));
    });
    const handleInlineContactField = (event) => {
      const input = event.target.closest("[data-crm-contact-inline-field]");
      if (input) void saveCrmInlineContactField(input);
    };
    $(`[data-crm-contact-list]`)?.addEventListener("change", handleInlineContactField);
    $(`[data-crm-contact-cards]`)?.addEventListener("change", handleInlineContactField);
    detailDialog?.addEventListener("change", handleInlineContactField);
    $(`[data-crm-contact-form]`)?.addEventListener("submit", saveCrmContact); $(`[data-crm-contact-existing]`)?.addEventListener("click", (event) => void openCrmExistingContact(event.currentTarget.dataset.crmContactExisting)); $(`[data-crm-contact-delete]`)?.addEventListener("click", deleteCrmContact);
    $(`[data-crm-contact-flow]`)?.addEventListener("change", () => renderCrmContactStages());
    $(`[data-crm-contact-lookup]`)?.addEventListener("click", openCrmProfileSearch);
    $(`[data-crm-email-new]`)?.addEventListener("click", () => openCrmEmail());
    $(`[data-crm-email-key]`)?.addEventListener("input", () => { $(`[data-crm-email-submit]`).textContent = crmEmailSubmitLabel(); });
    $(`[data-crm-email-form]`)?.addEventListener("submit", sendCrmEmail);
    $(`[data-crm-email-dialog]`)?.addEventListener("close", () => { $(`[data-crm-email-key]`).value = ""; });
    $(`[data-crm-email-link-open]`)?.addEventListener("click", () => $(`[data-crm-link-dialog]`).showModal()); $(`[data-crm-link-form]`)?.addEventListener("submit", linkCrmEmail);
    $(`[data-crm-settings-open]`)?.addEventListener("click", openCrmSettings); $(`[data-crm-settings-form]`)?.addEventListener("submit", saveCrmSettings);
    $(`[data-crm-outreach-form]`)?.addEventListener("submit", sendCrmOutreach);
    $(`[data-crm-outreach-template]`)?.addEventListener("change", applyCrmOutreachTemplate);
    $(`[data-crm-outreach-team-add]`)?.addEventListener("click", () => {
      const input = $(`[data-crm-outreach-team-name]`); const name = (input?.value || "").trim().toLowerCase();
      if (!name) return; input.value = ""; void updateCrmOutreachTeam("add", name);
    });
    $(`[data-crm-outreach-team-list]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-crm-outreach-team-remove]");
      if (button) void updateCrmOutreachTeam("remove", button.getAttribute("data-crm-outreach-team-remove"));
    });
    $(`[data-crm-pipelines-open]`)?.addEventListener("click", () => { $(`[data-crm-settings-dialog]`)?.close(); openCrmPipelineSettings(); });
    const pipelinesDialog = $(`[data-crm-pipelines-dialog]`);
    pipelinesDialog?.addEventListener("click", handleCrmPipelinesClick);
    pipelinesDialog?.addEventListener("input", handleCrmPipelinesInput);
    pipelinesDialog?.addEventListener("change", handleCrmPipelinesInput);
    pipelinesDialog?.addEventListener("close", () => { state.crmView.pipelineEdit = null; });
    $(`[data-crm-pipelines-form]`)?.addEventListener("submit", (event) => void saveCrmPipelines(event));
    $(`[data-crm-members-form]`)?.addEventListener("submit", (event) => void saveCrmMembers(event));
    $(`[data-crm-members-search]`)?.addEventListener("input", filterCrmMembersRoster);
    const strip = $(`[data-crm-stage-strip]`);
    strip?.addEventListener("click", (event) => {
      if (event.target.closest("[data-crm-stage-strip-menu]")) { openCrmStageStripMenu(event); return; }
      const chip = event.target.closest("[data-crm-stage-chip]");
      if (chip) pickCrmStage(chip.dataset.crmStageChip);
    });
    strip?.addEventListener("contextmenu", openCrmStageStripMenu);
    $(`[data-crm-zoho-sync]`)?.addEventListener("click", () => {
      const { zoho, mine } = crmZohoActive();
      if (zoho?.connected) void syncCrmZoho(false, mine ? "mine" : "organization");
      else selectCrmSection("zoho", true);
    });
    $(`[data-crm-my-zoho-open]`)?.addEventListener("click", () => selectCrmSection("zoho", true));
    $(`[data-crm-my-zoho-connect]`)?.addEventListener("click", () => void connectCrmZoho(true));
    $(`[data-crm-my-zoho-sync]`)?.addEventListener("click", () => void syncCrmZoho(false, "mine"));
    $(`[data-crm-my-zoho-disconnect]`)?.addEventListener("click", () => void disconnectCrmMyZoho());
    $(`[data-crm-my-zoho-form]`)?.addEventListener("submit", (event) => void exchangeCrmMyZohoCode(event));
    $(`[data-crm-my-zoho-guide]`)?.addEventListener("click", openCrmZohoGuide);
    $(`[data-crm-org-zoho-sync]`)?.addEventListener("click", () => void syncCrmZoho(false, "organization"));
    $(`[data-crm-org-zoho-manage]`)?.addEventListener("click", openCrmSettings);
    $(`[data-crm-zoho-guide]`)?.addEventListener("click", openCrmZohoGuide);
    $(`[data-crm-zoho-bar]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-crm-zoho-step]");
      if (button) runCrmZohoStep(button.dataset.crmZohoStep, button.dataset.crmZohoMailbox || "mine");
    });
    $$(`[data-crm-zoho-guide-copy]`).forEach((button) => button.addEventListener("click", () => void copyCrmZohoGuide(button.dataset.crmZohoGuideCopy)));
    $(`[data-crm-zoho-guide-settings]`)?.addEventListener("click", () => { $(`[data-crm-zoho-guide-dialog]`).close(); openCrmSettings(); });
    $(`[data-crm-zoho-guide-connect]`)?.addEventListener("click", () => { $(`[data-crm-zoho-guide-dialog]`).close(); runCrmZohoStep("connect", "organization"); });
    $(`[data-crm-drafts-open]`)?.addEventListener("click", openCrmDrafts);
    $(`[data-crm-drafts-body]`)?.addEventListener("click", (event) => {
      const button = event.target.closest("[data-crm-draft-send]");
      if (button) void sendCrmDraft(Number(button.dataset.crmDraftSend));
    });
    $(`[data-crm-zoho-connect]`)?.addEventListener("click", () => void connectCrmZoho());
    $(`[data-crm-zoho-exchange]`)?.addEventListener("click", () => void exchangeCrmZohoCode());
    $(`[data-crm-zoho-extension]`)?.addEventListener("click", () => { $(`[data-crm-settings-dialog]`).close(); startCrmZohoSetup(); });
    $(`[data-crm-zoho-disconnect]`)?.addEventListener("click", () => void disconnectCrmZoho());
    $(`[data-crm-mastodon-sync]`)?.addEventListener("click", () => void syncCrmToots(false));
    $(`[data-crm-mastodon-disconnect]`)?.addEventListener("click", () => void disconnectCrmMastodon());
    $(`[data-crm-mail-box-filter]`)?.addEventListener("change", renderCrmMail);
    $(`[data-crm-mail-groups]`)?.addEventListener("change", (event) => {
      const select = event.target.closest("[data-crm-mail-move]");
      if (select) void moveCrmMail(select.dataset.crmMailMove, select.value);
    });
    // --- sheet tabs, and the column layout they carry ---------------------
    const tabs = $(`[data-crm-tabs]`);
    tabs?.addEventListener("click", (event) => {
      const sharedOptions = event.target.closest("[data-crm-tab-shared-menu]");
      if (sharedOptions) { openCrmSharedMenu(event, sharedOptions.dataset.crmTabSharedMenu); return; }
      const sharedTab = event.target.closest("[data-crm-tab-shared]");
      if (sharedTab) { void selectCrmSharedSheet(sharedTab.dataset.crmTabShared); return; }
      const options = event.target.closest("[data-crm-tab-menu]");
      if (options) { openCrmSheetMenu(event, options.dataset.crmTabMenu); return; }
      const tab = event.target.closest("[data-crm-tab]");
      if (tab) selectCrmSheet(tab.dataset.crmTab);
    });
    tabs?.addEventListener("contextmenu", (event) => {
      const sharedTab = event.target.closest("[data-crm-tab-shared], [data-crm-tab-shared-menu]");
      if (sharedTab) { openCrmSharedMenu(event, sharedTab.dataset.crmTabShared ?? sharedTab.dataset.crmTabSharedMenu); return; }
      const tab = event.target.closest("[data-crm-tab], [data-crm-tab-menu]");
      if (tab) openCrmSheetMenu(event, tab.dataset.crmTab ?? tab.dataset.crmTabMenu);
    });
    // Double-click renames the tab under the pointer, the way a spreadsheet does.
    tabs?.addEventListener("dblclick", (event) => {
      const tab = event.target.closest("[data-crm-tab]");
      const id = tab?.dataset.crmTab;
      if (!id || id === CRM_NO_PIPELINE_TAB) return;
      event.preventDefault();
      void renameCrmSheet(crmTabView(id));
    });
    // The count of people a sheet reaches opens the list of who they are.
    tabs?.addEventListener("mouseover", (event) => {
      const badge = event.target.closest("[data-crm-share]");
      if (badge) showCrmShareCard(badge); else hideCrmShareCard();
    });
    tabs?.addEventListener("mouseleave", hideCrmShareCard);
    tabs?.addEventListener("focusout", hideCrmShareCard);
    // Tabs are reordered by dragging one onto another, mirroring the column
    // headings. The tab strip scrolls, so the order is saved on drop.
    let draggedTab = "";
    tabs?.addEventListener("dragstart", (event) => {
      const tab = event.target.closest("[data-crm-tab-drag]");
      if (!tab) return;
      closeCrmMenu(); hideCrmShareCard();
      draggedTab = tab.dataset.crmTabDrag;
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", draggedTab);
    });
    tabs?.addEventListener("dragover", (event) => {
      if (draggedTab && event.target.closest("[data-crm-tab-drag]")) {
        event.preventDefault(); event.dataTransfer.dropEffect = "move";
      }
    });
    tabs?.addEventListener("drop", (event) => {
      const tab = event.target.closest("[data-crm-tab-drag]");
      if (!tab || !draggedTab) return;
      event.preventDefault();
      crmDropSheet(draggedTab, tab.dataset.crmTabDrag);
      draggedTab = "";
    });
    tabs?.addEventListener("dragend", () => { draggedTab = ""; });
    // The "Import contacts" tab: pick a CSV and it opens as a new tab named
    // after the file, which can then be renamed and shared.
    $(`[data-crm-tab-import]`)?.addEventListener("click", () => crmPickCsv(""));
    $(`[data-crm-shared-copy]`)?.addEventListener("click", () => void copyCrmSharedSheet(crmActiveShared()));
    $(`[data-crm-sheet-new]`)?.addEventListener("click", () => openCrmSheetDialog(null));
    $(`[data-crm-pipeline-new]`)?.addEventListener("click", openCrmPipelineDialog);
    $(`[data-crm-pipeline-form]`)?.addEventListener("submit", saveCrmPipeline);
    $(`[data-crm-sheet-form]`)?.addEventListener("submit", saveCrmSheet);
    $(`[data-crm-sheet-visibility]`)?.addEventListener("change", renderCrmSheetShare);
    $(`[data-crm-sheet-link]`)?.addEventListener("click", (event) => void copyCrmListLink(
      state.crmView.views.find((view) => view.id === $(`[data-crm-sheet-id]`).value), event.currentTarget));
    $(`[data-crm-sheet-delete]`)?.addEventListener("click", () => void deleteCrmSheet(
      state.crmView.views.find((view) => view.id === $(`[data-crm-sheet-id]`).value)));
    // The heading row is replaced on every render, so both the menu and the
    // drag reorder are delegated from the row itself.
    const head = $(`[data-crm-contact-head]`);
    let dragged = "";
    head?.addEventListener("contextmenu", (event) => {
      const cell = event.target.closest("[data-crm-column]");
      if (cell) openCrmColumnMenu(event, cell.dataset.crmColumn);
    });
    head?.addEventListener("dragstart", (event) => {
      const cell = event.target.closest("[data-crm-column]");
      if (!cell || crmActiveShared()) return;
      dragged = cell.dataset.crmColumn;
      event.dataTransfer.effectAllowed = "move";
      event.dataTransfer.setData("text/plain", dragged);
    });
    head?.addEventListener("dragover", (event) => {
      if (dragged && event.target.closest("[data-crm-column]")) {
        event.preventDefault(); event.dataTransfer.dropEffect = "move";
      }
    });
    head?.addEventListener("drop", (event) => {
      const cell = event.target.closest("[data-crm-column]");
      if (!cell || !dragged) return;
      event.preventDefault();
      crmDropColumn(dragged, cell.dataset.crmColumn);
      dragged = "";
    });
    head?.addEventListener("dragend", () => { dragged = ""; });
    document.addEventListener("click", (event) => {
      if (!event.target.closest("[data-crm-menu]")) closeCrmMenu();
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") { closeCrmMenu(); hideCrmShareCard(); }
    });
    $(`[data-crm-install]`)?.addEventListener("click", () => void installCrmPlugin());
    $(`[data-crm-export-all]`)?.addEventListener("click", () => void exportCrmAll());
    $(`[data-crm-uninstall]`)?.addEventListener("click", () => void uninstallCrmPlugin());
    $(`[data-crm-import-file]`)?.addEventListener("change", (event) => void previewCrmCsv(event.target.files?.[0])); $(`[data-crm-export]`)?.addEventListener("click", () => void exportCrmCsv()); $(`[data-crm-account-evidence]`)?.addEventListener("click", () => void checkCrmAccounts()); $$(`[data-crm-bulk-edit]`).forEach((button) => button.addEventListener("click", () => void bulkEditShownCrmContacts(button.dataset.crmBulkEdit)));
    $(`[data-crm-import-errors-close]`)?.addEventListener("click", () => crmImportErrors());
    $(`[data-crm-import-map-form]`)?.addEventListener("submit", submitCrmImportMap);
    $(`[data-crm-mastodon-import]`)?.addEventListener("click", () => openCrmMastodonDialog(""));
    $(`[data-crm-mastodon-form]`)?.addEventListener("submit", (event) => void submitCrmMastodon(event));
    $(`[data-crm-import-map-rows]`)?.addEventListener("change", renderCrmImportMapNote);
    $(`[data-crm-import-map-dialog]`)?.addEventListener("close", () => { crmImportPending = null; });
    // Nothing here re-reads the CRM on its own: a teammate's import, edit or
    // send shows up on the next explicit Refresh. It is the only network read
    // after the page has loaded once.
    $(`[data-crm-refresh]`)?.addEventListener("click", () => void loadCrm());
    $$(`[data-crm-dialog-close], [data-crm-contact-cancel], [data-crm-email-cancel], [data-crm-link-cancel], [data-crm-settings-cancel], [data-crm-sheet-cancel], [data-crm-pipeline-cancel], [data-crm-import-map-cancel], [data-crm-mastodon-cancel]`).forEach((button) => button.addEventListener("click", () => button.closest("dialog")?.close()));
    wireCrmTableParity();
    if (sharedLink !== null) {
      if (!(await openCrmSharedOnly(sharedLink))) crmMessage("error", "Join or create an organization before using CRM. Nothing has been shared with you yet.");
      return;
    }
    openCrmListFromUrl();
    openCrmContactFromUrl();
  }

  return initCrmPage();
});
