  const AGENT_PROFILE_PROVIDERS = Object.freeze({
    "claude-code": "Claude Code",
    codex: "Codex",
  });
  const AGENT_PROFILE_ERRORS = Object.freeze({
    unsupported_provider: "That desktop does not support this provider.",
    unknown_account: "The desktop no longer has that account. Refresh and try again.",
    bad_request: "The desktop refused the request.",
    account_failed: "The desktop could not create the account's folder.",
    cli_missing: "The provider's command-line tool is not installed on that desktop.",
    unknown_repository: "That repository is not checked out on the desktop.",
    guardrails_on: "That repository's agent guardrails do not allow a free-form terminal.",
    terminal_vm: "Agents on that desktop run in its virtual machine, so it cannot open a terminal here.",
    terminal_limit: "Six terminals are already running on that desktop. End one first.",
    terminal_failed: "The desktop could not open a terminal.",
    unsupported: "That desktop cannot open terminals.",
    unsupported_method: "Update the ForkMesh desktop app to do this from the web.",
    unknown_terminal: "That terminal has ended.",
    not_owner: "That desktop is signed in to another account.",
  });
  const AGENT_PROFILE_REFRESH_MS = 1500;
  const agentProfilesView = {
    account: "",
    nodes: new Map(),
    notice: "",
    noticeError: false,
    loaded: false,
  };

  function agentProfileProviderLabel(id) {
    return AGENT_PROFILE_PROVIDERS[id] || String(id || "");
  }

  function agentProfileErrorText(error) {
    const code = String(error?.code || "");
    return AGENT_PROFILE_ERRORS[code] || desktopControlErrorText(error);
  }

  function agentProfileButton(text, onClick, { primary = false, disabled = false, title = "" } = {}) {
    const button = runnersEl("button", primary
      ? "inline-flex items-center gap-1.5 rounded-md bg-primary px-2.5 py-1 text-xs font-semibold text-primary-foreground hover:opacity-90 transition-opacity disabled:opacity-50"
      : "inline-flex items-center gap-1.5 rounded-md border border-border px-2.5 py-1 text-xs font-semibold text-muted-foreground hover:bg-secondary hover:text-foreground transition-colors disabled:opacity-50", text);
    button.type = "button";
    button.disabled = disabled;
    if (title) button.title = title;
    button.addEventListener("click", onClick);
    return button;
  }

  function agentProfileUsage(windows) {
    const box = runnersEl("div", "grid gap-1.5");
    const known = (Array.isArray(windows) ? windows : []).filter((window) => Number(window?.usedPercent) >= 0);
    for (const window of known) {
      box.append(runnerUsageRow({
        id: window.id,
        label: window.label,
        usedPercent: window.usedPercent,
        resetsAt: Number(window.resetAtMs) || 0,
      }));
    }
    if (!known.length) {
      box.append(runnersEl("p", "text-xs text-muted-foreground", "No usage reading yet. It is read while the account is in use on the desktop."));
    }
    return box;
  }

  function agentProfileHas(entry, capability) {
    return entry.capabilities.has(capability);
  }

  function setAgentProfilesNotice(message, isError = false) {
    agentProfilesView.notice = message || "";
    agentProfilesView.noticeError = Boolean(isError);
    const notice = $("[data-agent-profiles-notice]");
    if (!notice) return;
    notice.hidden = !message;
    notice.textContent = message || "";
    notice.classList.toggle("text-red-500", Boolean(isError));
    notice.classList.toggle("text-muted-foreground", !isError);
  }

  function agentProfilesVisible() {
    return Boolean($("[data-agent-profiles-desktops]")) && state.settingsView?.section === "agents";
  }

  function renderAgentProfiles() {
    const list = $("[data-agent-profiles-desktops]");
    if (!list) return;
    list.replaceChildren();
    if (!agentProfilesView.nodes.size) {
      list.append(runnersEl("p", "text-sm text-muted-foreground", agentProfilesView.loaded
        ? "No ForkMesh desktop app is linked to your account yet. Install it and sign in, and its Claude Code and Codex accounts show up here."
        : "Loading desktops…"));
      return;
    }
    for (const entry of agentProfilesView.nodes.values()) list.append(renderAgentProfileDesktop(entry));
    window.lucide?.createIcons();
  }

  function renderAgentProfileDesktop(entry) {
    const card = runnersEl("div", "rounded-md border border-border bg-card p-4 grid gap-4");
    card.dataset.agentProfileDesktop = entry.node;
    const head = runnersEl("div", "flex flex-wrap items-center gap-2");
    head.append(runnersEl("i", "h-4 w-4 text-muted-foreground"));
    head.lastChild.dataset.lucide = "monitor";
    head.append(runnersEl("span", "text-sm font-semibold text-foreground", entry.name || entry.node));
    if (entry.name && entry.name !== entry.node) {
      head.append(runnersEl("span", "font-mono text-xs text-muted-foreground", entry.node));
    }
    const stateText = entry.status === "open" ? "Connected"
      : entry.status === "connecting" ? "Connecting…" : "Not reachable";
    head.append(runnersEl("span", "text-xs " + (entry.status === "open" ? "text-emerald-500"
      : entry.status === "error" ? "text-red-500" : "text-muted-foreground"), stateText));
    if (entry.status !== "connecting") {
      const actions = runnersEl("div", "ml-auto flex items-center gap-2");
      actions.append(agentProfileButton(entry.status === "open" ? "Refresh" : "Try again", () => {
        if (entry.status === "open") void refreshAgentProfileDesktop(entry);
        else void connectAgentProfileDesktop(entry.node);
      }, { disabled: entry.busy }));
      head.append(actions);
    }
    card.append(head);
    if (entry.status === "error") {
      card.append(runnersEl("p", "text-sm text-muted-foreground", entry.error || "The desktop did not answer."));
      return card;
    }
    if (entry.status !== "open") return card;
    if (entry.error) card.append(runnersEl("p", "text-sm text-red-500", entry.error));
    if (!entry.usage) {
      card.append(runnersEl("p", "text-sm text-muted-foreground", "Reading accounts and usage…"));
      return card;
    }
    const providers = Array.isArray(entry.usage.providers) ? entry.usage.providers : [];
    if (!providers.length) {
      card.append(runnersEl("p", "text-sm text-muted-foreground", "This desktop reported no Claude Code or Codex accounts."));
    }
    if (agentProfileHas(entry, "terminal.agents") && entry.repos.length) {
      card.append(renderAgentProfileRepoPicker(entry));
    }
    for (const provider of providers) card.append(renderAgentProfileProvider(entry, provider));
    if (entry.terminals.length) card.append(renderAgentProfileTerminals(entry));
    const spend = entry.usage.spend || {};
    const spendParts = [];
    if (typeof spend.claudeUsd === "number") spendParts.push(`Anthropic API $${spend.claudeUsd.toFixed(2)}`);
    if (typeof spend.openAiUsd === "number") spendParts.push(`OpenAI API $${spend.openAiUsd.toFixed(2)}`);
    if (spendParts.length) {
      card.append(runnersEl("p", "text-xs text-muted-foreground", `API spend this month: ${spendParts.join(" · ")}`));
    }
    return card;
  }

  function renderAgentProfileRepoPicker(entry) {
    const row = runnersEl("label", "flex flex-wrap items-center gap-2 text-xs text-muted-foreground");
    row.append(runnersEl("span", "", "Agent terminals open in"));
    const select = runnersEl("select", "min-w-0 max-w-xs rounded-md border border-border bg-background px-2 py-1 text-xs text-foreground");
    select.dataset.agentProfileRepo = entry.node;
    for (const repo of entry.repos) {
      const option = runnersEl("option", "", repo.repo);
      option.value = repo.repo;
      option.selected = repo.repo === entry.repo;
      select.append(option);
    }
    select.addEventListener("change", () => { entry.repo = select.value; });
    row.append(select);
    return row;
  }

  function renderAgentProfileProvider(entry, provider) {
    const box = runnersEl("div", "grid gap-3 border-t border-border pt-3");
    box.dataset.agentProfileProvider = provider.id;
    const head = runnersEl("div", "flex flex-wrap items-center gap-2");
    head.append(runnersEl("span", "text-sm font-semibold text-foreground", provider.label || agentProfileProviderLabel(provider.id)));
    if (entry.usage.activeProvider === provider.id) {
      head.append(runnersEl("span", "rounded-full border border-border px-2 py-0.5 text-[10px] font-semibold uppercase tracking-wide text-muted-foreground", "New agents"));
    }
    const actions = runnersEl("div", "ml-auto flex flex-wrap items-center gap-2");
    if (agentProfileHas(entry, "terminal.agents")) {
      actions.append(agentProfileButton("New terminal", () => {
        void openAgentProfileTerminal(entry, { provider: provider.id, mode: "agent" });
      }, {
        disabled: !entry.repo,
        title: entry.repo ? `Start ${provider.label} in ${entry.repo} on the desktop, with full permissions` : "Check out a repository on the desktop first.",
      }));
    }
    if (agentProfileHas(entry, "agentops.accounts")) {
      actions.append(agentProfileButton("Add account", () => { void addAgentProfileAccount(entry, provider); }, { disabled: entry.busy }));
    }
    head.append(actions);
    box.append(head);
    const accounts = Array.isArray(provider.accounts) ? provider.accounts : [];
    if (!accounts.length) {
      box.append(agentProfileUsage(provider.windows));
    }
    for (const account of accounts) box.append(renderAgentProfileAccount(entry, provider, account));
    if (provider.credits) {
      const credits = provider.credits;
      const parts = [];
      if (credits.used !== undefined) parts.push(`${credits.used} used`);
      if (credits.limit !== undefined) parts.push(`of ${credits.limit}`);
      if (parts.length) box.append(runnersEl("p", "text-xs text-muted-foreground", `Extra usage credits: ${parts.join(" ")} ${credits.currency || ""}`.trim()));
    }
    return box;
  }

  function renderAgentProfileAccount(entry, provider, account) {
    const item = runnersEl("div", "grid gap-2 rounded-md border border-border bg-background p-3");
    item.dataset.agentProfileAccount = account.id;
    const head = runnersEl("div", "flex flex-wrap items-center gap-2");
    head.append(runnersEl("span", "text-sm text-foreground", account.label || "Account"));
    if (account.active) head.append(runnersEl("span", "text-xs font-semibold text-emerald-500", "In use"));
    const signedIn = account.signedIn !== false;
    head.append(runnersEl("span", "text-xs " + (signedIn ? "text-muted-foreground" : "text-amber-600"),
      account.identity || (signedIn ? "Signed in" : "Not signed in")));
    item.append(head);
    const windows = account.active && Array.isArray(provider.windows) && provider.windows.length
      ? provider.windows : account.windows;
    item.append(agentProfileUsage(windows));
    const actions = runnersEl("div", "flex flex-wrap items-center gap-2");
    const accounts = agentProfileHas(entry, "agentops.accounts");
    const terminals = agentProfileHas(entry, "terminal.accounts");
    if (accounts && !account.active) {
      actions.append(agentProfileButton("Use for new agents", () => {
        void agentProfileAccountRequest(entry, "agentops.accounts.select", { provider: provider.id, account: account.id });
      }, { primary: true, disabled: entry.busy }));
    }
    if (terminals) {
      actions.append(agentProfileButton(signedIn ? "Log in again" : "Log in", () => {
        void openAgentProfileTerminal(entry, { provider: provider.id, mode: "login", account: account.id }, account);
      }));
      if (signedIn) {
        actions.append(agentProfileButton("Log out", () => {
          if (!window.confirm(`Log ${account.label || "this account"} out of ${provider.label} on ${entry.name || entry.node}?`)) return;
          void openAgentProfileTerminal(entry, { provider: provider.id, mode: "logout", account: account.id }, account);
        }));
      }
      if (agentProfileHas(entry, "terminal.agents")) {
        actions.append(agentProfileButton("Terminal as this account", () => {
          void openAgentProfileTerminal(entry, { provider: provider.id, mode: "agent", account: account.id }, account);
        }, { disabled: !entry.repo, title: entry.repo ? "" : "Check out a repository on the desktop first." }));
      }
    } else if (agentProfileHas(entry, "terminal.agents") || !accounts) {
      actions.append(runnersEl("span", "text-xs text-muted-foreground", "Update the ForkMesh desktop app to log in and out from here."));
    }
    if (accounts) {
      actions.append(agentProfileButton("Rename", () => {
        const label = window.prompt(`Rename ${account.label || "this account"}`, account.label || "");
        if (label === null || !label.trim()) return;
        void agentProfileAccountRequest(entry, "agentops.accounts.rename", { provider: provider.id, account: account.id, label: label.trim() });
      }, { disabled: entry.busy }));
    }
    if (actions.childElementCount) item.append(actions);
    return item;
  }

  function renderAgentProfileTerminals(entry) {
    const box = runnersEl("div", "grid gap-2 border-t border-border pt-3");
    box.append(runnersEl("span", "text-sm font-semibold text-foreground", "Running terminals"));
    for (const terminal of entry.terminals) {
      const row = runnersEl("div", "flex flex-wrap items-center gap-2 text-xs");
      row.dataset.agentProfileTerminal = terminal.terminal;
      const where = terminal.repositoryOwner && terminal.repositoryName
        ? `${terminal.repositoryOwner}/${terminal.repositoryName}` : "sign-in";
      row.append(runnersEl("span", "text-foreground", `${terminal.label || agentProfileProviderLabel(terminal.provider)} · ${where}`));
      if (terminal.device) row.append(runnersEl("span", "text-muted-foreground", `started from ${terminal.device}`));
      const actions = runnersEl("div", "ml-auto flex items-center gap-2");
      actions.append(agentProfileButton("Open", () => {
        void openAgentProfileTerminal(entry, null, null, terminal);
      }));
      actions.append(agentProfileButton("End", () => {
        if (!window.confirm("End this terminal? The program running in it stops.")) return;
        void endAgentProfileTerminal(entry, terminal.terminal);
      }));
      row.append(actions);
      box.append(row);
    }
    return box;
  }

  async function loadAgentProfiles() {
    if (!$("[data-agent-profiles-desktops]")) return;
    const account = String(state.session?.nodeName || "").toLowerCase();
    if (agentProfilesView.account !== account) {
      for (const entry of agentProfilesView.nodes.values()) dropAgentProfileListeners(entry);
      agentProfilesView.nodes = new Map();
      agentProfilesView.account = account;
      agentProfilesView.loaded = false;
    }
    if (!state.session?.sessionToken) {
      agentProfilesView.loaded = true;
      setAgentProfilesNotice("Sign in to manage your desktops' agent accounts.", true);
      renderAgentProfiles();
      return;
    }
    let desktops;
    try {
      const hub = await desktopControlHub();
      desktops = await hub.desktops();
    } catch (error) {
      agentProfilesView.loaded = true;
      setAgentProfilesNotice(desktopControlErrorText(error), true);
      renderAgentProfiles();
      return;
    }
    agentProfilesView.loaded = true;
    setAgentProfilesNotice("");
    const seen = new Set();
    for (const desktop of desktops) {
      if (seen.has(desktop.node)) continue;
      seen.add(desktop.node);
      const entry = agentProfilesView.nodes.get(desktop.node);
      if (entry?.status === "open") void refreshAgentProfileDesktop(entry);
      else if (entry?.status !== "connecting") void connectAgentProfileDesktop(desktop.node);
    }
    for (const [node, entry] of agentProfilesView.nodes) {
      if (seen.has(node)) continue;
      dropAgentProfileListeners(entry);
      agentProfilesView.nodes.delete(node);
    }
    renderAgentProfiles();
  }

  function dropAgentProfileListeners(entry) {
    for (const off of entry.listeners || []) off();
    entry.listeners = [];
    if (entry.refreshTimer) window.clearTimeout(entry.refreshTimer);
    entry.refreshTimer = 0;
  }

  async function connectAgentProfileDesktop(node) {
    const previous = agentProfilesView.nodes.get(node);
    if (previous) dropAgentProfileListeners(previous);
    const entry = {
      node,
      name: previous?.name || "",
      status: "connecting",
      error: "",
      connection: null,
      capabilities: new Set(),
      usage: null,
      repos: [],
      repo: previous?.repo || "",
      terminals: [],
      busy: false,
      listeners: [],
      refreshTimer: 0,
    };
    agentProfilesView.nodes.set(node, entry);
    renderAgentProfiles();
    try {
      const hub = await desktopControlHub();
      const connection = await hub.connect(node);
      if (agentProfilesView.nodes.get(node) !== entry) return;
      entry.connection = connection;
      entry.status = "open";
      entry.name = String(connection.info?.name || node);
      entry.listeners.push(connection.on("repos.changed", () => scheduleAgentProfileRefresh(entry)));
      entry.listeners.push(connection.on("agents.changed", () => scheduleAgentProfileRefresh(entry, { terminalsOnly: true })));
      entry.listeners.push(connection.onClose((reason) => {
        if (agentProfilesView.nodes.get(node) !== entry) return;
        dropAgentProfileListeners(entry);
        entry.status = "error";
        entry.error = desktopControlErrorText({ message: reason === "closed" ? "The channel to the desktop closed." : reason });
        renderAgentProfiles();
      }));
      await refreshAgentProfileDesktop(entry);
    } catch (error) {
      if (agentProfilesView.nodes.get(node) !== entry) return;
      entry.status = "error";
      entry.error = agentProfileErrorText(error);
      renderAgentProfiles();
    }
  }

  function scheduleAgentProfileRefresh(entry, { terminalsOnly = false } = {}) {
    if (!agentProfilesVisible()) return;
    if (entry.refreshTimer) {
      if (!terminalsOnly) entry.refreshFull = true;
      return;
    }
    entry.refreshFull = !terminalsOnly;
    entry.refreshTimer = window.setTimeout(() => {
      entry.refreshTimer = 0;
      if (agentProfilesView.nodes.get(entry.node) !== entry || entry.status !== "open") return;
      if (entry.refreshFull) void refreshAgentProfileDesktop(entry);
      else void refreshAgentProfileTerminals(entry);
    }, AGENT_PROFILE_REFRESH_MS);
  }

  async function refreshAgentProfileTerminals(entry) {
    if (!agentProfileHas(entry, "terminal.agents") || !entry.connection?.isOpen) return;
    try {
      const result = await entry.connection.request("terminal.list", {});
      entry.terminals = Array.isArray(result.terminals) ? result.terminals : [];
    } catch (_) {
      return;
    }
    if (agentProfilesView.nodes.get(entry.node) === entry) renderAgentProfiles();
  }

  async function refreshAgentProfileDesktop(entry) {
    const connection = entry.connection;
    if (!connection?.isOpen) return;
    entry.busy = true;
    renderAgentProfiles();
    try {
      const status = await connection.request("status", {});
      entry.capabilities = new Set((Array.isArray(status.capabilities) ? status.capabilities : []).map(String));
      const [usage, repos, terminals] = await Promise.all([
        connection.request("agentops.usage", {}),
        agentProfileHas(entry, "repos") ? connection.request("repos.list", {}).catch(() => ({})) : Promise.resolve({}),
        agentProfileHas(entry, "terminal.agents") ? connection.request("terminal.list", {}).catch(() => ({})) : Promise.resolve({}),
      ]);
      entry.usage = usage;
      entry.repos = (Array.isArray(repos.repos) ? repos.repos : []).filter((repo) => repo?.checkout && repo.repo);
      if (!entry.repos.some((repo) => repo.repo === entry.repo)) entry.repo = entry.repos[0]?.repo || "";
      entry.terminals = Array.isArray(terminals.terminals) ? terminals.terminals : [];
      entry.error = "";
    } catch (error) {
      entry.error = agentProfileErrorText(error);
    } finally {
      entry.busy = false;
    }
    if (agentProfilesView.nodes.get(entry.node) === entry) renderAgentProfiles();
  }

  async function agentProfileAccountRequest(entry, method, params) {
    if (!entry.connection?.isOpen) return null;
    entry.busy = true;
    renderAgentProfiles();
    let result = null;
    try {
      result = await entry.connection.request(method, params);
      if (Array.isArray(result.providers)) entry.usage = { ...entry.usage, ...result };
      entry.error = "";
    } catch (error) {
      entry.error = agentProfileErrorText(error);
    } finally {
      entry.busy = false;
    }
    renderAgentProfiles();
    return result;
  }

  async function addAgentProfileAccount(entry, provider) {
    const label = window.prompt(`Name the new ${provider.label} account (for example "Work")`, "");
    if (label === null || !label.trim()) return;
    const result = await agentProfileAccountRequest(entry, "agentops.accounts.add", { provider: provider.id, label: label.trim() });
    const account = String(result?.account || "");
    if (!account || !agentProfileHas(entry, "terminal.accounts")) return;
    await openAgentProfileTerminal(entry, { provider: provider.id, mode: "login", account }, { id: account, label: label.trim() });
  }

  async function endAgentProfileTerminal(entry, terminal) {
    try {
      await entry.connection.request("terminal.close", { terminal });
    } catch (error) {
      entry.error = agentProfileErrorText(error);
    }
    await refreshAgentProfileTerminals(entry);
  }

  async function openAgentProfileTerminal(entry, start, account = null, existing = null) {
    const desktop = entry.name || entry.node;
    const provider = existing?.provider || start?.provider || "";
    const providerLabel = agentProfileProviderLabel(provider);
    const mode = existing ? (existing.mode || "agent") : start.mode;
    const signIn = mode === "login" || mode === "logout";
    let repo = "";
    let request = start ? { ...start } : null;
    if (request && mode === "agent") {
      repo = entry.repo;
      const [owner, name] = repo.split("/");
      if (!owner || !name) {
        entry.error = "Check out a repository on the desktop first.";
        renderAgentProfiles();
        return;
      }
      request = { ...request, owner, name };
    }
    const who = account?.label ? ` (${account.label})` : "";
    const title = existing
      ? `${providerLabel} terminal on ${desktop}`
      : mode === "login" ? `Log in to ${providerLabel}${who} on ${desktop}`
        : mode === "logout" ? `Log out of ${providerLabel}${who} on ${desktop}`
          : `${providerLabel}${who} in ${repo} on ${desktop}`;
    const modal = showAgentLoginModal(title, `Starting on ${desktop}…`, signIn
      ? "Keystrokes and screen updates are end-to-end encrypted between this tab and your desktop; the relay only forwards them. The desktop runs the provider's own sign-in command for this account, and the credential is created on the provider's site, never here."
      : "Keystrokes and screen updates are end-to-end encrypted between this tab and your desktop; the relay only forwards them. The agent runs on the desktop with full permissions in the repository shown above.");
    const surface = modal?.querySelector("[data-agent-login-surface]");
    if (!modal || !surface || !entry.connection?.isOpen) {
      if (modal) setAgentLoginStatus("The desktop is not connected. Refresh and try again.", "error");
      return;
    }
    let createDesktopControlTerminal;
    try {
      ({ createDesktopControlTerminal } = await import("/agent-terminal.js"));
    } catch (_) {
      setAgentLoginStatus("This browser could not load the terminal module.", "error");
      return;
    }
    if (modal.classList.contains("hidden")) return;
    const terminal = createDesktopControlTerminal({
      surface,
      connection: entry.connection,
      label: title,
      start: request,
      attach: existing?.terminal || "",
      detachOnClose: !signIn,
      onStatus: setAgentLoginStatus,
      onExit: () => { void refreshAgentProfileDesktop(entry); },
    });
    state.agentLoginTerminal = {
      close() {
        terminal.close();
        void refreshAgentProfileDesktop(entry);
      },
    };
    try {
      await terminal.start();
    } catch (error) {
      setAgentLoginStatus(agentProfileErrorText(error), "error");
      return;
    }
    setAgentLoginStatus(signIn
      ? `Live on ${desktop}. Follow the provider's own steps below; open the link it prints to finish.`
      : `Running on ${desktop}. Closing this window leaves it running; End it under Running terminals.`, "ok");
    void refreshAgentProfileTerminals(entry);
  }

  function initSettingsAgents() {
    const refresh = $("[data-agent-profiles-refresh]");
    if (refresh && refresh.dataset.wired !== "1") {
      refresh.dataset.wired = "1";
      refresh.addEventListener("click", () => { void loadAgentProfiles(); });
    }
    renderAgentProfiles();
    void loadAgentProfiles();
    initSettingsRunners();
  }
