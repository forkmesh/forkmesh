  const AGENTS_ACTIVE_STATUSES = new Set(["queued", "running", "security_pending"]);
  const AGENTS_FAILED_STATUSES = new Set(["failed", "rejected", "stopped"]);
  const AGENTS_FILTER_KEYS = Object.freeze(["status", "repo", "node"]);
  const AGENTS_STATUS_LABELS = Object.freeze({
    running: "Working",
    queued: "Queued",
    security_pending: "Security review",
    completed: "Completed",
    failed: "Failed",
    rejected: "Rejected",
    stopped: "Stopped",
    attention: "Needs attention",
    stopping: "Stopping",
    desktop: "On desktop",
  });
  const AGENTS_PROVIDER_LABELS = Object.freeze({
    "claude-code": "Claude Code",
    codex: "Codex",
    "local-llama": "Local model",
  });
  const AGENTS_AMBER_TEXT = "text-amber-500 [[data-dashboard-theme=light]_&]:text-amber-700";
  const agentsPageView = {
    cloud: [],
    desktop: [],
    loading: true,
    signedOut: false,
    cloudError: "",
    desktopError: "",
    finishedBefore: 0,
    desktopTruncated: false,
    filters: { status: "", repo: "", node: "" },
    reads: { cloud: { busy: false, again: false }, desktop: { busy: false, again: false } },
  };

  function agentsSessionHref(owner, repo, id) {
    const base = `${repoPathUrl({ owner, name: repo })}/agents`;
    return id ? `${base}?session=${encodeURIComponent(id)}` : base;
  }

  function agentsCloudRow(session) {
    const status = String(session?.status || "");
    const info = session?.agentInfo && typeof session.agentInfo === "object" ? session.agentInfo : {};
    const owner = String(session?.org || "");
    const repo = String(session?.repo || "");
    const id = String(session?.id || "");
    const provider = String(session?.provider || "");
    const attention = String(session?.displayStatus || "") === "attention";
    const stopping = AGENTS_ACTIVE_STATUSES.has(status) && Number(session?.stopRequestedAt) > 0;
    return {
      key: `cloud:${id}`,
      kind: session?.scope === "organization" ? "organization" : "cloud",
      id,
      title: String(session?.title || "").trim() || `${AGENTS_PROVIDER_LABELS[provider] || "Agent"} session`,
      owner,
      repo,
      provider: AGENTS_PROVIDER_LABELS[provider] || provider,
      model: String(info.model || session?.requestedModel || ""),
      node: String(session?.targetNode || ""),
      status,
      badge: stopping ? "stopping" : attention ? "attention" : status,
      active: AGENTS_ACTIVE_STATUSES.has(status),
      live: status === "running",
      attention,
      createdAt: Number(session?.createdAt) || 0,
      updatedAt: Number(session?.updatedAt) || 0,
      createdBy: String(session?.createdBy || ""),
      message: attention ? String(session?.diagnostic?.message || "") : "",
      href: owner && repo && id ? agentsSessionHref(owner, repo, id) : "",
    };
  }

  function agentsDesktopRow(agent) {
    const owner = String(agent?.owner || "");
    const repo = String(agent?.repo || "");
    const id = String(agent?.id || "");
    return {
      key: `desktop:${owner}/${repo}:${id}`,
      kind: "desktop",
      id,
      title: `Desktop session ${id}`,
      owner,
      repo,
      provider: "",
      model: "",
      node: String(agent?.node || owner),
      status: "desktop",
      badge: "desktop",
      active: false,
      live: false,
      attention: false,
      createdAt: 0,
      updatedAt: Number(agent?.reportedAt) || 0,
      createdBy: "",
      message: "",
      href: owner && repo ? agentsSessionHref(owner, repo, "") : "",
    };
  }

  function agentsRows() {
    return [...agentsPageView.cloud, ...agentsPageView.desktop]
      .sort((a, b) => (Number(b.live) - Number(a.live))
        || (Number(b.active) - Number(a.active))
        || (b.updatedAt - a.updatedAt));
  }

  function agentsRepoLabel(row) {
    return row.owner && row.repo ? repoPublicKey(`${row.owner}/${row.repo}`) : row.repo || "";
  }

  function agentsMatchStatus(row, wanted) {
    if (!wanted) return true;
    if (wanted === "active") return row.active;
    if (wanted === "attention") return row.attention;
    if (wanted === "completed") return row.status === "completed";
    if (wanted === "failed") return AGENTS_FAILED_STATUSES.has(row.status);
    if (wanted === "desktop") return row.kind === "desktop";
    return row.status === wanted;
  }

  function agentsFilteredRows(rows = agentsRows()) {
    const { status, repo, node } = agentsPageView.filters;
    return rows.filter((row) => agentsMatchStatus(row, status)
      && (!repo || [agentsRepoLabel(row), `${row.owner}/${row.repo}`].some((value) => value.toLowerCase() === repo))
      && (!node || row.node.toLowerCase() === node));
  }

  function agentsClockHtml(label, value) {
    const date = Number(value) > 0 ? parseFlexibleDate(value) : null;
    if (!date) return "";
    return `<span>${escapeHtml(label)} <time datetime="${escapeHtml(date.toISOString())}" title="${escapeHtml(date.toLocaleString())}">${escapeHtml(formatTimeAgo(value))}</time></span>`;
  }

  function agentsStatusIcon(row) {
    if (row.kind === "desktop") return { icon: "monitor", tone: "text-muted-foreground" };
    if (row.attention) return { icon: "triangle-alert", tone: "text-amber-500" };
    if (row.badge === "stopping") return { icon: "circle-stop", tone: "text-muted-foreground" };
    if (row.live) return { icon: "loader-circle", tone: "text-amber-500 animate-spin" };
    if (row.status === "completed") return { icon: "circle-check", tone: "text-success" };
    if (row.status === "stopped") return { icon: "circle-slash", tone: "text-muted-foreground" };
    if (AGENTS_FAILED_STATUSES.has(row.status)) return { icon: "circle-x", tone: "text-destructive" };
    if (row.active) return { icon: "circle-dashed", tone: "text-amber-500" };
    return { icon: "circle", tone: "text-muted-foreground" };
  }

  function agentsBadgeTone(row) {
    if (row.kind === "desktop" || row.badge === "stopping" || row.status === "stopped") return "text-muted-foreground";
    if (row.attention || row.active) return AGENTS_AMBER_TEXT;
    if (row.status === "completed") return "text-success";
    if (AGENTS_FAILED_STATUSES.has(row.status)) return "text-destructive";
    return "text-muted-foreground";
  }

  function agentsKindLabel(row) {
    if (row.kind === "organization") return "Organization run";
    if (row.kind === "desktop") return "Desktop";
    return "Cloud run";
  }

  function renderAgentsRow(row) {
    const repoLabel = agentsRepoLabel(row);
    const runtime = [row.provider, row.model].filter(Boolean).join(" · ");
    const badge = AGENTS_STATUS_LABELS[row.badge] || row.badge || "Unknown";
    const status = agentsStatusIcon(row);
    const updated = agentsClockHtml(row.kind === "desktop" ? "Reported" : "Updated", row.updatedAt);
    const started = agentsClockHtml("Started", row.createdAt);
    const body = `
        <span class="mt-0.5 shrink-0" title="${escapeHtml(badge)}"><i data-lucide="${status.icon}" class="h-4 w-4 ${status.tone}" aria-hidden="true"></i></span>
        <div class="grid min-w-0 flex-1 gap-1">
          <div class="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1">
            <span class="min-w-0 truncate font-semibold text-foreground${row.href ? " group-hover:text-accent" : ""}">${escapeHtml(row.title)}</span>
            <span data-agents-status="${escapeHtml(row.badge)}" class="shrink-0 rounded-full border border-border px-2 text-xs font-medium leading-5 ${agentsBadgeTone(row)}">${escapeHtml(badge)}</span>
          </div>
          <div class="flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground">
            ${repoLabel ? `<span class="font-medium text-foreground">${escapeHtml(repoLabel)}</span>` : ""}
            <span>${escapeHtml(agentsKindLabel(row))}</span>
            ${runtime ? `<span>${escapeHtml(runtime)}</span>` : ""}
            ${row.node ? `<span class="inline-flex items-center gap-1"><i data-lucide="server" class="h-3 w-3 text-muted-foreground" aria-hidden="true"></i><span class="font-mono">${escapeHtml(row.node)}</span></span>` : ""}
            ${row.createdBy ? `<span>by ${escapeHtml(row.createdBy)}</span>` : ""}
            ${started ? `<span class="sm:hidden">${started}</span>` : ""}
            ${updated ? `<span class="sm:hidden">${updated}</span>` : ""}
          </div>
          ${row.message ? `<p class="text-xs ${AGENTS_AMBER_TEXT}">${escapeHtml(row.message)}</p>` : ""}
          ${row.kind === "desktop" ? `<p class="text-xs text-muted-foreground">End-to-end encrypted: the relay holds only a sealed copy. Open the repository's Agents tab while this desktop is running to follow and manage it.</p>` : ""}
        </div>
        ${started || updated ? `<div class="hidden shrink-0 gap-1 text-right text-xs text-muted-foreground sm:grid">${updated ? `<span class="inline-flex items-center justify-end gap-1"><i data-lucide="clock" class="h-3 w-3" aria-hidden="true"></i>${updated}</span>` : ""}${started}</div>` : ""}`;
    const attrs = `data-agents-row="${escapeHtml(row.key)}" data-agents-kind="${escapeHtml(row.kind)}"`;
    if (row.href) {
      return `<a ${attrs} href="${escapeHtml(row.href)}" class="group flex items-start gap-3 px-4 py-3 text-sm transition-colors hover:bg-[rgb(var(--dashboard-subtle-rgb))] focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-accent">${body}</a>`;
    }
    return `<div ${attrs} class="flex items-start gap-3 px-4 py-3 text-sm">${body}</div>`;
  }

  function agentsBlankslate(icon, heading, detail) {
    return `<div class="px-6 py-12 text-center">
        <i data-lucide="${icon}" class="mx-auto h-6 w-6 text-muted-foreground" aria-hidden="true"></i>
        <h3 class="mt-3 text-base font-semibold text-foreground">${escapeHtml(heading)}</h3>
        <p class="mx-auto mt-1 max-w-md text-sm text-muted-foreground">${detail}</p>
      </div>`;
  }

  function renderAgentsFilterOptions(rows) {
    const choices = {
      repo: [...new Set(rows.map((row) => agentsRepoLabel(row).toLowerCase()).filter(Boolean))].sort(),
      node: [...new Set(rows.map((row) => row.node.toLowerCase()).filter(Boolean))].sort(),
    };
    for (const key of ["repo", "node"]) {
      const select = $(`[data-agents-filter="${key}"]`);
      if (!select) continue;
      const wanted = agentsPageView.filters[key];
      const values = wanted && !choices[key].includes(wanted) ? [wanted, ...choices[key]] : choices[key];
      select.innerHTML = `<option value="">All</option>${values.map((value) => `<option value="${escapeHtml(value)}">${escapeHtml(value)}</option>`).join("")}`;
      select.value = wanted;
    }
    const status = $('[data-agents-filter="status"]');
    if (status) status.value = agentsPageView.filters.status;
  }

  function setAgentsNotice(message, isError) {
    const notice = $("[data-agents-notice]");
    if (!notice) return;
    notice.hidden = !message;
    notice.replaceChildren();
    if (!message) return;
    const flash = document.createElement("div");
    flash.className = isError ? "fm-flash fm-flash-danger" : "fm-flash fm-flash-neutral";
    const icon = document.createElement("i");
    icon.setAttribute("data-lucide", isError ? "circle-alert" : "info");
    icon.setAttribute("aria-hidden", "true");
    const body = document.createElement("span");
    body.className = "fm-flash-body";
    body.textContent = message;
    flash.append(icon, body);
    notice.append(flash);
    window.lucide?.createIcons();
  }

  function agentsCountText(rows) {
    const working = rows.filter((row) => row.active).length;
    const total = `${formatCount(rows.length)} ${rows.length === 1 ? "session" : "sessions"}`;
    return working ? `${total} · ${formatCount(working)} working` : total;
  }

  function setAgentsSetup(show) {
    const setup = $("[data-agents-setup]");
    if (setup) setup.hidden = !show;
  }

  function agentsNothingCanRun(rows) {
    if (agentsPageView.loading || agentsPageView.cloudError || agentsPageView.desktopError) return false;
    if (rows.length) return false;
    const picker = $("[data-runner-accounts-picker]");
    return Boolean(picker && picker.hidden);
  }

  function renderAgentsPage() {
    const list = $("[data-agents-list]");
    if (!list) return;
    const count = $("[data-agents-count]");
    const create = $("[data-agents-new]");
    if (create) {
      create.disabled = agentsPageView.signedOut;
      create.title = agentsPageView.signedOut ? "Sign in to start an agent" : "Start an agent";
    }
    if (agentsPageView.signedOut) {
      if (count) count.textContent = "Signed out";
      setAgentsNotice("");
      setAgentsSetup(false);
      list.innerHTML = agentsBlankslate("bot", "Sign in to see your agents",
        `Agent runs from your repositories, organizations and desktops are listed here. <a href="/login?next=${encodeURIComponent("/dashboard/agents")}" class="font-medium text-accent hover:underline">Sign in</a>`);
      window.lucide?.createIcons();
      return;
    }
    if (agentsPageView.loading && !agentsPageView.cloud.length && !agentsPageView.desktop.length) {
      if (count) count.textContent = "Loading";
      list.innerHTML = `<div class="px-4 py-10 text-center text-sm text-muted-foreground">${loadingHtml("Loading agents...")}</div>`;
      return;
    }
    const rows = agentsRows();
    const visible = agentsFilteredRows(rows);
    if (count) count.textContent = agentsCountText(rows);
    renderAgentsFilterOptions(rows);
    const problems = [agentsPageView.cloudError, agentsPageView.desktopError].filter(Boolean);
    const notes = [];
    if (agentsPageView.finishedBefore > 0) {
      notes.push("Older finished cloud runs are listed in each repository's Agents tab.");
    }
    if (agentsPageView.desktopTruncated) {
      notes.push("Only the most recently reported desktop sessions are shown.");
    }
    setAgentsNotice(problems.length ? problems.join(" ") : notes.join(" "), problems.length > 0);
    setAgentsSetup(agentsNothingCanRun(rows));
    const filtered = AGENTS_FILTER_KEYS.some((key) => agentsPageView.filters[key]);
    list.innerHTML = visible.length
      ? visible.map(renderAgentsRow).join("")
      : filtered
        ? agentsBlankslate("search", "No agents match these filters",
          `<button data-agents-clear-filters type="button" class="font-medium text-accent hover:underline">Clear filters</button>`)
        : agentsBlankslate("bot", "No agent sessions yet",
          `Start one with New agent, from a repository's Agents tab, or from the ${desktopBrand()}desktop.`);
    window.lucide?.createIcons();
  }

  function openAgentsComposer() {
    if (agentsPageView.signedOut) return;
    if (window.ForkMeshComposer?.open) {
      window.ForkMeshComposer.open();
      return;
    }
    $("#agentModalToggle")?.click();
  }

  function agentsErrorText(error, fallback) {
    if (Number(error?.status) === 401) return "Your session has expired. Sign in again to see your agents.";
    return String(error?.message || "") || fallback;
  }

  async function agentsRead(which, read) {
    const guard = agentsPageView.reads[which];
    if (guard.busy) {
      guard.again = true;
      return;
    }
    guard.busy = true;
    try {
      await read();
    } finally {
      guard.busy = false;
      if (guard.again) {
        guard.again = false;
        void agentsRead(which, read);
      }
    }
  }

  function loadAgentsCloud() {
    return agentsRead("cloud", async () => {
      try {
        const payload = await orgAgentRequest("GET", "/api/account/agent-sessions");
        agentsPageView.cloud = (Array.isArray(payload?.sessions) ? payload.sessions : []).map(agentsCloudRow);
        agentsPageView.finishedBefore = Number(payload?.finishedBefore) || 0;
        agentsPageView.cloudError = "";
      } catch (error) {
        agentsPageView.cloudError = agentsErrorText(error, "Cloud agent runs are unavailable right now.");
      }
      renderAgentsPage();
    });
  }

  function loadAgentsDesktop() {
    return agentsRead("desktop", async () => {
      try {
        const payload = await orgAgentRequest("GET", "/api/account/desktop-agents");
        agentsPageView.desktop = (Array.isArray(payload?.agents) ? payload.agents : []).map(agentsDesktopRow);
        agentsPageView.desktopTruncated = payload?.truncated === true;
        agentsPageView.desktopError = "";
      } catch (error) {
        agentsPageView.desktopError = agentsErrorText(error, "Desktop agent sessions are unavailable right now.");
      }
      renderAgentsPage();
    });
  }

  async function loadAgentsPage() {
    if (!state.session?.sessionToken) {
      agentsPageView.signedOut = true;
      agentsPageView.loading = false;
      renderAgentsPage();
      return;
    }
    agentsPageView.signedOut = false;
    agentsPageView.loading = true;
    renderAgentsPage();
    await Promise.all([loadAgentsCloud(), loadAgentsDesktop(), loadRunnerAccountsPicker(true)]);
    agentsPageView.loading = false;
    renderAgentsPage();
  }

  function syncAgentsFilterUrl() {
    const params = new URLSearchParams(location.search);
    for (const key of AGENTS_FILTER_KEYS) {
      if (agentsPageView.filters[key]) params.set(key, agentsPageView.filters[key]);
      else params.delete(key);
    }
    const query = params.toString();
    try {
      history.replaceState(history.state, "", `${location.pathname}${query ? `?${query}` : ""}${location.hash}`);
    } catch (_) {}
  }

  function initAgentsPage() {
    if (!$("[data-agents-panel]")) return;
    const params = new URLSearchParams(location.search);
    for (const key of AGENTS_FILTER_KEYS) {
      agentsPageView.filters[key] = String(params.get(key) || "").trim().toLowerCase();
    }
    $("[data-agents-refresh]")?.addEventListener("click", () => { void loadAgentsPage(); });
    $("[data-agents-new]")?.addEventListener("click", openAgentsComposer);
    $("[data-agents-list]")?.addEventListener("click", (event) => {
      if (!event.target?.closest?.("[data-agents-clear-filters]")) return;
      for (const key of AGENTS_FILTER_KEYS) agentsPageView.filters[key] = "";
      syncAgentsFilterUrl();
      renderAgentsPage();
    });
    $("[data-agents-filters]")?.addEventListener("change", (event) => {
      const key = event.target?.dataset?.agentsFilter;
      if (!AGENTS_FILTER_KEYS.includes(key)) return;
      agentsPageView.filters[key] = String(event.target.value || "").toLowerCase();
      syncAgentsFilterUrl();
      renderAgentsPage();
    });
    window.addEventListener("forkmesh:cloud-agents", () => {
      if (!agentsPageView.signedOut) void loadAgentsCloud();
    });
    window.addEventListener("forkmesh:desktop-agents", () => {
      if (!agentsPageView.signedOut) void loadAgentsDesktop();
    });
    void loadAgentsPage();
  }
